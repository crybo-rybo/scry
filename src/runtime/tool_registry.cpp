#include "core/json_codec.hpp"
#include "runtime/tool_registry_impl.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scry::detail {
namespace {

[[nodiscard]] Error invalid_registration(std::string message) {
  return Error{
      .category = ErrorCategory::invalid_argument,
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
      canonicalize_json_object(definition.input_schema, ErrorCategory::invalid_argument,
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
  state.entries.push_back(std::make_shared<const RegisteredTool>(RegisteredTool{
      .definition = std::move(definition),
      .handler = std::make_shared<ToolHandler>(std::move(handler)),
  }));
  return {};
}

ToolSnapshots snapshot_tools(ToolRegistryState& state) {
  // The registry is additive-only and single-app-thread, so a frozen view whose
  // size matches the working list is current. Registration leaves the frozen
  // pair stale; the first accepted send pays one rebuild after admission
  // validation, and every later accepted turn shares the same immutable blocks.
  if (state.frozen.entries && state.frozen.entries->size() == state.entries.size()) {
    return state.frozen;
  }
  auto entries = std::make_shared<ToolSnapshot>(state.entries);
  auto schemas = std::make_shared<std::vector<ToolSchema>>();
  schemas->reserve(entries->size());
  for (const auto& registration : *entries) {
    schemas->push_back(ToolSchema{
        .name = registration->definition.name,
        .description = registration->definition.description,
        .input_schema = registration->definition.input_schema,
    });
  }
  state.frozen = ToolSnapshots{
      .entries = std::move(entries),
      .schemas = std::move(schemas),
  };
  return state.frozen;
}

} // namespace scry::detail

namespace scry {

Status ToolRegistry::Impl::add(ToolDefinition definition, ToolHandler handler) {
  return detail::add_tool_registration(state, std::move(definition),
                                       std::move(handler));
}

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
  return impl_->add(std::move(definition), std::move(handler));
}

std::size_t ToolRegistry::size() const noexcept {
  return impl_ == nullptr ? 0 : impl_->state.entries.size();
}

bool ToolRegistry::empty() const noexcept { return size() == 0; }

bool ToolRegistry::contains(const std::string_view name) const noexcept {
  if (impl_ == nullptr) {
    return false;
  }
  return std::ranges::any_of(impl_->state.entries, [name](const auto& entry) {
    return entry->definition.name == name;
  });
}

std::vector<std::string> ToolRegistry::names() const {
  std::vector<std::string> registered{};
  if (impl_ == nullptr) {
    return registered;
  }
  registered.reserve(impl_->state.entries.size());
  for (const auto& entry : impl_->state.entries) {
    registered.push_back(entry->definition.name);
  }
  return registered;
}

Result<Json> ToolRegistry::to_json() const {
  if (impl_ == nullptr) {
    return std::unexpected(Error{
        .category = ErrorCategory::invalid_state,
        .message = "ToolRegistry is not active",
    });
  }

  detail::JsonValue::array_t tools{};
  tools.reserve(impl_->state.entries.size());
  for (const auto& entry : impl_->state.entries) {
    const auto& definition = entry->definition;
    auto schema =
        detail::parse_json(definition.input_schema.text, ErrorCategory::invalid_state,
                           "Registered tool schema could not be encoded");
    if (!schema) {
      return std::unexpected(std::move(schema.error()));
    }
    detail::JsonValue tool{};
    tool["name"] = definition.name;
    tool["description"] = definition.description;
    tool["input_schema"] = std::move(*schema);
    tools.push_back(std::move(tool));
  }

  detail::JsonValue root{};
  root["tools"].data = std::move(tools);
  root["version"] = std::uint64_t{1};
  return detail::write_json(root, ErrorCategory::invalid_state,
                            "Tool manifest could not be encoded");
}

} // namespace scry
