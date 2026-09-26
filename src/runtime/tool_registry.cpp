#include "core/error.hpp"
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

ContextualToolHandler to_contextual_handler(ToolHandler handler) {
  if (!handler) {
    return {};
  }
  return ContextualToolHandler{
      [inner = std::move(handler)](const ToolCallContext&, Json input) mutable {
        return inner(std::move(input));
      }};
}

const RegisteredTool* find_tool(const ToolSnapshot& tools,
                                const std::string_view name) noexcept {
  const auto found = std::ranges::find_if(
      tools, [name](const auto& entry) { return entry->definition.name == name; });
  return found == tools.end() ? nullptr : found->get();
}

} // namespace scry::detail

namespace scry {
namespace {

constexpr std::uint64_t tool_manifest_version = 1;

[[nodiscard]] Error inactive_registry() {
  return detail::make_error(ErrorCategory::invalid_state, "ToolRegistry is not active");
}

} // namespace

Status ToolRegistry::Impl::add(ToolDefinition definition,
                               ContextualToolHandler handler) {
  const auto invalid = [](std::string message) {
    return std::unexpected(
        detail::make_error(ErrorCategory::invalid_argument, std::move(message)));
  };
  if (definition.name.empty()) {
    return invalid("tool name must not be empty");
  }
  if (!handler) {
    return invalid("tool handler must not be empty");
  }
  auto schema = detail::canonicalize_json_object(
      definition.input_schema, ErrorCategory::invalid_argument,
      "tool input schema must be a valid JSON object");
  if (!schema) {
    return std::unexpected(std::move(schema.error()));
  }
  if (detail::find_tool(entries_, definition.name) != nullptr) {
    return invalid("a tool with that name is already registered");
  }

  definition.input_schema = std::move(*schema);
  entries_.push_back(
      std::make_shared<const detail::RegisteredTool>(detail::RegisteredTool{
          .definition = std::move(definition),
          .handler = std::move(handler),
      }));
  return {};
}

detail::FrozenToolSnapshot ToolRegistry::Impl::snapshot() {
  // Tools are only ever added, and only from the app thread, so a frozen
  // snapshot with as many entries as the working list is still current. Any
  // rebuild is shared by every turn until the next registration.
  if (frozen_.entries && frozen_.entries->size() == entries_.size()) {
    return frozen_;
  }
  auto entries = std::make_shared<detail::ToolSnapshot>(entries_);
  auto schemas = std::make_shared<std::vector<ToolDefinition>>();
  schemas->reserve(entries->size());
  for (const auto& registration : *entries) {
    schemas->push_back(registration->definition);
  }
  frozen_ = detail::FrozenToolSnapshot{
      .entries = std::move(entries),
      .schemas = std::move(schemas),
  };
  return frozen_;
}

ToolRegistry::ToolRegistry() : impl_(std::make_unique<Impl>()) {}

ToolRegistry::~ToolRegistry() = default;
ToolRegistry::ToolRegistry(ToolRegistry&&) noexcept = default;
ToolRegistry& ToolRegistry::operator=(ToolRegistry&&) noexcept = default;

Status ToolRegistry::add(ToolDefinition definition, ToolHandler handler) {
  return add(std::move(definition), detail::to_contextual_handler(std::move(handler)));
}

Status ToolRegistry::add(ToolDefinition definition, ContextualToolHandler handler) {
  if (impl_ == nullptr) {
    return std::unexpected(inactive_registry());
  }
  return impl_->add(std::move(definition), std::move(handler));
}

std::size_t ToolRegistry::size() const noexcept {
  return impl_ == nullptr ? 0 : impl_->entries().size();
}

bool ToolRegistry::empty() const noexcept { return size() == 0; }

bool ToolRegistry::contains(const std::string_view name) const noexcept {
  return impl_ != nullptr && detail::find_tool(impl_->entries(), name) != nullptr;
}

std::vector<std::string> ToolRegistry::names() const {
  std::vector<std::string> registered{};
  if (impl_ == nullptr) {
    return registered;
  }
  registered.reserve(impl_->entries().size());
  for (const auto& entry : impl_->entries()) {
    registered.push_back(entry->definition.name);
  }
  return registered;
}

Result<Json> ToolRegistry::to_json() const {
  if (impl_ == nullptr) {
    return std::unexpected(inactive_registry());
  }

  detail::JsonValue::array_t tools{};
  tools.reserve(impl_->entries().size());
  for (const auto& entry : impl_->entries()) {
    const auto& definition = entry->definition;
    detail::JsonValue tool{};
    tool["name"] = definition.name;
    tool["description"] = definition.description;
    // Registration already canonicalized the schema. Preserve a diagnostic if
    // that invariant is broken instead of exporting a null or partial schema.
    if (auto status =
            detail::parse_json_into(tool["input_schema"], definition.input_schema.text,
                                    ErrorCategory::invalid_state,
                                    "Registered schema for tool '" + definition.name +
                                        "' could not be encoded");
        !status) {
      return std::unexpected(std::move(status.error()));
    }
    tools.push_back(std::move(tool));
  }

  detail::JsonValue root{};
  root["tools"].data = std::move(tools);
  root["version"] = tool_manifest_version;
  return detail::write_json(root, ErrorCategory::invalid_state,
                            "Tool manifest could not be encoded");
}

} // namespace scry

namespace scry::detail {

void ToolRegistryAccess::ensure_active(ToolRegistry& tools) {
  if (tools.impl_ == nullptr) {
    tools.impl_ = std::make_unique<ToolRegistry::Impl>();
  }
}

FrozenToolSnapshot ToolRegistryAccess::snapshot(ToolRegistry& tools) {
  ensure_active(tools);
  return tools.impl_->snapshot();
}

} // namespace scry::detail
