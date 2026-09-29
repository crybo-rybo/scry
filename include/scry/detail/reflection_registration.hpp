#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <meta>
#include <scry/detail/reflection_decode.hpp>
#include <scry/detail/reflection_encode.hpp>
#include <scry/detail/reflection_meta.hpp>
#include <scry/detail/reflection_model.hpp>
#include <scry/detail/reflection_schema.hpp>
#include <scry/detail/reflection_tools.hpp>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/tool.hpp>
#include <scry/unique_function.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Turns reflected tool declarations into the entries ToolRegistry stores: a
// generated schema plus a handler that decodes the arguments strictly, calls the
// C++ function, and encodes what it returns.
namespace scry::reflection::detail {

template <typename Return>
  requires(handler_result_problem<std::remove_cvref_t<Return>>({}).empty() &&
           !is_acknowledgement(^^Return))
[[nodiscard]] Result<Json> encode_handler_result(Return&& result) {
  using ResultType = std::remove_cvref_t<Return>;
  if constexpr (expected_traits<ResultType>::recognized) {
    // A Result<const T> is supported like a const T return; encode the T.
    using Value = std::remove_cv_t<typename expected_traits<ResultType>::value_type>;
    if (!result) {
      return std::unexpected(std::forward<Return>(result).error());
    }
    return encode_value<Value>(*result);
  } else {
    return encode_value<ResultType>(result);
  }
}

// What the model receives from a tool that returns void or a successful Status.
[[nodiscard]] inline Json acknowledgement() { return Json{.text = "{}"}; }

// Runs a tool body and turns whatever it returns into the tool result. The body
// is invoked exactly once, and its return value is encoded where it lands, with
// no extra copy or move.
template <typename Body> [[nodiscard]] Result<Json> run_tool(Body&& body) {
  using Return = decltype(std::forward<Body>(body)());
  if constexpr (std::is_void_v<Return>) {
    std::forward<Body>(body)();
    return acknowledgement();
  } else if constexpr (is_acknowledgement(^^Return)) {
    auto status = std::forward<Body>(body)();
    if (!status) {
      return std::unexpected(std::move(status).error());
    }
    return acknowledgement();
  } else {
    return encode_handler_result(std::forward<Body>(body)());
  }
}

template <ToolArguments Args, typename Handler>
  requires ToolHandlerFor<Handler, Args>
[[nodiscard]] ContextualToolHandler make_tool_handler(Handler&& handler) {
  using Callable = std::decay_t<Handler>;
  return ContextualToolHandler{
      [callable = Callable{std::forward<Handler>(handler)}](
          const ToolCallContext& context, Json input) mutable -> Result<Json> {
        auto arguments = decode_arguments<Args>(input);
        if (!arguments) {
          return std::unexpected(std::move(arguments.error()));
        }
        return run_tool([&]() -> decltype(auto) {
          // The same preference ToolHandlerFor applies: the contextual form wins.
          if constexpr (std::invocable<Callable&, const ToolCallContext&, Args>) {
            return std::invoke(callable, context, std::move(*arguments));
          } else {
            return std::invoke(callable, std::move(*arguments));
          }
        });
      }};
}

// ---- Reflected functions -----------------------------------------------------

// Calls a tool function, on `object` when it is a non-static member function.
template <std::meta::info Function, typename Object, typename... Values>
decltype(auto) call_on_object([[maybe_unused]] Object* object, Values&&... values) {
  if constexpr (std::meta::is_class_member(Function) &&
                !std::meta::is_static_member(Function)) {
    return object->[:Function:](std::forward<Values>(values)...);
  } else {
    return [:Function:](std::forward<Values>(values)...);
  }
}

// Calls a tool function with its value arguments, preceded by the context when
// it takes one. This is a function template rather than a generic lambda
// because GCC 16 instantiates both branches of an if constexpr on an enclosing
// template parameter inside a generic lambda.
template <std::meta::info Function, typename Object, typename... Values>
decltype(auto) call_tool_function(Object* object,
                                  [[maybe_unused]] const ToolCallContext& context,
                                  Values&&... values) {
  if constexpr (takes_context(Function)) {
    return call_on_object<Function>(object, context, std::forward<Values>(values)...);
  } else {
    return call_on_object<Function>(object, std::forward<Values>(values)...);
  }
}

// Spreads a decoded argument object over the function's parameter list: nothing,
// the whole object, or one synthesized member per parameter. `Args` is deduced
// rather than spelled tool_arguments_t<Function>, because GCC 16 cannot mangle a
// function template whose signature contains a splice.
template <std::meta::info Function, typename Object, typename Args>
decltype(auto) invoke_tool_function(Object* object, const ToolCallContext& context,
                                    [[maybe_unused]] Args& arguments) {
  constexpr auto form = arguments_form_of(Function);
  if constexpr (form == arguments_form::none) {
    return call_tool_function<Function>(object, context);
  } else if constexpr (form == arguments_form::aggregate) {
    return call_tool_function<Function>(object, context, std::move(arguments));
  } else {
    static constexpr auto members = declared_members_v<Args>;
    return [&]<std::size_t... Index>(std::index_sequence<Index...>) -> decltype(auto) {
      return call_tool_function<Function>(object, context,
                                          std::move(arguments.[:members[Index]:])...);
    }(std::make_index_sequence<members.size()>{});
  }
}

// The handler of one reflected function. `object` is the toolbox a member tool
// is bound to; the handler shares ownership of it, so the toolbox lives as long
// as the registration and every turn snapshot holding it.
template <std::meta::info Function, typename Object>
[[nodiscard]] ContextualToolHandler
make_function_handler(std::shared_ptr<Object> object) {
  using Args = tool_arguments_t<Function>;
  return ContextualToolHandler{
      [object = std::move(object)](const ToolCallContext& context,
                                   Json input) -> Result<Json> {
        auto arguments = decode_arguments<Args>(input);
        if (!arguments) {
          return std::unexpected(std::move(arguments.error()));
        }
        return run_tool([&]() -> decltype(auto) {
          return invoke_tool_function<Function>(object.get(), context, *arguments);
        });
      }};
}

template <std::meta::info Function, typename Object>
[[nodiscard]] scry::detail::ToolEntry
make_function_entry(std::shared_ptr<Object> object) {
  static constexpr std::string_view name = tool_name_of(Function);
  static constexpr std::string_view description = tool_description_of(Function);
  static constexpr std::string_view schema = input_schema_v<tool_arguments_t<Function>>;
  scry::detail::ToolEntry entry{};
  entry.definition.name = std::string{name};
  entry.definition.description = std::string{description};
  entry.definition.input_schema = Json{.text = std::string{schema}};
  entry.handler = make_function_handler<Function>(std::move(object));
  return entry;
}

// Every tool function of a toolbox class, in declaration order.
template <std::meta::info Scope, typename Object>
[[nodiscard]] std::vector<scry::detail::ToolEntry>
scope_entries(const std::shared_ptr<Object>& object) {
  std::vector<scry::detail::ToolEntry> entries{};
  constexpr std::size_t count = tool_functions_v<Scope>.size();
  entries.reserve(count);
  template for (constexpr std::meta::info function : tool_functions_v<Scope>) {
    entries.push_back(make_function_entry<function>(object));
  }
  return entries;
}

// The entries add<^^Entity>() registers: free functions, bound to no object.
// They depend on nothing but the functions named here, so every translation unit
// that registers the same set instantiates the same code.
template <std::meta::info... Functions>
[[nodiscard]] std::vector<scry::detail::ToolEntry>
tool_set_entries(tool_set<Functions...>) {
  std::vector<scry::detail::ToolEntry> entries{};
  entries.reserve(sizeof...(Functions));
  (entries.push_back(make_function_entry<Functions>(std::shared_ptr<void>{})), ...);
  return entries;
}

template <typename Toolbox>
[[nodiscard]] std::vector<scry::detail::ToolEntry>
toolbox_entries(const std::shared_ptr<Toolbox>& toolbox) {
  return scope_entries<plain_type(^^Toolbox)>(toolbox);
}

} // namespace scry::reflection::detail
