#pragma once

/**
 * @file tool_registry.hpp
 * @brief Explicit-schema tool definitions, handlers, and Harness-owned registration.
 *
 * This C++23 surface is the single runtime substrate for tools. The optional C++26
 * reflection API generates a schema and typed wrapper, then lowers to the same
 * ToolRegistry::add() operation.
 */

#include <cstddef>
#include <memory>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/unique_function.hpp>
#include <string>

namespace scry {

/// Provider-visible definition of an explicitly registered tool.
///
/// Registration validates the name and parses input_schema as a JSON object. Scry
/// stores a canonicalized copy; mutating the caller's original definition afterwards
/// has no effect on the registry.
struct ToolDefinition {
  /// Nonempty tool name, unique within the owning registry, exposed to the model.
  std::string name{};
  /// Optional human-readable description exposed to the model.
  std::string description{};
  /// JSON Schema object describing the tool's arguments.
  ///
  /// The explicit surface accepts runtime schemas beyond the closed subset generated
  /// by reflection. Scry verifies that this value is a valid JSON object but leaves
  /// application-specific schema semantics to the provider and handler.
  Json input_schema{};
};

/// Move-only type-erased explicit tool handler.
///
/// The input is the provider's validated, canonical JSON object. The handler is still
/// responsible for application-level field validation. A successful return must
/// contain syntactically valid JSON and fit the configured result/conversation bounds;
/// typed C++ handlers can instead use the optional scry::reflection component.
///
/// Handlers execute synchronously, in provider order, on the thread running
/// Harness::update(). Their returned errors and thrown exceptions become bounded
/// model-visible tool errors rather than exceptions delivered to the host.
using ToolHandler = UniqueFunction<Result<Json>(Json)>;

/// Harness-owned, additive registry of model-callable tools.
///
/// Registrations are snapshotted when a turn is accepted. Adding a tool therefore
/// affects only later turns, including when registration occurs reentrantly from a
/// callback. Duplicate names are rejected and registrations cannot be replaced or
/// removed. The Harness retains ownership; callers receive only a stable reference
/// through Harness::tools().
///
/// Registry mutation and observation belong to the host/pump thread. Handler callables
/// remain pump-owned and never cross into the worker actor.
class ToolRegistry final {
public:
  /// Destroys all live registrations after its owning Harness has stopped using them.
  ~ToolRegistry();

  /// Registers an explicit-schema tool.
  ///
  /// The definition and move-only handler are consumed regardless of success. On
  /// success, future accepted turns see the registration; turns already accepted retain
  /// their previous immutable snapshot.
  ///
  /// The handler executes synchronously inside Harness::update() on its calling thread.
  /// @param definition Valid provider-visible name, description, and object schema.
  /// @param handler Move-only callable that receives canonical argument JSON.
  /// @return Success, or an immediate inactive-registry, empty-name, empty-handler,
  /// invalid-schema, or duplicate-name error. Failure does not alter the registry.
  [[nodiscard]] Status add(ToolDefinition definition, ToolHandler handler);

  /// Returns the number of registrations currently available to future turns.
  /// @return Registration count.
  [[nodiscard]] std::size_t size() const noexcept;

  /// Reports whether no tools are registered.
  /// @return true when size() is zero.
  [[nodiscard]] bool empty() const noexcept;

private:
  /// Opaque additive registry state and cached immutable snapshots.
  class Impl;

  /// Transfers registry ownership internally while constructing a Harness.
  /// @param other Registry whose implementation ownership is transferred.
  ToolRegistry(ToolRegistry&& other) noexcept;
  /// Replaces registry ownership internally while constructing a Harness.
  /// @param other Registry whose implementation ownership is transferred.
  /// @return This internal registry object.
  ToolRegistry& operator=(ToolRegistry&& other) noexcept;
  /// Registry ownership cannot be copied away from its Harness.
  ToolRegistry(const ToolRegistry&) = delete;
  /// Registry ownership cannot be copy-assigned away from its Harness.
  ToolRegistry& operator=(const ToolRegistry&) = delete;

  /// Constructs the registry around freshly allocated implementation state.
  /// @param impl Non-null implementation created by Harness.
  explicit ToolRegistry(std::unique_ptr<Impl> impl) noexcept;

  /// Exclusive ownership of the opaque registry implementation.
  std::unique_ptr<Impl> impl_;

  friend class Harness;
};

} // namespace scry
