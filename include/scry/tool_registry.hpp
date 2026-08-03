#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/unique_function.hpp>
#include <string>

namespace scry {

/// Provider-visible definition of an explicitly registered tool.
struct ToolDefinition {
  /// Unique tool name exposed to the model.
  std::string name{};
  /// Human-readable description exposed to the model.
  std::string description{};
  /// JSON Schema object describing the tool's arguments.
  Json input_schema{};
};

/// Move-only type-erased explicit tool handler.
///
/// The input is a validated JSON object. A successful return must contain valid JSON;
/// typed C++ handlers can instead use the optional scry::reflection component.
using ToolHandler = UniqueFunction<Result<Json>(Json)>;

/// Thread on which a registered tool handler executes.
enum class ToolExecution : std::uint8_t {
  /// Execute synchronously inside Harness::update() on its calling thread.
  app_thread,
  /// Execute on the Harness worker after Harness::update() delivers the call.
  worker_thread,
};

/// Host-side execution policy for one tool registration.
struct ToolRegistrationOptions {
  /// Selected handler execution thread.
  ToolExecution execution{ToolExecution::app_thread};
};

/// Harness-owned, additive registry of model-callable tools.
///
/// Registrations are snapshotted when a turn is accepted. Adding a tool therefore
/// affects only later turns. Duplicate names are rejected and registrations cannot be
/// replaced or removed.
class ToolRegistry final {
public:
  /// Destroys the registry after its owning Harness has stopped using it.
  ~ToolRegistry();

  /// Registers an explicit-schema tool for app-thread execution.
  /// @param definition Valid provider-visible name, description, and object schema.
  /// @param handler Move-only callable that receives canonical argument JSON.
  /// @return Success, or an immediate validation/duplicate-name error.
  [[nodiscard]] Status add(ToolDefinition definition, ToolHandler handler);

  /// Registers an explicit-schema tool with an execution policy.
  ///
  /// Worker-thread handlers are non-preemptive. Cancellation suppresses their result
  /// after they return, so applications requiring bounded teardown must ensure those
  /// handlers cooperate.
  /// @param definition Valid provider-visible name, description, and object schema.
  /// @param handler Move-only callable that receives canonical argument JSON.
  /// @param options Host-side execution policy, never exposed to the provider.
  /// @return Success, or an immediate validation/duplicate-name error.
  [[nodiscard]] Status add(ToolDefinition definition, ToolHandler handler,
                           ToolRegistrationOptions options);

  /// Returns the number of registrations currently available to future turns.
  /// @return Registration count.
  [[nodiscard]] std::size_t size() const noexcept;

  /// Reports whether no tools are registered.
  /// @return true when size() is zero.
  [[nodiscard]] bool empty() const noexcept;

private:
  class Impl;

  ToolRegistry(ToolRegistry&&) noexcept;
  ToolRegistry& operator=(ToolRegistry&&) noexcept;
  ToolRegistry(const ToolRegistry&) = delete;
  ToolRegistry& operator=(const ToolRegistry&) = delete;

  explicit ToolRegistry(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;

  friend class Harness;
};

} // namespace scry
