#include <scry/reflection.hpp>

struct Arguments {
  int value{};
};

void register_tool(scry::ToolRegistry& registry) {
  const auto status =
      scry::reflection::add<Arguments>(registry,
                                       {
                                           .name = "raw_json_handler",
                                           .description = "Must not compile",
                                       },
                                       [](Arguments) -> scry::Json { return {}; });
  (void)status;
}
