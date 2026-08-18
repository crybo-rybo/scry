#include "runtime/state.hpp"
#include "runtime/tool_registry_impl.hpp"
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

constexpr std::uint64_t fnv_offset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t fnv_prime = 1'099'511'628'211ULL;

void digest_byte(std::uint64_t& digest, const std::uint8_t value) noexcept {
  digest ^= value;
  digest *= fnv_prime;
}

void digest_number(std::uint64_t& digest, std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    digest_byte(digest, static_cast<std::uint8_t>(value & 0xffU));
    value >>= 8U;
  }
}

void digest_text(std::uint64_t& digest, const std::string_view text) noexcept {
  digest_number(digest, static_cast<std::uint64_t>(text.size()));
  for (const auto value : text) {
    digest_byte(digest, static_cast<std::uint8_t>(value));
  }
}

[[nodiscard]] std::string fixture_text(const std::size_t size, const std::size_t seed) {
  return std::string(size, static_cast<char>('a' + static_cast<char>(seed % 26)));
}

[[nodiscard]] std::string representative_schema(const std::size_t seed) {
  constexpr auto prefix = std::string_view{R"({"description":")"};
  constexpr auto suffix = std::string_view{
      R"(","properties":{"payload":{"type":"string"}},"type":"object"})"};
  constexpr std::size_t schema_bytes = 2048;
  return std::string{prefix} +
         fixture_text(schema_bytes - prefix.size() - suffix.size(), seed) +
         std::string{suffix};
}

[[nodiscard]] detail::ToolSchema schema_template(const std::size_t index) {
  return {
      .name = "benchmark-tool-" + std::to_string(index),
      .description = fixture_text(128, index),
      .input_schema = Json{.text = representative_schema(index)},
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

struct ToolRegistryOperation::Impl final {
  Impl(const std::vector<detail::ToolSchema>& schema_templates,
       const std::size_t logical_input_bytes, const bool semantic_validation,
       const ScenarioResult expected)
      : schemas{schema_templates}, oracle{expected}, input_bytes{logical_input_bytes},
        validate{semantic_validation} {
    handlers.reserve(schemas.size());
    for (std::size_t index = 0; index < schemas.size(); ++index) {
      handlers.push_back(tool_handler());
    }
  }

  detail::ToolRegistryState state{};
  std::vector<detail::ToolSchema> schemas{};
  std::vector<ToolHandler> handlers{};
  ScenarioResult oracle{};
  std::size_t input_bytes{};
  bool validate{};
};

ToolRegistryOperation::ToolRegistryOperation(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ToolRegistryOperation::~ToolRegistryOperation() = default;
ToolRegistryOperation::ToolRegistryOperation(ToolRegistryOperation&&) noexcept =
    default;
ToolRegistryOperation&
ToolRegistryOperation::operator=(ToolRegistryOperation&&) noexcept = default;

ScenarioResult ToolRegistryOperation::run() {
  if (!impl_) {
    return {};
  }
  auto& operation = *impl_;
  auto digest = operation.validate ? fnv_offset : operation.oracle.digest;
  auto valid = true;
  for (std::size_t index = 0; index < operation.schemas.size(); ++index) {
    auto& schema = operation.schemas[index];
    auto status = detail::add_tool_registration(
        operation.state,
        ToolDefinition{.name = std::move(schema.name),
                       .description = std::move(schema.description),
                       .input_schema = std::move(schema.input_schema)},
        std::move(operation.handlers[index]));
    valid = status.has_value() && valid;
  }
  if (operation.validate) {
    for (const auto& entry : operation.state.entries) {
      digest_text(digest, entry->definition.name);
      digest_text(digest, entry->definition.description);
      digest_text(digest, entry->definition.input_schema.text);
    }
  }
  valid = operation.state.entries.size() == operation.schemas.size() && valid;
  return {
      .digest = digest,
      .input_bytes = static_cast<std::uint64_t>(operation.input_bytes),
      .output_bytes = static_cast<std::uint64_t>(operation.input_bytes),
      .items = static_cast<std::uint64_t>(operation.state.entries.size()),
      .valid = valid && (operation.validate || operation.oracle.valid),
  };
}

ToolRegistryOperation ToolRegistryScenario::make_operation(const bool validate) const {
  return ToolRegistryOperation{std::make_unique<ToolRegistryOperation::Impl>(
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

} // namespace scry::bench
