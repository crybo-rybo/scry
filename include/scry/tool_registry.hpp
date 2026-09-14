#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/turn_id.hpp>
#include <scry/unique_function.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace scry {

namespace detail {
class ToolRegistryAccess;
} // namespace detail

/// Provider-visible definition of an explicitly registered tool.
struct ToolDefinition {
  /// Unique tool name exposed to the model.
  std::string name{};
  /// Human-readable description exposed to the model.
  std::string description{};
  /// JSON Schema object describing the tool's arguments.
  Json input_schema{};
};

/// Identity of the tool call a handler is servicing.
///
/// A handler that needs to know which turn, round, or call it is running for takes
/// one of these as its leading parameter. Both string views are borrowed from the
/// live call block and are valid only for the duration of the invocation; a handler
/// that keeps either beyond its return must copy it.
struct ToolCallContext {
  /// Turn the call belongs to.
  TurnId turn_id{};
  /// Provider-assigned identifier of this call, matching ToolCall::id.
  std::string_view call_id{};
  /// Registered name of the tool being invoked.
  std::string_view tool_name{};
  /// One-based tool round within the turn.
  std::uint32_t round{};
  /// Zero-based position of this call in its round's batch.
  std::uint32_t index{};
};

/// Move-only type-erased explicit tool handler.
///
/// The input is a canonical JSON object. The handler validates it against its schema;
/// Scry does not perform general JSON Schema validation. A successful return must
/// contain valid JSON. Typed C++ handlers can instead use scry::reflection.
using ToolHandler = UniqueFunction<Result<Json>(Json)>;

/// Move-only type-erased explicit tool handler that also receives its call identity.
///
/// Identical to ToolHandler apart from the leading ToolCallContext, whose string
/// views are borrowed for the invocation only.
using ContextualToolHandler =
    UniqueFunction<Result<Json>(const ToolCallContext&, Json)>;

/// Additive registry of model-callable tools.
///
/// A registry is a standalone value: build one and export its manifest without a
/// Harness, provider configuration, or network stack. Harness::create() takes
/// ownership of a registry and exposes it through Harness::tools(), where
/// registration continues to work after creation. A moved-from registry is
/// inactive and every operation on it reports emptiness or
/// ErrorCategory::invalid_state.
///
/// Registrations are snapshotted when a turn is accepted. Adding a tool therefore
/// affects only later turns. Duplicate names are rejected and registrations cannot be
/// replaced or removed. Use one registry from one host thread.
class ToolRegistry final {
public:
  /// Creates an empty, active registry that any host thread can populate.
  ToolRegistry();

  /// Destroys the registry and its registrations.
  ~ToolRegistry();

  /// Moves ownership of the registrations, leaving the source inactive.
  ToolRegistry(ToolRegistry&&) noexcept;

  /// Replaces this registry with another moved registry.
  /// @return This registry.
  ToolRegistry& operator=(ToolRegistry&&) noexcept;

  /// Registries are not copyable.
  ToolRegistry(const ToolRegistry&) = delete;

  /// Registries are not copy-assignable.
  ToolRegistry& operator=(const ToolRegistry&) = delete;

  /// Registers an explicit-schema tool.
  ///
  /// The handler executes synchronously inside Harness::update() on its calling thread.
  /// @param definition Valid provider-visible name, description, and object schema.
  /// @param handler Move-only callable that receives canonical argument JSON.
  /// @return Success, an immediate validation/duplicate-name error, or
  /// ErrorCategory::invalid_state for an inactive registry.
  [[nodiscard]] Status add(ToolDefinition definition, ToolHandler handler);

  /// Registers an explicit-schema tool whose handler also receives its call identity.
  ///
  /// Behaves exactly like the ToolHandler overload; the handler additionally learns
  /// which turn, round, and call it is servicing. The context is borrowed for the
  /// invocation only.
  /// @param definition Valid provider-visible name, description, and object schema.
  /// @param handler Move-only callable that receives the call context and canonical
  /// argument JSON.
  /// @return Success, an immediate validation/duplicate-name error, or
  /// ErrorCategory::invalid_state for an inactive registry.
  [[nodiscard]] Status add(ToolDefinition definition, ContextualToolHandler handler);

  /// Returns the number of registrations currently available to future turns.
  /// @return Registration count, or 0 for an inactive registry.
  [[nodiscard]] std::size_t size() const noexcept;

  /// Reports whether no tools are registered.
  /// @return true when size() is zero.
  [[nodiscard]] bool empty() const noexcept;

  /// Reports whether a tool with the given name is registered.
  /// @param name Tool name to look for. Comparison is exact.
  /// @return true when a registration with that name exists; false for an inactive
  /// registry.
  [[nodiscard]] bool contains(std::string_view name) const noexcept;

  /// Lists every registered tool name in registration order.
  /// @return The names, or an empty vector for an inactive registry.
  [[nodiscard]] std::vector<std::string> names() const;

  /// Exports the currently registered LLM tool contracts as a JSON manifest.
  ///
  /// The version-1 document contains a tools array in registration order. Each
  /// entry contains name, description, and input_schema (a JSON object). Both
  /// explicit and reflected registrations are included. Export does not invoke
  /// handlers or contact a provider, and needs no Harness: a registry built on
  /// its own exports the same manifest a Harness-owned one would. Later
  /// registrations appear only in subsequent exports, independently of any
  /// in-flight turn's frozen tool set. Object keys are emitted in lexical order.
  /// @return Canonical JSON, or ErrorCategory::invalid_state if the registry is
  /// inactive or its manifest cannot be encoded.
  [[nodiscard]] Result<Json> to_json() const;

private:
  class Impl;

  std::unique_ptr<Impl> impl_;

  friend class detail::ToolRegistryAccess;
};

} // namespace scry
