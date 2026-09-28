#include "support/llamad/fake_daemon.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace scry::test {
namespace {

namespace wire = ::llamad::v1;

// Unique per process and per daemon, and short: a Unix socket path must fit in
// sun_path, about a hundred bytes.
[[nodiscard]] std::filesystem::path unique_socket_path(const char* stem) {
  static std::atomic<unsigned> counter{0};
  const auto name = std::string{stem} + "-" + std::to_string(::getpid()) + "-" +
                    std::to_string(counter.fetch_add(1)) + ".sock";
  auto path = std::filesystem::temp_directory_path() / name;
  if (path.native().size() >= 100) {
    throw std::runtime_error{"fake llamad socket path is too long: " + path.native()};
  }
  return path;
}

void write_text(grpc::ServerWriter<wire::GenerateChunk>& writer,
                const std::string& text) {
  wire::GenerateChunk chunk;
  chunk.set_text(text);
  writer.Write(chunk);
}

[[nodiscard]] wire::GenerateChunk final_chunk(const FakeRound& round) {
  wire::GenerateChunk chunk;
  chunk.set_text(round.final_text);
  chunk.set_finish_reason(round.reason);
  chunk.mutable_stats()->set_prompt_tokens(round.prompt_tokens);
  chunk.mutable_stats()->set_completion_tokens(round.completion_tokens);
  for (const auto& call : round.tool_calls) {
    auto* encoded = chunk.add_tool_calls();
    encoded->set_id(call.id);
    encoded->set_name(call.name);
    encoded->set_arguments_json(call.arguments);
  }
  return chunk;
}

} // namespace

class FakeDaemon::Service final : public wire::Llama::Service {
public:
  explicit Service(std::vector<FakeRound> script) : script_(std::move(script)) {}

  grpc::Status Chat(grpc::ServerContext* context, const wire::ChatRequest* request,
                    grpc::ServerWriter<wire::GenerateChunk>* writer) override {
    const auto round = record(*request);
    for (const auto& text : round.text) {
      write_text(*writer, text);
    }
    if (round.hang) {
      return hang(*context);
    }
    if (round.failure) {
      return *round.failure;
    }
    if (round.end_without_final) {
      return grpc::Status::OK;
    }
    writer->Write(final_chunk(round));
    if (round.chunk_after_final) {
      write_text(*writer, "late");
    }
    return round.failure_after_final.value_or(grpc::Status::OK);
  }

  [[nodiscard]] std::vector<wire::ChatRequest> requests() const {
    const std::lock_guard lock{mutex_};
    return requests_;
  }

  // Releases any hanging round so the server can shut down.
  void stop() noexcept { stopping_.store(true); }

  std::atomic<bool> hanging{false};
  std::atomic<bool> observed_cancel{false};

private:
  [[nodiscard]] FakeRound record(const wire::ChatRequest& request) {
    const std::lock_guard lock{mutex_};
    requests_.push_back(request);
    return script_.at(std::min(requests_.size(), script_.size()) - 1);
  }

  grpc::Status hang(grpc::ServerContext& context) {
    hanging.store(true);
    while (!context.IsCancelled() && !stopping_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    observed_cancel.store(context.IsCancelled());
    hanging.store(false);
    return grpc::Status::CANCELLED;
  }

  mutable std::mutex mutex_;
  std::vector<FakeRound> script_;
  std::vector<wire::ChatRequest> requests_{};
  std::atomic<bool> stopping_{false};
};

FakeDaemon::FakeDaemon(std::vector<FakeRound> script)
    : socket_path_(unique_socket_path("scry-llamad")),
      target_("unix:" + socket_path_.native()),
      service_(std::make_unique<Service>(std::move(script))) {
  std::error_code ignored;
  std::filesystem::remove(socket_path_, ignored);
  grpc::ServerBuilder builder;
  builder.AddListeningPort(target_, grpc::InsecureServerCredentials());
  builder.RegisterService(service_.get());
  server_ = builder.BuildAndStart();
  if (!server_) {
    throw std::runtime_error{"fake llamad could not listen on " + target_};
  }
}

FakeDaemon::~FakeDaemon() {
  service_->stop();
  server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds{1});
  server_->Wait();
  std::error_code ignored;
  std::filesystem::remove(socket_path_, ignored);
}

std::vector<wire::ChatRequest> FakeDaemon::requests() const {
  return service_->requests();
}

std::size_t FakeDaemon::calls() const { return service_->requests().size(); }

bool FakeDaemon::hanging() const noexcept { return service_->hanging.load(); }

bool FakeDaemon::observed_cancel() const noexcept {
  return service_->observed_cancel.load();
}

std::string missing_socket_target() {
  return "unix:" + unique_socket_path("scry-llamad-missing").native();
}

} // namespace scry::test
