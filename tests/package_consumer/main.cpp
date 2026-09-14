// Downstream smoke for the installed package: the explicit-schema surface and
// the reflected surface must both be usable through scry::scry alone.
#include <scry/config.hpp>
#include <scry/error.hpp>
#include <scry/harness.hpp>
#include <scry/reflection.hpp>
#include <scry/scry.hpp>
#include <scry/version.hpp>
#if defined(SCRY_CONSUMER_HAS_TESTING)
#include <scry/testing/scripted_transport.hpp>
#include <scry/testing/streams.hpp>
#endif
#include <string_view>
#include <utility>

namespace {

struct PackageArguments {
  bool ready{};
};

[[nodiscard]] bool encode_smoke() {
  const auto encoded = scry::reflection::encode(PackageArguments{.ready = true});
  return encoded && encoded->text == R"({"ready":true})";
}

#if defined(SCRY_CONSUMER_HAS_TESTING)
// The optional testing component must drive a whole turn from the installed
// package alone: headers, library, and the runtime underneath all of it.
[[nodiscard]] bool scripted_turn_smoke() {
  scry::testing::ScriptedTransport transport;
  transport.enqueue({
      .request_id = "package-smoke",
      .body_chunks = {scry::testing::anthropic_text_stream("installed")},
  });
  auto config = scry::Config{
      .base_url = "http://127.0.0.1:1",
      .api_key = "package-smoke",
      .model = "package-smoke",
  };
  config.retry.max_attempts = 1;
  auto created = scry::testing::create_harness(std::move(config), transport);
  if (!created) {
    return false;
  }
  auto conversation = scry::Conversation::create();
  if (!conversation) {
    return false;
  }
  const auto completion = created->send_and_wait(*conversation, "Question");
  return completion && completion->text == "installed" && transport.calls() == 1;
}
#endif

} // namespace

int main() {
  using namespace std::literals;

  // <scry/version.hpp> is generated into the build tree and installed from
  // there; reading it here makes a missing generated header a compile error.
  static_assert(scry::version_major == 0);

  static_assert(
      scry::reflection::input_schema_v<PackageArguments> ==
      R"({"additionalProperties":false,"properties":{"ready":{"type":"boolean"}},"required":[],"type":"object"})"sv);

  const auto conversation = scry::Conversation::create();
  if (!conversation) {
    return 1;
  }

  const auto rejected = scry::Harness::create(scry::Config{
      .base_url = "http://localhost:8080",
      .model = "package-smoke",
  });
  if (rejected || rejected.error().category != scry::ErrorCategory::invalid_config) {
    return 2;
  }

  auto created = scry::Harness::create(scry::Config{
      .base_url = "https://api.anthropic.com",
      .api_key = "package-smoke",
      .model = "package-smoke",
  });
  if (!created) {
    return 3;
  }
  auto harness = std::move(*created);

  const auto registration = scry::reflection::add<PackageArguments>(
      harness.tools(),
      {
          .name = "package_smoke",
          .description = "Prove the installed reflected API is linkable",
      },
      [](PackageArguments arguments) { return arguments.ready; });

  if (!registration || harness.tools().size() != 1 || !encode_smoke()) {
    return 4;
  }

#if defined(SCRY_CONSUMER_HAS_TESTING)
  if (!scripted_turn_smoke()) {
    return 5;
  }
#endif

  return 0;
}
