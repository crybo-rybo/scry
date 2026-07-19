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

Status add_tool_registration(ToolRegistryState& state, CommandQueue& commands,
                             ToolDefinition definition, ToolHandler handler,
                             const ToolRegistrationOptions options) {
  if (definition.name.empty()) {
    return std::unexpected(invalid_registration("tool name must not be empty"));
  }
  if (!handler) {
    return std::unexpected(invalid_registration("tool handler must not be empty"));
  }
  if (options.execution != ToolExecution::app_thread &&
      options.execution != ToolExecution::worker_thread) {
    return std::unexpected(
        invalid_registration("tool execution mode is not recognized"));
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
  const auto worker_name = definition.name;
  if (options.execution == ToolExecution::worker_thread) {
    state.entries.push_back(std::make_shared<const RegisteredTool>(RegisteredTool{
        .definition = std::move(definition),
        .execution = options.execution,
        .handler = {},
    }));
    commands.push(RegisterWorkerToolCommand{
        .name = worker_name,
        .handler = std::move(handler),
    });
    return {};
  }

  state.entries.push_back(std::make_shared<const RegisteredTool>(RegisteredTool{
      .definition = std::move(definition),
      .execution = options.execution,
      .handler = std::make_shared<ToolHandler>(std::move(handler)),
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

std::vector<std::string> snapshot_worker_tool_names(const ToolSnapshot& snapshot) {
  std::vector<std::string> names{};
  names.reserve(snapshot.size());
  for (const auto& registration : snapshot) {
    if (registration->execution == ToolExecution::worker_thread) {
      names.push_back(registration->definition.name);
    }
  }
  return names;
}

} // namespace scry::detail

namespace scry {

Status ToolRegistry::Impl::add(ToolDefinition definition, ToolHandler handler,
                               const ToolRegistrationOptions options) {
  const auto commands = commands_.lock();
  if (!commands) {
    return std::unexpected(Error{
        .category = ErrorCategory::invalid_state,
        .message = "ToolRegistry is not active",
    });
  }
  return detail::add_tool_registration(state, *commands, std::move(definition),
                                       std::move(handler), options);
}

ToolRegistry::ToolRegistry(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ToolRegistry::~ToolRegistry() = default;
ToolRegistry::ToolRegistry(ToolRegistry&&) noexcept = default;
ToolRegistry& ToolRegistry::operator=(ToolRegistry&&) noexcept = default;

Status ToolRegistry::add(ToolDefinition definition, ToolHandler handler) {
  return add(std::move(definition), std::move(handler), ToolRegistrationOptions{});
}

Status ToolRegistry::add(ToolDefinition definition, ToolHandler handler,
                         const ToolRegistrationOptions options) {
  if (impl_ == nullptr) {
    return std::unexpected(Error{
        .category = ErrorCategory::invalid_state,
        .message = "ToolRegistry is not active",
    });
  }
  return impl_->add(std::move(definition), std::move(handler), options);
}

std::size_t ToolRegistry::size() const noexcept {
  return impl_ == nullptr ? 0 : impl_->state.entries.size();
}

bool ToolRegistry::empty() const noexcept { return size() == 0; }

} // namespace scry
