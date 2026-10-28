#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <scrydantic/annotations.hpp>
#include <scrydantic/detail/meta.hpp>
#include <scrydantic/detail/model.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// The compile-time model of reflected tools: which functions are tools, what they
// are named, how a parameter list becomes one argument object, and why a
// declaration cannot be bound. Everything here is a consteval function of a
// reflection; binding.hpp turns the answers into handlers.
//
// A binding may give its tools a call context, a type of the host's choosing that
// a tool function receives by const reference as its leading parameter. The
// consteval functions take it as `context`, the reflection of that type, or of
// `void` when the binding has none.
namespace scrydantic::detail {

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

consteval bool has_context(const std::meta::info context) {
  return std::meta::dealias(context) != ^^void;
}

// The type a context parameter is declared with: `const Context&`.
consteval std::meta::info context_parameter_type(const std::meta::info context) {
  return std::meta::add_lvalue_reference(
      std::meta::add_const(std::meta::dealias(context)));
}

consteval bool is_context_parameter(const std::meta::info parameter,
                                    const std::meta::info context) {
  return has_context(context) && std::meta::dealias(std::meta::type_of(parameter)) ==
                                     context_parameter_type(context);
}

// A C++23 explicit object parameter (`this const Box& self`) is the object a
// member tool is called on. Member-call syntax binds it to the toolbox, so it is
// neither the context nor an argument the model supplies.
consteval bool has_explicit_object(const std::meta::info function) {
  const auto parameters = std::meta::parameters_of(function);
  return !parameters.empty() &&
         std::meta::is_explicit_object_parameter(parameters.front());
}

consteval std::size_t object_parameter_count(const std::meta::info function) {
  return has_explicit_object(function) ? 1U : 0U;
}

consteval bool takes_context(const std::meta::info function,
                             const std::meta::info context) {
  const auto parameters = std::meta::parameters_of(function);
  const auto first = object_parameter_count(function);
  return parameters.size() > first && is_context_parameter(parameters[first], context);
}

// The index of the first parameter the model supplies: after the explicit object
// parameter and the context, when the function has them.
consteval std::size_t first_value_index(const std::meta::info function,
                                        const std::meta::info context) {
  return object_parameter_count(function) +
         (takes_context(function, context) ? 1U : 0U);
}

// The parameters the model supplies, in declaration order.
consteval std::vector<std::meta::info>
value_parameters_of(const std::meta::info function, const std::meta::info context) {
  auto parameters = std::meta::parameters_of(function);
  parameters.erase(parameters.begin(),
                   parameters.begin() + static_cast<std::ptrdiff_t>(
                                            first_value_index(function, context)));
  return parameters;
}

// How a tool function receives its arguments. A lone parameter of plain aggregate
// type is the whole argument object, exactly as for bind_handler<Args>(); every other
// non-empty list is synthesized into an aggregate with one member per parameter.
enum class arguments_form : std::uint8_t {
  none,
  aggregate,
  synthesized,
};

consteval arguments_form arguments_form_of(const std::meta::info function,
                                           const std::meta::info context) {
  const auto values = value_parameters_of(function, context);
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
consteval std::string synthesized_member_list(const std::meta::info function,
                                              const std::meta::info context) {
  std::string list{};
  if (arguments_form_of(function, context) == arguments_form::synthesized) {
    for (const auto parameter : value_parameters_of(function, context)) {
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
// function's type and the same in every unit. `Context` is the binding's call
// context, which decides whether the leading parameter is an argument.
template <std::meta::info Function, std::meta::info Context, std::meta::info Name,
          std::meta::info Description, std::meta::info Members>
struct bound_tool {
  static constexpr std::meta::info function = Function;
  static constexpr std::meta::info context = Context;
  static constexpr std::string_view name = constant_text_v<Name>;
  static constexpr std::string_view description = constant_text_v<Description>;
  // The synthesized member names, as synthesized_member_list writes them.
  static constexpr std::string_view members = constant_text_v<Members>;
};

// The tools one binding call binds, as bound_tool types, in declaration order. The set
// is part of the registration's identity too: a namespace can hold different functions
// in different translation units, and a class's member functions can gain annotations,
// including the tool annotation itself, from an out-of-class definition that only some
// units see.
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
    for (const auto parameter : value_parameters_of(Tool::function, Tool::context)) {
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
  constexpr auto form = arguments_form_of(Tool::function, Tool::context);
  if constexpr (form == arguments_form::none) {
    return ^^no_arguments;
  } else if constexpr (form == arguments_form::aggregate) {
    return plain_type(
        std::meta::type_of(value_parameters_of(Tool::function, Tool::context).front()));
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
    return "has more than one scrydantic::tool annotation";
  }
  if (annotation_count(function, ^^name) > 1) {
    return "has more than one scrydantic::name annotation";
  }
  if (has_annotation(function, ^^description) || has_annotation(function, ^^tag) ||
      has_annotation(function, ^^skip_null_t) ||
      has_annotation(function, ^^emit_null_t) ||
      has_annotation(function, ^^ignore_unknown_t)) {
    return "carries a codec annotation (description, tag, skip_null, emit_null, or "
           "ignore_unknown), which applies to data members or classes; a tool's "
           "description is its scrydantic::tool text";
  }
  if (!has_annotation(function, ^^name) && !std::meta::has_identifier(function)) {
    return "has no identifier to name the tool after; give it a "
           "scrydantic::name annotation";
  }
  return {};
}

// A member tool with an explicit object parameter is called on the toolbox its
// binding holds, an lvalue that is const when the toolbox is. That lvalue must
// initialize the parameter, as for any argument: a reference to the toolbox
// class or a base of it binds directly, a by-value parameter copies the toolbox,
// and a parameter of another type needs an implicit conversion.
consteval std::string explicit_object_problem(const std::meta::info function,
                                              const std::meta::info toolbox,
                                              const bool const_toolbox) {
  const auto declared = std::meta::type_of(std::meta::parameters_of(function).front());
  const auto type = std::meta::dealias(declared);
  const auto referred = std::meta::remove_reference(type);
  if (std::meta::is_rvalue_reference_type(type)) {
    return "takes its explicit object parameter by rvalue reference, so the toolbox "
           "object the tools are bound to cannot call it";
  }
  if (std::meta::is_lvalue_reference_type(type) && const_toolbox &&
      !std::meta::is_const_type(referred)) {
    return "takes its explicit object parameter by non-const reference, and the "
           "toolbox is const";
  }
  const auto object = const_toolbox ? std::meta::add_const(toolbox) : toolbox;
  if (std::meta::is_convertible_type(std::meta::add_lvalue_reference(object), type)) {
    return {};
  }
  if (!std::meta::is_reference_type(type) && plain_type(type) == toolbox) {
    return "takes its explicit object parameter by value, and " + display(toolbox) +
           " cannot be copy-initialized from an lvalue of " + display(object);
  }
  return "takes its explicit object parameter as " + display(declared) +
         ", which an lvalue of " + display(object) + " does not convert to";
}

// `toolbox` is the class a member tool is bound to, or the null reflection for a
// function bound on its own.
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
  if (has_explicit_object(function)) {
    return explicit_object_problem(function, toolbox, const_toolbox);
  }
  if (bound && std::meta::is_rvalue_reference_qualified(function)) {
    return "is &&-qualified, so the toolbox object the tools are bound to cannot "
           "call it";
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
// rvalue reference, and the call context may appear only first. Parameters are
// numbered as declared, counting an explicit object parameter.
consteval std::string passing_problem(const std::meta::info function,
                                      const std::meta::info context) {
  const auto parameters = std::meta::parameters_of(function);
  for (std::size_t index = first_value_index(function, context);
       index < parameters.size(); ++index) {
    const auto type = std::meta::dealias(std::meta::type_of(parameters[index]));
    const auto label = parameter_label(parameters[index], index);
    if (has_context(context) && plain_type(type) == plain_type(context)) {
      return "has " + label + " of type " + display(plain_type(context)) +
             "; the context is only ever the leading parameter, as " +
             display(context_parameter_type(context));
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

consteval std::string arguments_problem(const std::meta::info function,
                                        const std::meta::info context) {
  const auto values = value_parameters_of(function, context);
  const std::size_t first = first_value_index(function, context);
  if (arguments_form_of(function, context) == arguments_form::aggregate) {
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

// Why a function cannot be bound as a tool, prefixed with its name; empty when
// it can. It must already carry a tool annotation.
consteval std::string tool_function_problem(const std::meta::info function,
                                            const std::meta::info toolbox,
                                            const bool const_toolbox,
                                            const std::meta::info context) {
  auto reason = tool_annotation_problem(function);
  if (reason.empty()) {
    reason = binding_problem(function, toolbox, const_toolbox);
  }
  if (reason.empty()) {
    reason = passing_problem(function, context);
  }
  if (reason.empty()) {
    reason = arguments_problem(function, context);
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
             " carries scrydantic::tool, which applies to functions";
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
                                         const bool const_toolbox,
                                         const std::meta::info context) {
  if (auto misplaced = misplaced_tool_problem(scope); !misplaced.empty()) {
    return misplaced;
  }
  const auto tools = tool_functions_of(scope);
  if (tools.empty()) {
    return (toolbox == std::meta::info{} ? "namespace " : "") + entity_name(scope) +
           " declares no function annotated with scrydantic::tool";
  }
  for (const auto function : tools) {
    if (auto reason = tool_function_problem(function, toolbox, const_toolbox, context);
        !reason.empty()) {
      return reason;
    }
  }
  return duplicate_names_problem(tools);
}

consteval std::string toolbox_problem(const std::meta::info type,
                                      const std::meta::info context) {
  const auto toolbox = plain_type(type);
  if (!std::meta::is_class_type(toolbox)) {
    return display(type) + " is not a class type";
  }
  if (!is_complete_class(toolbox)) {
    return display(type) + " is an incomplete type";
  }
  return tool_scope_problem(toolbox, toolbox,
                            std::meta::is_const_type(std::meta::remove_reference(type)),
                            context);
}

consteval std::string entity_problem(const std::meta::info entity,
                                     const std::meta::info context) {
  if (std::meta::is_namespace(entity)) {
    return tool_scope_problem(entity, {}, false, context);
  }
  if (std::meta::is_function(entity)) {
    return is_tool_declaration(entity)
               ? tool_function_problem(entity, {}, false, context)
               : entity_name(entity) + " has no scrydantic::tool annotation";
  }
  if (std::meta::is_type(entity)) {
    return display(entity) + " is a type; bind an object of it as a toolbox instead";
  }
  if (std::meta::is_function_template(entity)) {
    return display(entity) +
           " is a function template; a tool is one function with one schema";
  }
  return display(entity) + " is neither a function nor a namespace";
}

// ---- Tool sets ----------------------------------------------------------------

// The bound_tool of a function that has passed tool_function_problem, with the
// facts the declarations preceding this point give it.
consteval std::meta::info bound_tool_of(const std::meta::info function,
                                        const std::meta::info context) {
  const auto text = [](const std::string_view value) {
    return std::meta::reflect_constant(
        std::meta::reflect_constant_string(owned_text(value)));
  };
  return std::meta::substitute(
      ^^bound_tool,
      {
          std::meta::reflect_constant(function), std::meta::reflect_constant(context),
          text(tool_name_of(function)), text(tool_description_of(function)),
          text(synthesized_member_list(function, context))});
}

consteval std::meta::info tool_set_of(const std::vector<std::meta::info>& functions,
                                      const std::meta::info context) {
  std::vector<std::meta::info> tools{};
  for (const auto function : functions) {
    tools.push_back(bound_tool_of(function, context));
  }
  return std::meta::substitute(^^tool_set, tools);
}

// The diagnostic of a binding of `Entity` through `api`, the function the caller
// called, such as "scrydantic::bind": the call, then the reason. Empty when the
// entity binds.
template <std::meta::info Entity, typename Context>
consteval std::string_view entity_tools_diagnostic(const std::string_view api) {
  const auto reason = entity_problem(Entity, ^^Context);
  if (reason.empty()) {
    return {};
  }
  const auto shown = std::meta::is_type(Entity) ? display(Entity) : entity_name(Entity);
  return static_string(owned_text(api) + "<^^" + shown + ">(): " + reason);
}

template <typename Type, typename Context>
consteval std::string_view toolbox_diagnostic() {
  return unsatisfied(display(^^Type), "Toolbox", toolbox_problem(^^Type, ^^Context));
}

} // namespace scrydantic::detail

namespace scrydantic {

/// Reflects the tools bind<^^Entity, Context>() binds: the tool functions a
/// namespace declares before the point of evaluation, or the one function.
///
/// This is the default of bind()'s `Tools` parameter. A library that wraps bind()
/// computes the same default on its own entry point and forwards it, so the tools
/// are the ones its caller's translation unit sees. An entity that cannot be bound
/// yields the empty set, and bind() reports why.
/// @param entity Reflection of a tool function, a static member function, or a
/// namespace.
/// @param context Reflection of the call context type, or of `void` for none.
/// @return Reflection of the tool set type.
consteval std::meta::info tools_of(const std::meta::info entity,
                                   const std::meta::info context) {
  if (!detail::entity_problem(entity, context).empty()) {
    return ^^detail::tool_set<>;
  }
  return detail::tool_set_of(std::meta::is_namespace(entity)
                                 ? detail::tool_functions_of(entity)
                                 : std::vector{entity},
                             context);
}

/// Reflects the tools bind<Context>(std::shared_ptr<Toolbox>) binds: the tool
/// member functions of the toolbox class, in declaration order.
///
/// This is the default of that bind()'s `Tools` parameter; see tools_of().
/// @param type Reflection of the toolbox type, `const` for a const toolbox.
/// @param context Reflection of the call context type, or of `void` for none.
/// @return Reflection of the tool set type.
consteval std::meta::info toolbox_tools_of(const std::meta::info type,
                                           const std::meta::info context) {
  if (!detail::toolbox_problem(type, context).empty()) {
    return ^^detail::tool_set<>;
  }
  return detail::tool_set_of(detail::tool_functions_of(detail::plain_type(type)),
                             context);
}

/// Classes whose `[[= scrydantic::tool{...}]]` member functions bind() binds as
/// tools of one object.
///
/// A toolbox is a complete class that declares at least one tool member function
/// itself; inherited member functions are not considered. Every tool member must
/// be a public, non-deleted, non-`&&`-qualified function with a valid tool
/// signature, `const` when the toolbox is, and the tool names must be distinct.
/// A member with an explicit object parameter is instead called on the toolbox as
/// an lvalue, `const` when the toolbox is, which must initialize that parameter:
/// it may not be an rvalue reference, nor a non-const reference when the toolbox
/// is const, and a by-value parameter needs a toolbox it can copy.
/// Binding reports the first violation with the member's name and the reason.
/// @tparam Type Toolbox class, `const` for a const toolbox.
/// @tparam Context Call context a tool member function may take first, or void.
template <typename Type, typename Context = void>
concept Toolbox = detail::toolbox_problem(^^Type, ^^Context).empty();

} // namespace scrydantic
