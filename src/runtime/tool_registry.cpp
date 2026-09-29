#include "kernel/error.hpp"
#include "kernel/json/codec.hpp"
#include "reflection/codec.hpp"
#include "runtime/tool_registry_impl.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
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

// The ToolRegistry::to_json() document, borrowed from the registered definitions.
struct ToolManifestEntry {
  std::string_view name;
  std::string_view description;
  const Json& input_schema;
};

struct ToolManifest {
  std::vector<ToolManifestEntry> tools;
  std::uint64_t version;
};

[[nodiscard]] Error inactive_registry() {
  return detail::make_error(ErrorCategory::invalid_state, "ToolRegistry is not active");
}

[[nodiscard]] std::unexpected<Error> invalid(std::string message) {
  return std::unexpected(
      detail::make_error(ErrorCategory::invalid_argument, std::move(message)));
}

// Checks one entry on its own and canonicalizes its schema in place.
[[nodiscard]] Status validate_entry(detail::ToolEntry& entry) {
  if (entry.definition.name.empty()) {
    return invalid("tool name must not be empty");
  }
  if (!entry.handler) {
    return invalid("tool handler must not be empty");
  }
  auto schema = detail::canonicalize_json_object(
      entry.definition.input_schema, ErrorCategory::invalid_argument,
      "tool input schema must be a valid JSON object");
  if (!schema) {
    return std::unexpected(std::move(schema.error()));
  }
  entry.definition.input_schema = std::move(*schema);
  return {};
}

} // namespace

Status ToolRegistry::Impl::add(std::vector<detail::ToolEntry> entries) {
  for (auto entry = entries.begin(); entry != entries.end(); ++entry) {
    if (auto valid = validate_entry(*entry); !valid) {
      return valid;
    }
    const auto& name = entry->definition.name;
    if (detail::find_tool(entries_, name) != nullptr) {
      return invalid("a tool named \"" + name + "\" is already registered");
    }
    if (std::any_of(entries.begin(), entry, [&name](const detail::ToolEntry& earlier) {
          return earlier.definition.name == name;
        })) {
      return invalid("a tool named \"" + name + "\" appears twice in one registration");
    }
  }

  // Everything that can allocate happens before the registry changes, so even an
  // allocation failure leaves no part of the batch registered.
  detail::ToolSnapshot added{};
  added.reserve(entries.size());
  for (auto& entry : entries) {
    added.push_back(
        std::make_shared<const detail::RegisteredTool>(detail::RegisteredTool{
            .definition = std::move(entry.definition),
            .handler = std::move(entry.handler),
        }));
  }
  entries_.reserve(entries_.size() + added.size());
  std::ranges::move(added, std::back_inserter(entries_));
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

Status ToolRegistry::add_dynamic(ToolDefinition definition, ToolHandler handler) {
  return add_dynamic(std::move(definition),
                     detail::to_contextual_handler(std::move(handler)));
}

Status ToolRegistry::add_dynamic(ToolDefinition definition,
                                 ContextualToolHandler handler) {
  std::vector<detail::ToolEntry> entries{};
  entries.push_back(detail::ToolEntry{
      .definition = std::move(definition),
      .handler = std::move(handler),
  });
  return add_all(std::move(entries));
}

Status ToolRegistry::add_all(std::vector<detail::ToolEntry> entries) {
  if (impl_ == nullptr) {
    return std::unexpected(inactive_registry());
  }
  return impl_->add(std::move(entries));
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

  ToolManifest manifest{.tools = {}, .version = tool_manifest_version};
  manifest.tools.reserve(impl_->entries().size());
  for (const auto& entry : impl_->entries()) {
    const auto& definition = entry->definition;
    manifest.tools.push_back(ToolManifestEntry{
        .name = definition.name,
        .description = definition.description,
        .input_schema = definition.input_schema,
    });
  }
  // Registration already canonicalized each schema. The codec validates it as it
  // splices it, so a broken invariant is a diagnostic rather than a null or
  // partial schema, and the final pass keeps the manifest canonical regardless.
  auto encoded = detail::encode_text(manifest);
  if (!encoded) {
    return std::unexpected(
        detail::make_error(ErrorCategory::invalid_state,
                           "Tool manifest at " + detail::describe(encoded.error())));
  }
  return detail::canonicalize_json(Json{.text = std::move(*encoded)},
                                   ErrorCategory::invalid_state,
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
