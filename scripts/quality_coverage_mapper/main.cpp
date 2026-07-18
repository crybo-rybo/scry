#include <cstdint>
#include <scry/tool_registry.hpp>
#include <scry/turn_id.hpp>
#include <utility>

int scry_public_api_contract_main();

int main(const int argc, char**) {
  if (scry_public_api_contract_main() != 0) {
    return 1;
  }

  const auto empty_id = scry::TurnId{.value = static_cast<std::uint64_t>(argc - 1)};
  const auto live_id = scry::TurnId{.value = static_cast<std::uint64_t>(argc)};
  if (static_cast<bool>(empty_id) || !static_cast<bool>(live_id)) {
    return 1;
  }

  scry::ToolHandler source{
      [](scry::Json input) { return scry::Result<scry::Json>{std::move(input)}; }};
  scry::ToolHandler target;
  target = std::move(source);
  auto* same = &target;
  target = std::move(*same);
  return static_cast<bool>(target) ? 0 : 1;
}
