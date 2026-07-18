#include <chrono>
#include <iostream>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <utility>

using namespace std::chrono_literals;

namespace {

[[nodiscard]] int verify_failure(const scry::Result<scry::Completion>& result,
                                 const scry::Conversation& conversation) {
  if (result || result.error().category != scry::ErrorCategory::protocol ||
      result.error().attempt != 1 || !conversation.empty()) {
    std::cerr << "default TLS verification did not reject exactly once\n";
    return 1;
  }
  return 0;
}

[[nodiscard]] int verify_success(const scry::Result<scry::Completion>& result,
                                 const scry::Conversation& conversation) {
  if (!result || result->text != "TLS mock" || result->attempt_count != 1 ||
      conversation.message_count() != 2) {
    std::cerr << "explicit TLS opt-out did not complete the mock turn\n";
    return 1;
  }
  return 0;
}

} // namespace

int main(const int argc, char** argv) {
  if (argc != 3) {
    return 2;
  }
  const auto mode = std::string_view{argv[2]};
  const auto insecure = mode == "insecure";
  if (!insecure && mode != "verify") {
    return 2;
  }

  auto config = scry::Config{
      .base_url = argv[1],
      .api_key = "tls-test-key",
      .model = "tls-test-model",
      .tls_verify_peer = !insecure,
  };
  config.retry.initial_backoff = 0ms;
  config.retry.max_backoff = 0ms;
  config.timeouts.connect = 1s;
  config.timeouts.transfer = 2s;
  config.timeouts.shutdown = 50ms;

  auto harness = scry::Harness::create(std::move(config));
  auto conversation = scry::Conversation::create();
  if (!harness || !conversation) {
    return 2;
  }
  const auto result = harness->send_and_wait(*conversation, "TLS verification");
  return insecure ? verify_success(result, *conversation)
                  : verify_failure(result, *conversation);
}
