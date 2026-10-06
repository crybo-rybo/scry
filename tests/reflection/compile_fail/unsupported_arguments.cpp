#include <scry/reflection.hpp>

struct UnsupportedArguments {
  char value{};
};

void register_tool(scry::ToolRegistry& registry) {
  const auto status = registry.add<UnsupportedArguments>(
      {
          .name = "unsupported_arguments",
          .description = "Must not compile",
      },
      [](UnsupportedArguments) { return 0; });
  (void)status;
}
