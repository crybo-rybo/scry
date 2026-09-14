#include <scry/reflection.hpp>

struct Arguments {
  int value{};
};

void register_tool(scry::ToolRegistry& registry) {
  // The call context is a leading parameter. Trailing it must not compile, or a
  // handler would silently never be reached.
  const auto status = scry::reflection::add<Arguments>(
      registry,
      {
          .name = "wrong_context_order",
          .description = "Must not compile",
      },
      [](Arguments, const scry::ToolCallContext&) { return 0; });
  (void)status;
}
