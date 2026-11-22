#include <scry/tool_registry.hpp>

struct Arguments {
  int value{};
};

void register_tool(scry::ToolRegistry& registry) {
  // Scrydantic calls a handler as const, so state it changes lives outside it. An
  // add_dynamic() handler is Scry's own UniqueFunction and may be mutable.
  const auto status = registry.add<Arguments>(
      {
          .name = "mutable_handler",
          .description = "Must not compile",
      },
      [calls = 0](Arguments) mutable { return ++calls; });
  (void)status;
}
