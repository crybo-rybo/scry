#include "runtime/config.hpp"
#include "runtime/startup.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <scry/config.hpp>
#include <scry/error.hpp>
#include <scry/harness.hpp>
#include <string>
#include <string_view>
#include <sys/un.h>
#include <system_error>
#include <utility>

namespace {

using namespace std::chrono_literals;
using scry::Config;

[[nodiscard]] Config valid_config() {
  return {
      .base_url = "https://example.test",
      .api_key = "test-key",
      .model = "model",
  };
}

[[nodiscard]] Config openai_config() {
  auto config = valid_config();
  config.dialect = scry::ProviderDialect::openai_compatible;
  return config;
}

[[nodiscard]] Config llamad_config() {
  return {
      .base_url = "unix:/run/user/1000/llamad.sock",
      .model = "local",
      .dialect = scry::ProviderDialect::llamad,
  };
}

constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();
constexpr double infinite = std::numeric_limits<double>::infinity();

constexpr std::string_view bad_url = "base_url must be an absolute HTTP or HTTPS URL";
constexpr std::string_view line_break_key = "api_key must contain no line breaks";
constexpr std::string_view anthropic_temperature =
    "Anthropic temperature must be finite and between 0 and 1";
constexpr std::string_view anthropic_top_p =
    "top_p must be finite, greater than 0, and at most 1";
constexpr std::string_view anthropic_max_tokens =
    "Anthropic max_tokens must be set and greater than 0; the Messages API requires it";
constexpr std::string_view openai_temperature =
    "OpenAI temperature must be finite and between 0 and 2";
constexpr std::string_view openai_top_p =
    "OpenAI top_p must be finite and between 0 and 1";
constexpr std::string_view bad_retry = "retry policy is invalid";
constexpr std::string_view bad_timeouts =
    "transport timeouts must be greater than 0 (transfer may be unset)";
constexpr std::string_view bad_header = "extra header name or value is invalid";
constexpr std::string_view header_collision =
    "extra header collides with a Scry-managed header";
constexpr std::string_view bad_path =
    "proxy or CA bundle path contains invalid characters";

struct Rejection {
  std::string_view name{};
  Config config{};
  std::string_view message{};
};

template <typename Mutate>
[[nodiscard]] Config with(Mutate mutate, Config config = valid_config()) {
  mutate(config);
  return config;
}

[[nodiscard]] Config with_header(std::string name, std::string value) {
  return with([&](Config& c) {
    c.extra_headers = {
        scry::HttpHeader{.name = std::move(name), .value = std::move(value)}};
  });
}

void require_rejected(const Config& config, const std::string_view message) {
  const auto status = scry::detail::validate_config(config);
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_config);
  CHECK(status.error().message == message);
}

constexpr std::string_view llamad_unavailable =
    "the llamad provider dialect requires a Scry build with SCRY_WITH_LLAMAD=ON";
constexpr std::string_view llamad_target =
    "llamad base_url must be a unix: gRPC target, such as "
    "unix:/run/user/1000/llamad.sock";
constexpr std::string_view llamad_temperature =
    "llamad temperature must be finite and between 0 and 2";
constexpr std::string_view llamad_top_p =
    "llamad top_p must be finite and between 0 and 1";
constexpr std::string_view llamad_max_tokens =
    "llamad max_tokens must be greater than 0 and at most 2147483647 when set";
constexpr std::string_view llamad_local_only =
    "llamad proxy and ca_bundle_path must be empty; the daemon is reached over a "
    "local socket";
constexpr std::string_view llamad_long_path =
    "llamad socket path must fit in sockaddr_un::sun_path with its terminator: "
    "107 bytes on Linux, 103 on macOS";

} // namespace

TEST_CASE("configuration accepts every valid shape") {
  const Config accepted[] = {
      valid_config(),
      with([](Config& c) { c.base_url = "http://localhost:8080/v1"; }),
      with([](Config& c) {
        c.extra_headers = {scry::HttpHeader{.name = "x-scry-example", .value = "1"}};
        c.proxy = "http://proxy.internal:3128";
        c.ca_bundle_path = "/etc/ssl/certs/corporate.pem";
      }),
      with_header("x-scry-example", "before\tafter"),
      // Unset is the default and means unlimited.
      with([](Config& c) { c.max_tool_calls_per_turn.reset(); }),
      with([](Config& c) { c.max_tool_calls_per_turn = 1; }),
      // An unset total transfer bound is the default.
      with([](Config& c) { c.timeouts.transfer = {}; }),
      // OpenAI-compatible local servers need no auth and take wider sampling bounds.
      with(
          [](Config& c) {
            c.api_key.clear();
            c.sampling.temperature = 2.0;
            c.sampling.top_p = 0.0;
          },
          openai_config()),
      with([](Config& c) { c.sampling.temperature = 1.5; }, openai_config()),
      with([](Config& c) { c.reasoning_mode = scry::ReasoningMode::disabled; },
           openai_config()),
      // An unset max_tokens is omitted from the request; the server default applies.
      with([](Config& c) { c.sampling.max_tokens.reset(); }, openai_config()),
      // Every seed value is the server's to interpret, including both extremes.
      with([](Config& c) { c.sampling.seed = 0; }, openai_config()),
      with(
          [](Config& c) {
            c.sampling.seed = std::numeric_limits<std::uint32_t>::max();
          },
          openai_config()),
  };
  for (const auto& config : accepted) {
    CHECK(scry::detail::validate_config(config));
  }
}

TEST_CASE("configuration rejects each invalid field with its own message") {
  const Rejection rejections[] = {
      // Endpoint and model.
      {"empty base_url", with([](Config& c) { c.base_url.clear(); }), bad_url},
      {"scheme only", with([](Config& c) { c.base_url = "https://"; }), bad_url},
      {"empty authority", with([](Config& c) { c.base_url = "https:///"; }), bad_url},
      {"non-HTTP scheme", with([](Config& c) { c.base_url = "ftp://example.test"; }),
       bad_url},
      {"query", with([](Config& c) { c.base_url = "https://example.test?version=1"; }),
       bad_url},
      {"fragment",
       with([](Config& c) { c.base_url = "https://example.test#fragment"; }), bad_url},
      {"space", with([](Config& c) { c.base_url = "https://example .test"; }), bad_url},
      {"empty model", with([](Config& c) { c.model.clear(); }),
       "model must not be empty"},
      // Auth.
      {"Anthropic without a key", with([](Config& c) { c.api_key.clear(); }),
       "Anthropic api_key must be present"},
      {"key with line breaks", with([](Config& c) { c.api_key = "unsafe\r\nheader"; }),
       line_break_key},
      {"OpenAI key with line breaks",
       with([](Config& c) { c.api_key = "unsafe\r\nheader"; }, openai_config()),
       line_break_key},
      // Reasoning and dialect. The enum casts reach the -Wreturn-type fallbacks.
      {"Anthropic reasoning disabled",
       with([](Config& c) { c.reasoning_mode = scry::ReasoningMode::disabled; }),
       "reasoning_mode = disabled requires the OpenAI-compatible provider dialect"},
      {"unknown reasoning mode", with([](Config& c) {
         c.reasoning_mode =
             static_cast<scry::ReasoningMode>(std::numeric_limits<std::uint8_t>::max());
       }),
       "reasoning_mode is invalid"},
      {"unknown dialect", with([](Config& c) {
         c.dialect = static_cast<scry::ProviderDialect>(
             std::numeric_limits<std::uint8_t>::max());
       }),
       "the configured provider dialect is not available"},
      // Anthropic sampling.
      {"Anthropic temperature NaN",
       with([](Config& c) { c.sampling.temperature = not_a_number; }),
       anthropic_temperature},
      {"Anthropic temperature infinite",
       with([](Config& c) { c.sampling.temperature = infinite; }),
       anthropic_temperature},
      {"Anthropic temperature negative",
       with([](Config& c) { c.sampling.temperature = -0.1; }), anthropic_temperature},
      {"Anthropic temperature above 1",
       with([](Config& c) { c.sampling.temperature = 1.01; }), anthropic_temperature},
      {"Anthropic top_p NaN", with([](Config& c) { c.sampling.top_p = not_a_number; }),
       anthropic_top_p},
      {"Anthropic top_p zero", with([](Config& c) { c.sampling.top_p = 0.0; }),
       anthropic_top_p},
      {"Anthropic top_p negative", with([](Config& c) { c.sampling.top_p = -0.1; }),
       anthropic_top_p},
      {"Anthropic top_p above 1", with([](Config& c) { c.sampling.top_p = 1.1; }),
       anthropic_top_p},
      {"Anthropic max_tokens unset",
       with([](Config& c) { c.sampling.max_tokens.reset(); }), anthropic_max_tokens},
      {"Anthropic max_tokens zero", with([](Config& c) { c.sampling.max_tokens = 0; }),
       anthropic_max_tokens},
      {"Anthropic seed", with([](Config& c) { c.sampling.seed = 42; }),
       "the Anthropic Messages API has no seed; use the OpenAI-compatible or llamad "
       "dialect"},
      // OpenAI-compatible sampling.
      {"OpenAI temperature NaN",
       with([](Config& c) { c.sampling.temperature = not_a_number; }, openai_config()),
       openai_temperature},
      {"OpenAI temperature negative",
       with([](Config& c) { c.sampling.temperature = -0.01; }, openai_config()),
       openai_temperature},
      {"OpenAI temperature above 2",
       with([](Config& c) { c.sampling.temperature = 2.01; }, openai_config()),
       openai_temperature},
      {"OpenAI top_p NaN",
       with([](Config& c) { c.sampling.top_p = not_a_number; }, openai_config()),
       openai_top_p},
      {"OpenAI top_p negative",
       with([](Config& c) { c.sampling.top_p = -0.01; }, openai_config()),
       openai_top_p},
      {"OpenAI top_p above 1",
       with([](Config& c) { c.sampling.top_p = 1.01; }, openai_config()), openai_top_p},
      {"OpenAI max_tokens zero",
       with([](Config& c) { c.sampling.max_tokens = 0; }, openai_config()),
       "OpenAI max_tokens must be greater than 0 when set"},
      // Retry policy.
      {"zero attempts", with([](Config& c) { c.retry.max_attempts = 0; }), bad_retry},
      {"initial above max backoff",
       with([](Config& c) { c.retry.initial_backoff = c.retry.max_backoff + 1ms; }),
       bad_retry},
      {"negative initial backoff",
       with([](Config& c) { c.retry.initial_backoff = -1ms; }), bad_retry},
      {"negative max backoff", with([](Config& c) { c.retry.max_backoff = -1ms; }),
       bad_retry},
      {"negative max elapsed", with([](Config& c) { c.retry.max_elapsed = -1ms; }),
       bad_retry},
      {"infinite jitter", with([](Config& c) { c.retry.jitter_ratio = infinite; }),
       bad_retry},
      {"negative jitter", with([](Config& c) { c.retry.jitter_ratio = -0.1; }),
       bad_retry},
      {"jitter above 1", with([](Config& c) { c.retry.jitter_ratio = 1.1; }),
       bad_retry},
      // Runtime bounds.
      {"zero connect timeout", with([](Config& c) { c.timeouts.connect = {}; }),
       bad_timeouts},
      {"zero idle timeout", with([](Config& c) { c.timeouts.idle = {}; }),
       bad_timeouts},
      {"zero shutdown timeout", with([](Config& c) { c.timeouts.shutdown = {}; }),
       bad_timeouts},
      {"zero transfer timeout", with([](Config& c) { c.timeouts.transfer = 0ms; }),
       bad_timeouts},
      {"negative transfer timeout", with([](Config& c) { c.timeouts.transfer = -1ms; }),
       bad_timeouts},
      {"undersized queued-event limit",
       with([](Config& c) { c.limits.max_queued_event_bytes_per_turn = 1023; }),
       "per-turn queued-event limit must be at least 1024 bytes"},
      {"zero tool rounds", with([](Config& c) { c.max_tool_rounds = 0; }),
       "max_tool_rounds must be greater than 0"},
      {"zero tool calls", with([](Config& c) { c.max_tool_calls_per_turn = 0; }),
       "max_tool_calls_per_turn must be greater than 0 when set"},
      // Network options.
      {"empty header name", with_header("", "1"), bad_header},
      {"header name with a space", with_header("x scry", "1"), bad_header},
      {"header value injection", with_header("x-scry-example", "1\r\nx-evil: 2"),
       bad_header},
      // Every control byte but tab is rejected, not only CR and LF: curl takes the
      // value as a C string, so an embedded NUL would otherwise be sent as a
      // silently truncated header.
      {"header value with NUL",
       with_header("x-scry-example", std::string{"before\0after", 12}), bad_header},
      {"header value with SOH",
       with_header("x-scry-example", "before\x01"
                                     "after"),
       bad_header},
      {"header value with DEL",
       with_header("x-scry-example", "before\x7f"
                                     "after"),
       bad_header},
      {"managed header", with_header("Content-Type", "text/plain"), header_collision},
      {"managed key header", with_header("X-Api-Key", "other"), header_collision},
      {"proxy with a space", with([](Config& c) { c.proxy = "http://a b"; }), bad_path},
      {"CA bundle with a line break",
       with([](Config& c) { c.ca_bundle_path = "/etc/ssl/ca\n.pem"; }), bad_path},
  };
  for (const auto& rejection : rejections) {
    INFO(rejection.name);
    require_rejected(rejection.config, rejection.message);
  }
}

TEST_CASE("configuration rejects a zero value for every resource limit") {
  constexpr std::array limits{
      &scry::ResourceLimits::max_pending_turns,
      &scry::ResourceLimits::max_sse_event_bytes,
      &scry::ResourceLimits::max_response_bytes,
      &scry::ResourceLimits::max_tool_arguments_bytes,
      &scry::ResourceLimits::max_tool_result_bytes,
      &scry::ResourceLimits::max_queued_event_bytes_per_turn,
      &scry::ResourceLimits::max_conversation_bytes,
  };
  for (const auto member : limits) {
    auto config = valid_config();
    config.limits.*member = 0;
    require_rejected(config, "resource limits must be greater than 0");
  }
}

TEST_CASE("Harness::validate runs the create-time configuration checks") {
  CHECK(scry::Harness::validate(valid_config()));

  const auto rejected =
      scry::Harness::validate(with([](Config& c) { c.model.clear(); }));
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().category == scry::ErrorCategory::invalid_config);
  CHECK(rejected.error().message == "model must not be empty");

  CHECK_FALSE(scry::Harness::validate(Config{}));
}

TEST_CASE("an HTTP dialect refuses a unix: gRPC target") {
  require_rejected(
      with([](Config& c) { c.base_url = "unix:/run/user/1000/llamad.sock"; }), bad_url);
  require_rejected(
      with([](Config& c) { c.base_url = "unix:/tmp/llamad.sock"; }, openai_config()),
      bad_url);
}

#if SCRY_WITH_LLAMAD

namespace {

// The longest socket path this platform can bind: sun_path less its terminator,
// 107 bytes on Linux and 103 on macOS.
constexpr std::size_t max_socket_path = sizeof(sockaddr_un{}.sun_path) - 1;

// A `unix:` target whose absolute socket path is exactly `bytes` long.
[[nodiscard]] std::string unix_target_of_length(const std::size_t bytes) {
  return "unix:/" + std::string(bytes - 1, 's');
}

} // namespace

TEST_CASE("a build with the llamad backend accepts every valid llamad shape") {
  const Config accepted[] = {
      llamad_config(),
      with([](Config& c) { c.base_url = "unix:///tmp/llamad.sock"; }, llamad_config()),
      with([](Config& c) { c.base_url = "unix:relative/llamad.sock"; },
           llamad_config()),
      with([](Config& c) { c.base_url = "unix:///abs.sock"; }, llamad_config()),
      with([](Config& c) { c.base_url = unix_target_of_length(max_socket_path); },
           llamad_config()),
      // 0 is greedy decoding on llamad.
      with([](Config& c) { c.sampling.temperature = 0.0; }, llamad_config()),
      with([](Config& c) { c.sampling.temperature = 2.0; }, llamad_config()),
      with([](Config& c) { c.sampling.top_p = 0.0; }, llamad_config()),
      with([](Config& c) { c.sampling.top_p = 1.0; }, llamad_config()),
      with([](Config& c) { c.sampling.max_tokens.reset(); }, llamad_config()),
      with([](Config& c) { c.sampling.max_tokens = 2'147'483'647U; }, llamad_config()),
      with(
          [](Config& c) {
            c.sampling.seed = std::numeric_limits<std::uint32_t>::max();
          },
          llamad_config()),
  };
  for (const auto& config : accepted) {
    CHECK(scry::detail::validate_config(config));
    CHECK(scry::Harness::validate(config));
  }
}

TEST_CASE("a build with the llamad backend rejects each invalid llamad field") {
  const Rejection rejections[] = {
      {"empty target", with([](Config& c) { c.base_url.clear(); }, llamad_config()),
       llamad_target},
      {"scheme only", with([](Config& c) { c.base_url = "unix:"; }, llamad_config()),
       llamad_target},
      {"URI form without a path",
       with([](Config& c) { c.base_url = "unix://"; }, llamad_config()), llamad_target},
      {"URI form with an authority",
       with([](Config& c) { c.base_url = "unix://host/llamad.sock"; }, llamad_config()),
       llamad_target},
      {"path longer than sun_path",
       with([](Config& c) { c.base_url = unix_target_of_length(max_socket_path + 1); },
            llamad_config()),
       llamad_long_path},
      {"HTTP URL",
       with([](Config& c) { c.base_url = "http://127.0.0.1:8080"; }, llamad_config()),
       llamad_target},
      {"bare path",
       with([](Config& c) { c.base_url = "/run/llamad.sock"; }, llamad_config()),
       llamad_target},
      {"line break",
       with([](Config& c) { c.base_url = "unix:/tmp/a\nb.sock"; }, llamad_config()),
       llamad_target},
      {"empty model", with([](Config& c) { c.model.clear(); }, llamad_config()),
       "model must not be empty"},
      {"api key", with([](Config& c) { c.api_key = "secret"; }, llamad_config()),
       "llamad api_key must be empty; the daemon takes no credential"},
      {"reasoning disabled",
       with([](Config& c) { c.reasoning_mode = scry::ReasoningMode::disabled; },
            llamad_config()),
       "reasoning_mode = disabled requires the OpenAI-compatible provider dialect"},
      {"temperature NaN",
       with([](Config& c) { c.sampling.temperature = not_a_number; }, llamad_config()),
       llamad_temperature},
      {"temperature negative",
       with([](Config& c) { c.sampling.temperature = -0.01; }, llamad_config()),
       llamad_temperature},
      {"temperature above 2",
       with([](Config& c) { c.sampling.temperature = 2.01; }, llamad_config()),
       llamad_temperature},
      {"top_p infinite",
       with([](Config& c) { c.sampling.top_p = infinite; }, llamad_config()),
       llamad_top_p},
      {"top_p negative",
       with([](Config& c) { c.sampling.top_p = -0.01; }, llamad_config()),
       llamad_top_p},
      {"top_p above 1",
       with([](Config& c) { c.sampling.top_p = 1.01; }, llamad_config()), llamad_top_p},
      {"max_tokens zero",
       with([](Config& c) { c.sampling.max_tokens = 0; }, llamad_config()),
       llamad_max_tokens},
      {"max_tokens above int32",
       with([](Config& c) { c.sampling.max_tokens = 2'147'483'648U; }, llamad_config()),
       llamad_max_tokens},
      {"extra header",
       with([](Config& c) { c.extra_headers = {{"x-scry", "1"}}; }, llamad_config()),
       "llamad extra_headers must be empty; the daemon takes no headers"},
      {"proxy",
       with([](Config& c) { c.proxy = "http://proxy.internal:3128"; }, llamad_config()),
       llamad_local_only},
      {"CA bundle",
       with([](Config& c) { c.ca_bundle_path = "/etc/ssl/ca.pem"; }, llamad_config()),
       llamad_local_only},
      {"TLS verification disabled",
       with([](Config& c) { c.tls_verify_peer = false; }, llamad_config()),
       "llamad tls_verify_peer must stay true; the daemon is reached over a local "
       "socket without TLS"},
      {"zero idle timeout",
       with([](Config& c) { c.timeouts.idle = {}; }, llamad_config()), bad_timeouts},
  };
  for (const auto& rejection : rejections) {
    INFO(rejection.name);
    require_rejected(rejection.config, rejection.message);
    const auto validated = scry::Harness::validate(rejection.config);
    REQUIRE_FALSE(validated);
    CHECK(validated.error().message == rejection.message);
  }
}

#else

TEST_CASE("a build without the llamad backend rejects the llamad dialect") {
  // Every llamad field is valid here, so only the missing build option can refuse it,
  // and it is named before any field is read.
  require_rejected(llamad_config(), llamad_unavailable);
  require_rejected(
      with([](Config& c) { c.base_url = "http://127.0.0.1:8080"; }, llamad_config()),
      llamad_unavailable);

  const auto validated = scry::Harness::validate(llamad_config());
  REQUIRE_FALSE(validated);
  CHECK(validated.error().category == scry::ErrorCategory::invalid_config);
  CHECK(validated.error().message == llamad_unavailable);

  const auto created = scry::Harness::create(llamad_config());
  REQUIRE_FALSE(created);
  CHECK(created.error().category == scry::ErrorCategory::invalid_config);
  CHECK(created.error().message == llamad_unavailable);
}

#endif

TEST_CASE(
    "worker thread startup failures are translated without hiding allocation failure") {
  const auto unavailable = scry::detail::translate_worker_start_failure<int>([] {
    throw std::system_error{
        std::make_error_code(std::errc::resource_unavailable_try_again)};
    return 0;
  });
  REQUIRE_FALSE(unavailable);
  CHECK(unavailable.error().category == scry::ErrorCategory::resource_limit);
  CHECK(unavailable.error().message == "Harness worker thread could not be started");

  CHECK_THROWS_AS(scry::detail::translate_worker_start_failure<int>([] {
                    throw std::bad_alloc{};
                    return 0;
                  }),
                  std::bad_alloc);
}
