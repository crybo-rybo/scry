// Downstream smoke for the installed package: the explicit-schema surface and
// the reflected surface must both be usable through scry::scry alone.
#include <scry/config.hpp>
#include <scry/error.hpp>
#include <scry/harness.hpp>
#include <scry/reflection.hpp>
#include <scry/scry.hpp>
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

} // namespace

int main() {
  using namespace std::literals;

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

  return registration && harness.tools().size() == 1 && encode_smoke() ? 0 : 4;
}
