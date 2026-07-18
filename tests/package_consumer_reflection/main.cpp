#include <scry/config.hpp>
#include <scry/harness.hpp>
#include <scry/reflection.hpp>
#include <string_view>
#include <utility>

namespace {

struct PackageArguments {
  bool ready{};
};

} // namespace

int main() {
  using namespace std::literals;

  static_assert(
      scry::reflection::input_schema_v<PackageArguments> ==
      R"({"additionalProperties":false,"properties":{"ready":{"type":"boolean"}},"required":[],"type":"object"})"sv);

  auto created = scry::Harness::create(scry::Config{
      .base_url = "https://api.anthropic.com",
      .api_key = "package-smoke",
      .model = "package-smoke",
  });
  if (!created) {
    return 1;
  }
  auto harness = std::move(*created);

  const auto registration = scry::reflection::add<PackageArguments>(
      harness.tools(),
      {
          .name = "package_smoke",
          .description = "Prove the installed reflection API is linkable",
      },
      [](PackageArguments arguments) { return arguments.ready; });
  return registration && harness.tools().size() == 1 ? 0 : 2;
}
