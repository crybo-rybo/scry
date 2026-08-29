#include "prepared_operation_impl.hpp"
#include "runtime/state.hpp"
#include "runtime/tool_registry_impl.hpp"
#include "scenario_support.hpp"
#include "scenarios.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scry::bench {
namespace {

[[nodiscard]] detail::ToolSchema schema_template(const std::size_t index) {
  return {
      .name = "benchmark-tool-" + std::to_string(index),
      .description = fixture_text(128, index),
      .input_schema = Json{.text = representative_schema(2048, index)},
  };
}

[[nodiscard]] ToolHandler tool_handler() {
  return ToolHandler{[](Json input) -> Result<Json> { return input; }};
}

} // namespace

ToolRegistryScenario::ToolRegistryScenario(const std::size_t schema_count) {
  schemas_.reserve(schema_count);
  for (std::size_t index = 0; index < schema_count; ++index) {
    auto schema = schema_template(index);
    input_bytes_ += schema.name.size() + schema.description.size() +
                    schema.input_schema.text.size();
    schemas_.push_back(std::move(schema));
  }
}

class ToolRegistryOperationState final {
public:
  ToolRegistryOperationState(const std::vector<detail::ToolSchema>& schema_templates,
                             const std::size_t logical_input_bytes,
                             const bool semantic_validation,
                             const ScenarioResult expected)
      : schemas{schema_templates}, oracle{expected}, input_bytes{logical_input_bytes},
        validate{semantic_validation} {
    handlers.reserve(schemas.size());
    for (std::size_t index = 0; index < schemas.size(); ++index) {
      handlers.push_back(tool_handler());
    }
  }

  [[nodiscard]] ScenarioResult run() {
    auto digest = validate ? fnv_offset : oracle.digest;
    auto valid = true;
    for (std::size_t index = 0; index < schemas.size(); ++index) {
      auto& schema = schemas[index];
      auto status = detail::add_tool_registration(
          state,
          ToolDefinition{.name = std::move(schema.name),
                         .description = std::move(schema.description),
                         .input_schema = std::move(schema.input_schema)},
          std::move(handlers[index]));
      valid = status.has_value() && valid;
    }
    if (validate) {
      for (const auto& entry : state.entries) {
        digest_text(digest, entry->definition.name);
        digest_text(digest, entry->definition.description);
        digest_text(digest, entry->definition.input_schema.text);
      }
    }
    valid = state.entries.size() == schemas.size() && valid;
    return {
        .digest = digest,
        .input_bytes = static_cast<std::uint64_t>(input_bytes),
        .output_bytes = static_cast<std::uint64_t>(input_bytes),
        .items = static_cast<std::uint64_t>(state.entries.size()),
        .valid = valid && (validate || oracle.valid),
    };
  }

private:
  detail::ToolRegistryState state{};
  std::vector<detail::ToolSchema> schemas{};
  std::vector<ToolHandler> handlers{};
  ScenarioResult oracle{};
  std::size_t input_bytes{};
  bool validate{};
};

ToolRegistryOperation ToolRegistryScenario::make_operation(const bool validate) const {
  return ToolRegistryOperation{std::make_unique<ToolRegistryOperationState>(
      schemas_, input_bytes_, validate, oracle_)};
}

ScenarioResult ToolRegistryScenario::validate() {
  auto operation = make_operation(true);
  oracle_ = operation.run();
  return oracle_;
}

ToolRegistryOperation ToolRegistryScenario::prepare() const {
  return make_operation(false);
}

template class PreparedOperation<ToolRegistryOperationState>;

} // namespace scry::bench
