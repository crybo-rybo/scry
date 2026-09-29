#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <scry/annotations.hpp>
#include <scry/detail/reflection_meta.hpp>
#include <scry/detail/reflection_model.hpp>
#include <scry/error.hpp>
#include <scry/tool.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// The compile-time model of reflected tools: which functions are tools, what they
// are named, how a parameter list becomes one argument object, and why a
// declaration cannot be registered. Everything here is a consteval function of a
// reflection; reflection_registration.hpp turns the answers into handlers.
namespace scry::reflection::detail {

// ---- Tool declarations -------------------------------------------------------

// A class or namespace walk meets entities that cannot carry annotations, such
// as templates and namespace aliases, where annotations_of throws. None of those
// is a tool.
consteval std::vector<std::meta::info>
annotations_or_none(const std::meta::info entity) {
  try {
    return std::meta::annotations_of(entity);
  } catch (const std::meta::exception&) {
    return {};
  }
}

consteval bool is_tool_declaration(const std::meta::info entity) {
  for (const auto annotation : annotations_or_none(entity)) {
    if (annotation_matches(annotation, ^^tool)) {
      return true;
    }
  }
  return false;
}

// The functions of a class or namespace that are tools, in declaration order.
consteval std::vector<std::meta::info> tool_functions_of(const std::meta::info scope) {
  std::vector<std::meta::info> functions{};
  for (const auto member :
       std::meta::members_of(scope, std::meta::access_context::unchecked())) {
    if (std::meta::is_function(member) && is_tool_declaration(member)) {
      functions.push_back(member);
    }
  }
  return functions;
}

// The tool name the model sees: a name annotation, or else the identifier.
consteval std::string_view tool_name_of(const std::meta::info function) {
  if (const auto annotated = annotated_text(function, ^^name)) {
    return *annotated;
  }
  return std::meta::identifier_of(function);
}

consteval std::string_view tool_description_of(const std::meta::info function) {
  return *annotated_text(function, ^^tool);
}

// ---- Parameters ----------------------------------------------------------------

consteval bool is_context_parameter(const std::meta::info parameter) {
  return std::meta::dealias(std::meta::type_of(parameter)) ==
         std::meta::dealias(^^const scry::ToolCallContext&);
}

consteval bool takes_context(const std::meta::info function) {
  const auto parameters = std::meta::parameters_of(function);
  return !parameters.empty() && is_context_parameter(parameters.front());
}

// The parameters the model supplies: every one after the optional context.
consteval std::vector<std::meta::info>
value_parameters_of(const std::meta::info function) {
  auto parameters = std::meta::parameters_of(function);
  if (takes_context(function)) {
    parameters.erase(parameters.begin());
  }
  return parameters;
}

// How a tool function receives its arguments. A lone parameter of plain aggregate
// type is the whole argument object, exactly as for add<Args>(); every other
// non-empty list is synthesized into an aggregate with one member per parameter.
enum class arguments_form : std::uint8_t {
  none,
  aggregate,
  synthesized,
};

consteval arguments_form arguments_form_of(const std::meta::info function) {
  const auto values = value_parameters_of(function);
  if (values.empty()) {
    return arguments_form::none;
  }
  if (values.size() == 1 &&
      kind_of(std::meta::type_of(values.front())) == value_kind::aggregate) {
    return arguments_form::aggregate;
  }
  return arguments_form::synthesized;
}

// The argument object of a tool that takes no arguments. Its schema is the empty
// closed object, and decoding accepts `{}` alone.
struct no_arguments {};

// ---- Registration identity ----------------------------------------------------

// The member names of a synthesized argument object, each followed by a comma:
// the value parameters' identifiers. Empty for the other forms, whose generated
// code does not depend on parameter names.
consteval std::string synthesized_member_list(const std::meta::info function) {
  std::string list{};
  if (arguments_form_of(function) == arguments_form::synthesized) {
    for (const auto parameter : value_parameters_of(function)) {
      list += owned_text(std::meta::identifier_of(parameter));
      list.push_back(',');
    }
  }
  return list;
}

// The text of a reflect_constant_string array, without its terminator.
template <std::meta::info Text>
inline constexpr std::string_view constant_text_v{[:Text:], sizeof([:Text:]) - 1U};

// One tool function as a registration sees it.
//
// A function's tool name, description, and parameter names come from the
// declarations of it that precede the point of evaluation, and annotations
// accumulate across redeclarations, so two translation units can see one
// function differently. Each of those facts is a template argument here, a
// constant string that GCC mangles by its content, so units that see different
// facts instantiate differently named code, which the linker cannot merge. The
// code generated for a tool reads these facts from here and never from the
// function's declarations, so what a specialization's name records is exactly
// what its code contains. Everything else it reads from the function, such as
// the parameter types, the return type, and the object parameter, is part of the
// function's type and the same in every unit.
template <std::meta::info Function, std::meta::info Name, std::meta::info Description,
          std::meta::info Members>
struct bound_tool {
  static constexpr std::meta::info function = Function;
  static constexpr std::string_view name = constant_text_v<Name>;
  static constexpr std::string_view description = constant_text_v<Description>;
  // The synthesized member names, as synthesized_member_list writes them.
  static constexpr std::string_view members = constant_text_v<Members>;
};

// The tools one registration call registers, as bound_tool types, in
// registration order. The set is part of the registration's identity too: a
// namespace can hold different functions in different translation units, and a
// class's member functions can gain annotations, including the tool annotation
// itself, from an out-of-class definition that only some units see.
template <typename... Tools> struct tool_set {};

// The argument object synthesized from a parameter list: one required member per
// value parameter, named as `Tool` records, of the parameter's type without
// references or cv-qualifiers. The consteval block runs when the specialization
// is instantiated, which C++26 permits because the aggregate it completes is a
// member of that same specialization. The names are read one character at a
// time, for the reason owned_text gives.
template <typename Tool> struct synthesized_arguments {
  struct type;

  consteval {
    std::vector<std::meta::info> members{};
    std::size_t position = 0;
    for (const auto parameter : value_parameters_of(Tool::function)) {
      std::string name{};
      for (; Tool::members[position] != ','; ++position) {
        name.push_back(Tool::members[position]);
      }
      ++position;
      std::meta::data_member_options options{};
      options.name = name;
      members.push_back(std::meta::data_member_spec(
          plain_type(std::meta::type_of(parameter)), options));
    }
    std::meta::define_aggregate(^^type, members);
  }
};

// Instantiates synthesized_arguments only for the form that needs it, and only
// after the function has passed tool_function_problem.
template <typename Tool> consteval std::meta::info tool_arguments_type() {
  constexpr auto form = arguments_form_of(Tool::function);
  if constexpr (form == arguments_form::none) {
    return ^^no_arguments;
  } else if constexpr (form == arguments_form::aggregate) {
    return plain_type(std::meta::type_of(value_parameters_of(Tool::function).front()));
  } else {
    return ^^typename synthesized_arguments<Tool>::type;
  }
}

template <typename Tool>
using tool_arguments_t = typename[:tool_arguments_type<Tool>():];

// ---- Diagnostics -------------------------------------------------------------

consteval std::string decimal(std::size_t value) {
  std::string digits{};
  do {
    digits.insert(digits.begin(), static_cast<char>('0' + (value % 10U)));
    value /= 10U;
  } while (value != 0U);
  return digits;
}

// The name a diagnostic calls an entity by: qualified through named namespaces
// and classes, without the signature display_string_of adds to a function.
consteval std::string entity_name(const std::meta::info entity) {
  const auto own = std::meta::has_identifier(entity)
                       ? owned_text(std::meta::identifier_of(entity))
                       : display(entity);
  if (entity == ^^::) {
    return own;
  }
  const auto parent = std::meta::parent_of(entity);
  if (std::meta::is_type(parent)) {
    return display(parent) + "::" + own;
  }
  if (std::meta::is_namespace(parent) &&
      parent != ^^::&&std::meta::has_identifier(parent)) {
    return entity_name(parent) + "::" + own;
  }
  return own;
}

consteval std::string tool_annotation_problem(const std::meta::info function) {
  if (annotation_count(function, ^^tool) > 1) {
    return "has more than one scry::reflection::tool annotation";
  }
  if (annotation_count(function, ^^name) > 1) {
    return "has more than one scry::reflection::name annotation";
  }
  if (has_annotation(function, ^^description) || has_annotation(function, ^^tag) ||
      has_annotation(function, ^^skip_null_t) ||
      has_annotation(function, ^^emit_null_t) ||
      has_annotation(function, ^^ignore_unknown_t)) {
    return "carries a codec annotation (description, tag, skip_null, emit_null, or "
           "ignore_unknown), which applies to data members or classes; a tool's "
           "description is its scry::reflection::tool text";
  }
  if (!has_annotation(function, ^^name) && !std::meta::has_identifier(function)) {
    return "has no identifier to name the tool after; give it a "
           "scry::reflection::name annotation";
  }
  return {};
}

// `toolbox` is the class a member tool is bound to, or the null reflection for a
// function registered on its own.
consteval std::string binding_problem(const std::meta::info function,
                                      const std::meta::info toolbox,
                                      const bool const_toolbox) {
  if (std::meta::is_deleted(function)) {
    return "is deleted";
  }
  if (std::meta::is_constructor(function) || std::meta::is_destructor(function)) {
    return "is a constructor or destructor";
  }
  const bool bound =
      std::meta::is_class_member(function) && !std::meta::is_static_member(function);
  if (toolbox == std::meta::info{}) {
    return bound ? "is a non-static member function; register an object of " +
                       display(std::meta::parent_of(function)) + " as a toolbox"
                 : std::string{};
  }
  if (!std::meta::is_public(function)) {
    return "is not a public member function";
  }
  if (bound && std::meta::is_rvalue_reference_qualified(function)) {
    return "is &&-qualified, so the toolbox object the registry holds cannot call it";
  }
  if (bound && const_toolbox && !std::meta::is_const(function)) {
    return "is not a const member function, and the toolbox is const";
  }
  return {};
}

consteval std::string parameter_label(const std::meta::info parameter,
                                      const std::size_t index) {
  return std::meta::has_identifier(parameter)
             ? "parameter `" + owned_text(std::meta::identifier_of(parameter)) + "`"
             : "parameter " + decimal(index + 1U);
}

// Checks each parameter's passing convention: the decoded arguments are moved
// into the call, so a parameter may take a value, a const reference, or an
// rvalue reference, and the call context may appear only first.
consteval std::string passing_problem(const std::meta::info function) {
  const auto parameters = std::meta::parameters_of(function);
  for (std::size_t index = takes_context(function) ? 1U : 0U; index < parameters.size();
       ++index) {
    const auto type = std::meta::dealias(std::meta::type_of(parameters[index]));
    const auto label = parameter_label(parameters[index], index);
    if (plain_type(type) == (^^scry::ToolCallContext)) {
      return "has " + label +
             " of type scry::ToolCallContext; the context is only ever the leading "
             "parameter, as const scry::ToolCallContext&";
    }
    const auto referred = std::meta::remove_reference(type);
    if ((std::meta::is_lvalue_reference_type(type) &&
         !std::meta::is_const_type(referred)) ||
        std::meta::is_volatile_type(referred)) {
      return "has " + label + " of type " +
             display(std::meta::type_of(parameters[index])) +
             ", which the decoded arguments cannot bind; take it by value or by "
             "const reference";
    }
  }
  return {};
}

consteval std::string arguments_problem(const std::meta::info function) {
  const auto values = value_parameters_of(function);
  const std::size_t first = takes_context(function) ? 1U : 0U;
  if (arguments_form_of(function) == arguments_form::aggregate) {
    const auto reason =
        tool_arguments_problem(plain_type(std::meta::type_of(values.front())));
    return reason.empty()
               ? std::string{}
               : "has " + parameter_label(values.front(), first) + ": " + reason;
  }
  for (std::size_t index = 0; index < values.size(); ++index) {
    const auto label = parameter_label(values[index], first + index);
    if (!std::meta::has_identifier(values[index])) {
      return "has the unnamed " + label +
             "; an argument synthesized from a parameter is named after it";
    }
    const auto reason =
        value_problem(std::meta::type_of(values[index]), value_family::supported);
    if (!reason.empty()) {
      return "has " + label + ": " + reason;
    }
  }
  return {};
}

// Why a function cannot be registered as a tool, prefixed with its name; empty
// when it can. It must already carry a tool annotation.
consteval std::string tool_function_problem(const std::meta::info function,
                                            const std::meta::info toolbox,
                                            const bool const_toolbox) {
  auto reason = tool_annotation_problem(function);
  if (reason.empty()) {
    reason = binding_problem(function, toolbox, const_toolbox);
  }
  if (reason.empty()) {
    reason = passing_problem(function);
  }
  if (reason.empty()) {
    reason = arguments_problem(function);
  }
  if (reason.empty()) {
    const auto result = result_type_problem(std::meta::return_type_of(function));
    reason = result.empty() ? std::string{} : "returns " + result;
  }
  return reason.empty() ? std::string{} : entity_name(function) + " " + reason;
}

// Rejects a tool annotation on anything a walk finds that is not a function.
consteval std::string misplaced_tool_problem(const std::meta::info scope) {
  for (const auto member :
       std::meta::members_of(scope, std::meta::access_context::unchecked())) {
    if (!std::meta::is_function(member) && is_tool_declaration(member)) {
      return entity_name(member) +
             " carries scry::reflection::tool, which applies to functions";
    }
  }
  return {};
}

consteval std::string
duplicate_names_problem(const std::vector<std::meta::info>& tools) {
  for (std::size_t left = 0; left < tools.size(); ++left) {
    for (std::size_t right = left + 1; right < tools.size(); ++right) {
      const auto shared = tool_name_of(tools[left]);
      if (shared == tool_name_of(tools[right])) {
        return display(tools[left]) + " and " + display(tools[right]) +
               " are both tools named \"" + owned_text(shared) + "\"";
      }
    }
  }
  return {};
}

// The checks a class or namespace of tools shares. `toolbox` is the class the
// tools bind to, or the null reflection for a namespace.
consteval std::string tool_scope_problem(const std::meta::info scope,
                                         const std::meta::info toolbox,
                                         const bool const_toolbox) {
  if (auto misplaced = misplaced_tool_problem(scope); !misplaced.empty()) {
    return misplaced;
  }
  const auto tools = tool_functions_of(scope);
  if (tools.empty()) {
    return (toolbox == std::meta::info{} ? "namespace " : "") + entity_name(scope) +
           " declares no function annotated with scry::reflection::tool";
  }
  for (const auto function : tools) {
    if (auto reason = tool_function_problem(function, toolbox, const_toolbox);
        !reason.empty()) {
      return reason;
    }
  }
  return duplicate_names_problem(tools);
}

consteval std::string toolbox_problem(const std::meta::info type) {
  const auto toolbox = plain_type(type);
  if (!std::meta::is_class_type(toolbox)) {
    return display(type) + " is not a class type";
  }
  if (!is_complete_class(toolbox)) {
    return display(type) + " is an incomplete type";
  }
  return tool_scope_problem(
      toolbox, toolbox, std::meta::is_const_type(std::meta::remove_reference(type)));
}

consteval std::string entity_problem(const std::meta::info entity) {
  if (std::meta::is_namespace(entity)) {
    return tool_scope_problem(entity, {}, false);
  }
  if (std::meta::is_function(entity)) {
    return is_tool_declaration(entity)
               ? tool_function_problem(entity, {}, false)
               : entity_name(entity) + " has no scry::reflection::tool annotation";
  }
  if (std::meta::is_type(entity)) {
    return display(entity) + " is a type; register an object of it as a toolbox with "
                             "add(std::shared_ptr<T>) or add(T&&)";
  }
  if (std::meta::is_function_template(entity)) {
    return display(entity) +
           " is a function template; a tool is one function with one schema";
  }
  return display(entity) + " is neither a function nor a namespace";
}

// ---- Registration sets --------------------------------------------------------

// The bound_tool of a function that has passed tool_function_problem, with the
// facts the declarations preceding this point give it.
consteval std::meta::info bound_tool_of(const std::meta::info function) {
  const auto text = [](const std::string_view value) {
    return std::meta::reflect_constant(
        std::meta::reflect_constant_string(owned_text(value)));
  };
  return std::meta::substitute(^^bound_tool,
                               {
                                   std::meta::reflect_constant(function),
                                   text(tool_name_of(function)),
                                   text(tool_description_of(function)),
                                   text(synthesized_member_list(function))});
}

consteval std::meta::info tool_set_of(const std::vector<std::meta::info>& functions) {
  std::vector<std::meta::info> tools{};
  for (const auto function : functions) {
    tools.push_back(bound_tool_of(function));
  }
  return std::meta::substitute(^^tool_set, tools);
}

// Reflects the tool_set add<^^Entity>() registers: the tool functions a
// namespace declares before this point, or the one function. An entity the
// registration rejects yields the empty set, and its diagnostic reports why.
consteval std::meta::info entity_tool_set(const std::meta::info entity) {
  if (!entity_problem(entity).empty()) {
    return ^^tool_set<>;
  }
  return tool_set_of(std::meta::is_namespace(entity) ? tool_functions_of(entity)
                                                     : std::vector{entity});
}

// Reflects the tool_set a toolbox of `type` registers: its class's tool member
// functions, in declaration order. A type the registration rejects yields the
// empty set, and its diagnostic reports why.
consteval std::meta::info toolbox_tool_set(const std::meta::info type) {
  if (!toolbox_problem(type).empty()) {
    return ^^tool_set<>;
  }
  return tool_set_of(tool_functions_of(plain_type(type)));
}

template <std::meta::info Entity> consteval std::string_view entity_tools_diagnostic() {
  const auto reason = entity_problem(Entity);
  if (reason.empty()) {
    return {};
  }
  const auto shown = std::meta::is_type(Entity) ? display(Entity) : entity_name(Entity);
  return static_string("scry::ToolRegistry::add<^^" + shown + ">(): " + reason);
}

template <typename Type> consteval std::string_view toolbox_diagnostic() {
  return unsatisfied(display(^^Type), "Toolbox", toolbox_problem(^^Type));
}

// add(Toolbox&&) takes ownership, so it needs an rvalue it can move from.
template <typename Passed> consteval std::string_view owned_toolbox_diagnostic() {
  using Owned = std::remove_cvref_t<Passed>;
  if constexpr (std::is_lvalue_reference_v<Passed>) {
    return static_string(
        display(std::meta::dealias(^^Owned)) +
        " is passed as an lvalue: ToolRegistry::add(Toolbox&&) takes ownership of a "
        "toolbox, so std::move it in, or share it through std::shared_ptr");
  } else if constexpr (!std::constructible_from<Owned, Passed>) {
    return static_string(display(std::meta::dealias(^^Owned)) +
                         " cannot be moved into the registry; share it through "
                         "std::shared_ptr instead");
  } else {
    return toolbox_diagnostic<Owned>();
  }
}

} // namespace scry::reflection::detail

namespace scry::reflection {

/// Classes whose `[[= scry::reflection::tool{...}]]` member functions
/// ToolRegistry::add() registers as tools bound to one object.
///
/// A toolbox is a complete class that declares at least one tool member function
/// itself; inherited member functions are not considered. Every tool member must
/// be a public, non-deleted, non-`&&`-qualified function with a valid tool
/// signature, `const` when the toolbox is, and the tool names must be distinct.
/// Registration reports the first violation with the member's name and the reason.
template <typename Type>
concept Toolbox = detail::toolbox_problem(^^Type).empty();

} // namespace scry::reflection
