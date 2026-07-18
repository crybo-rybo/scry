#include "runtime/tool_registry_impl.hpp"

#include <algorithm>
#include <utility>

namespace scry {
namespace {

[[nodiscard]] Error invalid_registration(std::string message) {
  return Error{
      .category = ErrorCategory::invalid_state,
      .message = std::move(message),
  };
}

} // namespace

ToolRegistry::ToolRegistry(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ToolRegistry::~ToolRegistry() = default;
ToolRegistry::ToolRegistry(ToolRegistry&&) noexcept = default;
ToolRegistry& ToolRegistry::operator=(ToolRegistry&&) noexcept = default;

Status ToolRegistry::add(ToolDefinition definition, ToolHandler handler) {
  if (impl_ == nullptr) {
    return std::unexpected(invalid_registration("ToolRegistry is not active"));
  }
  if (definition.name.empty()) {
    return std::unexpected(invalid_registration("tool name must not be empty"));
  }
  if (definition.input_schema.text.empty()) {
    return std::unexpected(invalid_registration("tool input schema must not be empty"));
  }
  if (!handler) {
    return std::unexpected(invalid_registration("tool handler must not be empty"));
  }
  const auto duplicate =
      std::ranges::any_of(impl_->state->entries, [&definition](const auto& entry) {
        return entry.definition.name == definition.name;
      });
  if (duplicate) {
    return std::unexpected(
        invalid_registration("a tool with that name is already registered"));
  }
  impl_->state->entries.push_back(detail::RegistryEntry{
      .definition = std::move(definition),
      .handler = std::move(handler),
  });
  return {};
}

std::size_t ToolRegistry::size() const noexcept {
  return impl_ == nullptr ? 0 : impl_->state->entries.size();
}

bool ToolRegistry::empty() const noexcept { return size() == 0; }

} // namespace scry
