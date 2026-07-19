#include "core/json_codec.hpp"
#include "runtime/tool_registry_impl.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace scry::detail {
namespace {

[[nodiscard]] Error invalid_registration(std::string message) {
  return Error{
      .category = ErrorCategory::invalid_state,
      .message = std::move(message),
  };
}

} // namespace

Status add_tool_registration(ToolRegistryState& state, ToolDefinition definition,
                             ToolHandler handler) {
  if (definition.name.empty()) {
    return std::unexpected(invalid_registration("tool name must not be empty"));
  }
  if (!handler) {
    return std::unexpected(invalid_registration("tool handler must not be empty"));
  }
  auto schema =
      canonicalize_json_object(definition.input_schema, ErrorCategory::invalid_state,
                               "tool input schema must be a valid JSON object");
  if (!schema) {
    return std::unexpected(std::move(schema.error()));
  }
  const auto duplicate =
      std::ranges::any_of(state.entries, [&definition](const auto& entry) {
        return entry->definition.name == definition.name;
      });
  if (duplicate) {
    return std::unexpected(
        invalid_registration("a tool with that name is already registered"));
  }

  definition.input_schema = std::move(*schema);
  auto shared_handler = std::make_shared<ToolHandler>(std::move(handler));
  state.entries.push_back(std::make_shared<const RegisteredTool>(RegisteredTool{
      .definition = std::move(definition),
      .handler = std::move(shared_handler),
  }));
  return {};
}

ToolSnapshot snapshot_tools(const ToolRegistryState& state) { return state.entries; }

std::vector<ToolSchema> snapshot_schemas(const ToolSnapshot& snapshot) {
  std::vector<ToolSchema> schemas{};
  schemas.reserve(snapshot.size());
  for (const auto& registration : snapshot) {
    schemas.push_back(ToolSchema{
        .name = registration->definition.name,
        .description = registration->definition.description,
        .input_schema = registration->definition.input_schema,
    });
  }
  return schemas;
}

} // namespace scry::detail

namespace scry {

ToolRegistry::ToolRegistry(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ToolRegistry::~ToolRegistry() = default;
ToolRegistry::ToolRegistry(ToolRegistry&&) noexcept = default;
ToolRegistry& ToolRegistry::operator=(ToolRegistry&&) noexcept = default;

Status ToolRegistry::add(ToolDefinition definition, ToolHandler handler) {
  if (impl_ == nullptr) {
    return std::unexpected(Error{
        .category = ErrorCategory::invalid_state,
        .message = "ToolRegistry is not active",
    });
  }
  return detail::add_tool_registration(impl_->state, std::move(definition),
                                       std::move(handler));
}

std::size_t ToolRegistry::size() const noexcept {
  return impl_ == nullptr ? 0 : impl_->state.entries.size();
}

bool ToolRegistry::empty() const noexcept { return size() == 0; }

} // namespace scry
