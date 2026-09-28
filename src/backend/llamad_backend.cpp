#include "backend/llamad_backend.hpp"

#include "backend/llamad_wire.hpp"
#include "core/error.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <grpcpp/grpcpp.h>
#include <limits>
#include <llamad/v1/llamad.grpc.pb.h>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>

namespace scry::detail {
namespace {

namespace wire = ::llamad::v1;
using Clock = std::chrono::steady_clock;
using llamad_wire::llamad_error;
using std::chrono::milliseconds;

// Far enough out to mean "no bound", near enough that adding it to any clock
// reading cannot overflow the clock's nanosecond count. Every configured
// duration passes through this before it is added to a time point, as the curl
// transport clamps its durations to what libcurl holds.
constexpr milliseconds longest_wait =
    std::chrono::duration_cast<milliseconds>(std::chrono::years{100});

[[nodiscard]] constexpr milliseconds saturated(const milliseconds value) noexcept {
  return std::min(value, longest_wait);
}

// What ended an attempt from this side of the wire before the daemon did.
enum class Abort : std::uint8_t { none, cancelled, shutdown, idle, local };

// The per-attempt inputs every wait consults: how long to wait, and the two
// signals that end the attempt early.
struct AttemptControl {
  const TransportTimeouts& timeouts;
  const std::stop_token& shutdown;
  const std::atomic<bool>& cancelled;
};

[[nodiscard]] Abort requested_abort(const AttemptControl& control) noexcept {
  if (control.shutdown.stop_requested()) {
    return Abort::shutdown;
  }
  if (control.cancelled.load(std::memory_order_acquire)) {
    return Abort::cancelled;
  }
  return Abort::none;
}

// The texts match the curl transport's, so a host sees one wording for a
// cancelled attempt whichever backend it runs.
[[nodiscard]] Error abort_error(const Abort cause) {
  if (cause == Abort::shutdown) {
    return make_error(ErrorCategory::cancelled,
                      "transfer cancelled by harness shutdown");
  }
  if (cause == Abort::idle) {
    return llamad_error(ErrorCategory::network, "transfer timed out", "idle_timeout");
  }
  return make_error(ErrorCategory::cancelled, "transfer cancelled");
}

// gRPC status codes are 0 to 16; anything else reads as unknown.
[[nodiscard]] std::string_view status_token(const grpc::StatusCode code) noexcept {
  constexpr std::array<std::string_view, 17> names{
      "ok",
      "cancelled",
      "unknown",
      "invalid_argument",
      "deadline_exceeded",
      "not_found",
      "already_exists",
      "permission_denied",
      "resource_exhausted",
      "failed_precondition",
      "aborted",
      "out_of_range",
      "unimplemented",
      "internal",
      "unavailable",
      "data_loss",
      "unauthenticated",
  };
  const auto index = static_cast<std::size_t>(code);
  return index < names.size() ? names.at(index) : std::string_view{"unknown"};
}

// Only fixed texts reach Error::message: the daemon's error_message can carry
// prompt or engine text, so it is dropped, and the code survives as a token in
// provider_detail. A daemon-side failure (INTERNAL, UNKNOWN, a cancel the
// daemon made) is network, retried before output like an HTTP 5xx. A request
// the daemon refuses, a prompt too long for its context among them, is
// protocol, as an HTTP 4xx is. gRPC reports an oversized message as
// RESOURCE_EXHAUSTED, and resending the same request cannot fit it, so that
// code is a resource limit rather than a rate limit.
[[nodiscard]] Error status_error(const grpc::Status& status) {
  const auto code = status.error_code();
  const auto token = status_token(code);
  switch (code) {
  case grpc::StatusCode::UNAVAILABLE:
    return llamad_error(ErrorCategory::network, "llamad daemon is unavailable", token);
  case grpc::StatusCode::DEADLINE_EXCEEDED:
    return llamad_error(ErrorCategory::network, "transfer timed out", token);
  case grpc::StatusCode::ABORTED:
    return llamad_error(ErrorCategory::network, "llamad aborted the call", token);
  case grpc::StatusCode::RESOURCE_EXHAUSTED:
    return llamad_error(ErrorCategory::resource_limit,
                        "llamad call exceeded a gRPC message size limit", token);
  case grpc::StatusCode::INVALID_ARGUMENT:
  case grpc::StatusCode::FAILED_PRECONDITION:
  case grpc::StatusCode::UNIMPLEMENTED:
    return llamad_error(ErrorCategory::protocol, "llamad rejected the request", token);
  case grpc::StatusCode::CANCELLED:
    return llamad_error(ErrorCategory::network, "llamad cancelled the call", token);
  default:
    return llamad_error(ErrorCategory::network, "llamad call failed", token);
  }
}

// Waits for the channel to reach READY, polling in slices of the shutdown bound
// so a cancel or a Harness shutdown is seen promptly. A socket nobody listens on
// puts the channel in TRANSIENT_FAILURE at once, and so does a listener that
// does not finish the handshake within gRPC's own per-attempt bound (its minimum
// reconnect backoff); either fails the attempt at once, as a refused connection
// does over HTTP. The connect timeout bounds only a channel that stays in its
// connecting state that long.
[[nodiscard]] Status wait_for_channel(grpc::Channel& channel,
                                      const AttemptControl& control) {
  const auto deadline = Clock::now() + saturated(control.timeouts.connect);
  auto state = channel.GetState(true);
  while (state != GRPC_CHANNEL_READY) {
    if (state == GRPC_CHANNEL_TRANSIENT_FAILURE || state == GRPC_CHANNEL_SHUTDOWN) {
      return std::unexpected(llamad_error(
          ErrorCategory::network, "llamad daemon is unreachable", "unavailable"));
    }
    if (const auto cause = requested_abort(control); cause != Abort::none) {
      return std::unexpected(abort_error(cause));
    }
    const auto now = Clock::now();
    if (now >= deadline) {
      return std::unexpected(llamad_error(
          ErrorCategory::network, "llamad connection timed out", "connect_timeout"));
    }
    const auto slice =
        std::min(std::chrono::duration_cast<milliseconds>(deadline - now),
                 saturated(control.timeouts.shutdown));
    static_cast<void>(channel.WaitForStateChange(
        state, std::chrono::system_clock::now() + std::max(slice, milliseconds{1})));
    state = channel.GetState(true);
  }
  return {};
}

// One Chat call on its own completion queue. At most one operation is ever
// outstanding - start, then each read, then finish - and every one is awaited
// before the next starts, so when run() returns no tag is pending and the
// queue only needs shutting down and draining.
class ChatCall {
public:
  explicit ChatCall(const AttemptControl& control)
      : control_(control), last_activity_(Clock::now()) {
    context_.set_wait_for_ready(false);
    if (control.timeouts.transfer) {
      context_.set_deadline(std::chrono::system_clock::now() +
                            saturated(*control.timeouts.transfer));
    }
  }

  ~ChatCall() {
    queue_.Shutdown();
    void* tag = nullptr;
    bool ok = false;
    while (queue_.Next(&tag, &ok)) {
    }
  }

  ChatCall(const ChatCall&) = delete;
  ChatCall& operator=(const ChatCall&) = delete;

  [[nodiscard]] Result<ModelResponse> run(wire::Llama::Stub& stub,
                                          const wire::ChatRequest& request,
                                          llamad_wire::StreamDecoder& decoder) {
    reader_ = stub.PrepareAsyncChat(&context_, request, &queue_);
    reader_->StartCall(tag());
    auto streaming = await();
    while (streaming) {
      reader_->Read(&chunk_, tag());
      streaming = await();
      if (streaming) {
        consume(decoder);
      }
    }
    reader_->Finish(&status_, tag());
    static_cast<void>(await());
    return outcome(decoder);
  }

private:
  // Operations never overlap, so one tag names whichever is outstanding. gRPC
  // hands it back unchanged and never dereferences it.
  [[nodiscard]] void* tag() noexcept { return this; }

  // After a cancel the remaining chunks are drained unread, so the read loop
  // still ends the way gRPC expects before Finish.
  void consume(llamad_wire::StreamDecoder& decoder) {
    if (abort_ != Abort::none) {
      return;
    }
    last_activity_ = Clock::now();
    if (auto status = decoder.consume(chunk_); !status) {
      local_error_ = std::move(status.error());
      cancel(Abort::local);
    }
  }

  void cancel(const Abort cause) {
    abort_ = cause;
    context_.TryCancel();
  }

  // Cancels the call once when the turn is cancelled, the Harness shuts down,
  // or the stream has been silent for the idle bound. The silence is compared
  // in milliseconds, so a huge bound is never scaled up to nanoseconds.
  void check_abort() {
    if (abort_ != Abort::none) {
      return;
    }
    auto cause = requested_abort(control_);
    if (cause == Abort::none && silence() >= control_.timeouts.idle) {
      cause = Abort::idle;
    }
    if (cause != Abort::none) {
      cancel(cause);
    }
  }

  [[nodiscard]] milliseconds silence() const {
    return std::chrono::duration_cast<milliseconds>(Clock::now() - last_activity_);
  }

  // Until the call is cancelled, a wait lasts no longer than the idle bound
  // still has to run, nor longer than the shutdown poll bound.
  [[nodiscard]] milliseconds next_wait() const {
    const auto poll = saturated(control_.timeouts.shutdown);
    if (abort_ != Abort::none) {
      return poll;
    }
    return std::clamp(control_.timeouts.idle - silence(), milliseconds{1}, poll);
  }

  // Waits for the one outstanding operation and returns its ok flag. A
  // cancelled call completes it promptly, so this never abandons a tag.
  [[nodiscard]] bool await() {
    while (true) {
      check_abort();
      void* completed = nullptr;
      bool ok = false;
      const auto status = queue_.AsyncNext(
          &completed, &ok, std::chrono::system_clock::now() + next_wait());
      if (status == grpc::CompletionQueue::GOT_EVENT) {
        return ok;
      }
      if (status == grpc::CompletionQueue::SHUTDOWN) {
        // Only the destructor shuts the queue down, after run() has returned,
        // so no wait can see it. Should one ever, ending the stream is the
        // one safe answer: waiting again would spin.
        assert(false && "llamad completion queue shut down during a call");
        return false;
      }
    }
  }

  [[nodiscard]] Result<ModelResponse> outcome(llamad_wire::StreamDecoder& decoder) {
    if (local_error_) {
      return std::unexpected(std::move(*local_error_));
    }
    if (abort_ != Abort::none) {
      return std::unexpected(abort_error(abort_));
    }
    if (!status_.ok()) {
      return std::unexpected(status_error(status_));
    }
    return decoder.finish();
  }

  const AttemptControl& control_;
  grpc::ClientContext context_{};
  grpc::CompletionQueue queue_{};
  std::unique_ptr<grpc::ClientAsyncReader<wire::GenerateChunk>> reader_{};
  wire::GenerateChunk chunk_{};
  grpc::Status status_{};
  Clock::time_point last_activity_;
  std::optional<Error> local_error_{};
  Abort abort_{Abort::none};
};

[[nodiscard]] int receive_limit(const std::size_t max_response_bytes) noexcept {
  constexpr auto maximum = static_cast<std::size_t>(std::numeric_limits<int>::max());
  return static_cast<int>(std::min(max_response_bytes, maximum));
}

class LlamadBackend final : public ModelBackend {
public:
  explicit LlamadBackend(const Config& config) {
    grpc::ChannelArguments arguments;
    // The daemon is either on the socket or it is not, so reconnects stay
    // quick: a daemon that comes back is found within a second, not after
    // gRPC's default two-minute backoff ceiling.
    arguments.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);
    arguments.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 100);
    arguments.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);
    arguments.SetMaxReceiveMessageSize(receive_limit(config.limits.max_response_bytes));
    channel_ = grpc::CreateCustomChannel(config.base_url,
                                         grpc::InsecureChannelCredentials(), arguments);
    stub_ = wire::Llama::NewStub(channel_);
  }

  [[nodiscard]] Result<ModelResponse>
  perform(const Config& config, const ModelRequest& request, std::stop_token shutdown,
          const std::atomic<bool>& cancelled, ProviderEventSink& sink) override {
    const AttemptControl control{
        .timeouts = config.timeouts, .shutdown = shutdown, .cancelled = cancelled};
    if (const auto cause = requested_abort(control); cause != Abort::none) {
      return std::unexpected(abort_error(cause));
    }
    wire::ChatRequest wire_request;
    if (auto status = llamad_wire::encode_chat_request(request, wire_request);
        !status) {
      return std::unexpected(std::move(status.error()));
    }
    if (auto status = wait_for_channel(*channel_, control); !status) {
      return std::unexpected(std::move(status.error()));
    }
    llamad_wire::StreamDecoder decoder{config.limits, sink};
    ChatCall call{control};
    return call.run(*stub_, wire_request, decoder);
  }

private:
  std::shared_ptr<grpc::Channel> channel_{};
  std::unique_ptr<wire::Llama::Stub> stub_{};
};

} // namespace

std::unique_ptr<ModelBackend> make_llamad_backend(const Config& config) {
  return std::make_unique<LlamadBackend>(config);
}

} // namespace scry::detail
