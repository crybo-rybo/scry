#pragma once

#if !defined(__cpp_impl_reflection)
#error "Scry requires a compiler implementing P2996 (GCC 16 or newer)"
#endif

#include <cstddef>
#include <memory>
#include <meta>
#include <scry/detail/reflection_meta.hpp>
#include <scry/detail/reflection_registration.hpp>
#include <scry/detail/reflection_schema.hpp>
#include <scry/detail/reflection_tools.hpp>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/tool.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace scry {

namespace detail {
class ToolRegistryAccess;

template <typename Type> inline constexpr bool is_shared_ptr_v = false;
template <typename Type>
inline constexpr bool is_shared_ptr_v<std::shared_ptr<Type>> = true;

// add(Toolbox&&) takes any class object other than a shared_ptr, which has its
// own overload; whether it is a valid toolbox is reported by a diagnostic.
template <typename Type>
concept owned_toolbox_candidate = std::is_class_v<std::remove_cvref_t<Type>> &&
                                  !is_shared_ptr_v<std::remove_cvref_t<Type>>;
} // namespace detail

/// Additive registry of model-callable tools.
///
/// Tools are declared in C++ and registered by reflection: the argument schema,
/// the strict argument decoder, and the result encoder are generated from the
/// declaration. add() takes a typed argument aggregate and a callable, a function
/// annotated with scry::reflection::tool, every such function of a namespace, or
/// a toolbox object whose annotated member functions become tools bound to it.
/// add_dynamic() is the escape hatch for tools known only at runtime, such as
/// bridged or scripted ones: a hand-written JSON schema and a handler that takes
/// and returns JSON.
///
/// A registry is a standalone value: build one and export its manifest without a
/// Harness, provider configuration, or network stack. Harness::create() takes
/// ownership of a registry and exposes it through Harness::tools(), where
/// registration continues to work after creation. A moved-from registry is
/// inactive and every operation on it reports emptiness or
/// ErrorCategory::invalid_state.
///
/// Every registration call is atomic: a call that registers several tools checks
/// all of them, including their names against each other and against the
/// registry, before it inserts any, so a failed call changes nothing.
/// Registrations are snapshotted when a turn is accepted. Adding a tool therefore
/// affects only later turns. Duplicate names are rejected and registrations cannot
/// be replaced or removed. Use one registry from one host thread; every handler
/// runs on that thread, inside Harness::update().
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

  /// Registers a callable whose arguments are a reflected aggregate.
  ///
  /// The argument schema is generated as scry::reflection::input_schema_v<Args>,
  /// incoming JSON is decoded strictly into `Args`, and the typed return is
  /// encoded back to JSON. A handler may take a leading const ToolCallContext& to
  /// learn which turn, round, and call it is running for; the context is borrowed
  /// for the invocation only. It may return a supported value, a Result of one,
  /// `void`, or scry::Status; the last two send `{}` on success.
  ///
  /// The template is unconstrained so that a rejected type fails with a diagnostic
  /// naming the offending member and the reason; scry::reflection::ToolArguments
  /// and scry::reflection::ToolHandlerFor are the SFINAE-friendly form of the same
  /// checks.
  /// @tparam Args Complete reflected argument aggregate satisfying ToolArguments.
  /// @tparam Handler Move-constructible callable satisfying ToolHandlerFor<Handler,
  /// Args>.
  /// @param metadata Provider-visible tool name and description.
  /// @param handler Callable invoked with Args moved by value, optionally preceded
  /// by a const ToolCallContext& naming the call being serviced.
  /// @return Success, an immediate validation/duplicate-name error, or
  /// ErrorCategory::invalid_state for an inactive registry.
  template <typename Args, typename Handler>
  [[nodiscard]] Status add(ToolMetadata metadata, Handler&& handler);

  /// Registers one annotated function, or every annotated function of a namespace.
  ///
  /// `tools.add<^^forecast>()` registers a function declared with
  /// `[[= scry::reflection::tool{"description"}]]`; `tools.add<^^npc_tools>()`
  /// registers every function so declared directly in that namespace, in
  /// declaration order. A tool is named after the function unless a
  /// scry::reflection::name annotation overrides it. Its parameters, after an
  /// optional leading const ToolCallContext&, are either nothing (the tool takes
  /// `{}`), one plain aggregate (the argument object, as for add<Args>()), or any
  /// other list of named parameters of supported types, which is synthesized into
  /// an argument object with one required member per parameter. It returns what
  /// an add<Args>() handler may return. A function that breaks these rules fails
  /// to compile with its name and the reason.
  /// A namespace is open, so the tools registered are the ones declared in it
  /// before the call, in the calling translation unit. Two translation units that
  /// see different parts of one namespace each register their own part.
  /// @tparam Entity Reflection of a tool function, a static member function, or a
  /// namespace.
  /// @tparam Tools Leave defaulted. The tool functions `Entity` holds at the call
  /// site; it gives each distinct set its own specialization, which the linker
  /// cannot merge with another translation unit's.
  /// @return Success, an immediate validation/duplicate-name error, or
  /// ErrorCategory::invalid_state for an inactive registry. A namespace is
  /// registered all or nothing.
  template <std::meta::info Entity,
            typename Tools = typename[:reflection::detail::tool_set_of(Entity):]>
  [[nodiscard]] Status add();

  /// Registers a shared toolbox: every tool member function of `Toolbox`, bound to
  /// this object.
  ///
  /// The member functions follow the rules of add<^^function>(). Each registration
  /// holds a copy of the pointer, so the toolbox lives while the registry, or any
  /// turn that snapshotted these tools, still holds one, and the last of those is
  /// released on the host thread. Handlers run on that thread inside
  /// Harness::update(), so the toolbox's state needs no locking. A `const`
  /// toolbox admits only const member functions.
  /// @tparam Toolbox Class satisfying scry::reflection::Toolbox.
  /// @param toolbox Non-null toolbox to share.
  /// @return Success, ErrorCategory::invalid_argument for a null toolbox or an
  /// immediate validation/duplicate-name error, or ErrorCategory::invalid_state
  /// for an inactive registry. The toolbox is registered all or nothing.
  template <typename Toolbox>
  [[nodiscard]] Status add(std::shared_ptr<Toolbox> toolbox);

  /// Registers an owned toolbox: moves it into the registry, then registers it as
  /// add(std::shared_ptr<Toolbox>) would.
  ///
  /// The registry owns the only reference, so the toolbox's state is reachable
  /// only through its tools. Share it through std::shared_ptr instead when the
  /// host needs to see that state too.
  /// @tparam Toolbox Movable class satisfying scry::reflection::Toolbox, passed as
  /// an rvalue.
  /// @param toolbox Toolbox to take ownership of.
  /// @return As add(std::shared_ptr<Toolbox>).
  template <typename Toolbox>
    requires detail::owned_toolbox_candidate<Toolbox>
  [[nodiscard]] Status add(Toolbox&& toolbox);

  /// Registers a dynamic tool: a hand-written schema and a JSON handler.
  ///
  /// This is the escape hatch for tools that exist only at runtime, such as ones
  /// bridged from a script or another process. Prefer add() for tools declared in
  /// C++. The handler executes synchronously inside Harness::update() on its
  /// calling thread and owns validation of its arguments against the schema.
  /// @param definition Valid provider-visible name, description, and object schema.
  /// @param handler Move-only callable that receives canonical argument JSON.
  /// @return Success, an immediate validation/duplicate-name error, or
  /// ErrorCategory::invalid_state for an inactive registry.
  [[nodiscard]] Status add_dynamic(ToolDefinition definition, ToolHandler handler);

  /// Registers a dynamic tool whose handler also receives its call identity.
  ///
  /// Behaves exactly like the ToolHandler overload; the handler additionally learns
  /// which turn, round, and call it is servicing. The context is borrowed for the
  /// invocation only.
  /// @param definition Valid provider-visible name, description, and object schema.
  /// @param handler Move-only callable that receives the call context and canonical
  /// argument JSON.
  /// @return Success, an immediate validation/duplicate-name error, or
  /// ErrorCategory::invalid_state for an inactive registry.
  [[nodiscard]] Status add_dynamic(ToolDefinition definition,
                                   ContextualToolHandler handler);

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
  /// reflected and dynamic registrations are included. Export does not invoke
  /// handlers or contact a provider, and needs no Harness: a registry built on
  /// its own exports the same manifest a Harness-owned one would. Later
  /// registrations appear only in subsequent exports, independently of any
  /// in-flight turn's frozen tool set. Object keys are emitted in lexical order.
  /// @return Canonical JSON, or ErrorCategory::invalid_state if the registry is
  /// inactive or its manifest cannot be encoded.
  [[nodiscard]] Result<Json> to_json() const;

private:
  class Impl;

  // Validates every entry, against each other and against the registry, then
  // inserts them all; a failure inserts none.
  [[nodiscard]] Status add_all(std::vector<detail::ToolEntry> entries);

  std::unique_ptr<Impl> impl_;

  friend class detail::ToolRegistryAccess;
};

template <typename Args, typename Handler>
Status ToolRegistry::add(ToolMetadata metadata, Handler&& handler) {
  constexpr std::string_view arguments_problem =
      reflection::detail::tool_arguments_diagnostic<Args>();
  static_assert(arguments_problem.empty(), arguments_problem);
  if constexpr (!arguments_problem.empty()) {
    return {};
  } else {
    constexpr std::string_view handler_problem =
        reflection::detail::tool_handler_diagnostic<Handler, Args>();
    static_assert(handler_problem.empty(), handler_problem);
    if constexpr (!handler_problem.empty()) {
      return {};
    } else {
      return add_dynamic(
          ToolDefinition{
              .name = std::move(metadata.name),
              .description = std::move(metadata.description),
              .input_schema =
                  Json{.text = std::string{reflection::input_schema_v<Args>}},
          },
          reflection::detail::make_tool_handler<Args>(std::forward<Handler>(handler)));
    }
  }
}

template <std::meta::info Entity, typename Tools> Status ToolRegistry::add() {
  constexpr std::string_view problem =
      reflection::detail::entity_tools_diagnostic<Entity>();
  static_assert(problem.empty(), problem);
  if constexpr (!problem.empty()) {
    return {};
  } else {
    return add_all(reflection::detail::tool_set_entries(Tools{}));
  }
}

template <typename Toolbox> Status ToolRegistry::add(std::shared_ptr<Toolbox> toolbox) {
  constexpr std::string_view problem =
      reflection::detail::toolbox_diagnostic<Toolbox>();
  static_assert(problem.empty(), problem);
  if constexpr (!problem.empty()) {
    return {};
  } else {
    if (toolbox == nullptr) {
      return std::unexpected(Error{
          .category = ErrorCategory::invalid_argument,
          .message = "toolbox must not be null",
      });
    }
    return add_all(reflection::detail::toolbox_entries(toolbox));
  }
}

template <typename Toolbox>
  requires detail::owned_toolbox_candidate<Toolbox>
Status ToolRegistry::add(Toolbox&& toolbox) {
  constexpr std::string_view problem =
      reflection::detail::owned_toolbox_diagnostic<Toolbox>();
  static_assert(problem.empty(), problem);
  if constexpr (!problem.empty()) {
    return {};
  } else {
    using Owned = std::remove_cvref_t<Toolbox>;
    return add(std::make_shared<Owned>(std::forward<Toolbox>(toolbox)));
  }
}

} // namespace scry
