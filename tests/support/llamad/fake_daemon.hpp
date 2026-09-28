#pragma once

// A scripted stand-in for the llamad daemon: a real gRPC server implementing
// llamad.v1.Llama/Chat on a private Unix socket, replaying canned rounds. No
// model and no inference, so what is under test is the request the backend
// sends and what it makes of the stream the daemon promises (text chunks, then
// one final chunk) or of the ways a stream can break that promise.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <grpcpp/grpcpp.h>
#include <llamad/v1/llamad.grpc.pb.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace scry::test {

struct FakeToolCall {
  std::string id{};
  std::string name{};
  std::string arguments{};
};

// One Chat call's reply. By default: each `text` entry as its own chunk, then
// a final chunk carrying `final_text`, `reason`, the stats, and `tool_calls`.
struct FakeRound {
  std::vector<std::string> text{};
  ::llamad::v1::FinishReason reason{::llamad::v1::FINISH_REASON_EOG};
  std::vector<FakeToolCall> tool_calls{};
  std::int32_t prompt_tokens{};
  std::int32_t completion_tokens{};
  std::string final_text{};
  // After the text, wait for the client to cancel instead of finishing: a
  // daemon still prefilling, queued behind another client, or wedged.
  bool hang{false};
  // After the text, end the call with this status instead of a final chunk.
  std::optional<grpc::Status> failure{};
  // After the text, end the stream with OK and no final chunk.
  bool end_without_final{false};
  // After the final chunk, send one more text chunk.
  bool chunk_after_final{false};
  // After the final chunk, end the call with this status instead of OK.
  std::optional<grpc::Status> failure_after_final{};
};

class FakeDaemon {
public:
  // A script shorter than the calls made replays its last round.
  explicit FakeDaemon(std::vector<FakeRound> script);
  ~FakeDaemon();

  FakeDaemon(const FakeDaemon&) = delete;
  FakeDaemon& operator=(const FakeDaemon&) = delete;

  // The gRPC target to put in Config::base_url.
  [[nodiscard]] const std::string& target() const noexcept { return target_; }
  // Every ChatRequest received so far, in arrival order.
  [[nodiscard]] std::vector<::llamad::v1::ChatRequest> requests() const;
  [[nodiscard]] std::size_t calls() const;
  // Whether a hanging round is waiting for its cancel right now.
  [[nodiscard]] bool hanging() const noexcept;
  // Whether a hanging round has seen the client cancel it.
  [[nodiscard]] bool observed_cancel() const noexcept;

private:
  class Service;

  std::filesystem::path socket_path_{};
  std::string target_{};
  std::unique_ptr<Service> service_{};
  std::unique_ptr<grpc::Server> server_{};
};

// A socket path under the temporary directory that nothing listens on.
[[nodiscard]] std::string missing_socket_target();

} // namespace scry::test
