#pragma once

#include <concepts>
#include <functional>
#include <scry/detail/reflection_decode.hpp>
#include <scry/detail/reflection_encode.hpp>
#include <scry/detail/reflection_meta.hpp>
#include <scry/detail/reflection_schema.hpp>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/tool_registry.hpp>
#include <scry/unique_function.hpp>
#include <string_view>
#include <type_traits>
#include <utility>

namespace scry::reflection::detail {

template <typename Return>
  requires(handler_result_problem<std::remove_cvref_t<Return>>({}).empty())
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
        // The same preference ToolHandlerFor applies: the contextual form wins.
        if constexpr (std::invocable<Callable&, const ToolCallContext&, Args>) {
          return encode_handler_result(
              std::invoke(callable, context, std::move(*arguments)));
        } else {
          return encode_handler_result(std::invoke(callable, std::move(*arguments)));
        }
      }};
}

} // namespace scry::reflection::detail

namespace scry::reflection {

/// Registers a typed reflected tool.
///
/// The argument schema is generated as input_schema_v<Args>, incoming JSON is decoded
/// strictly, and the typed return is encoded back to JSON. The tool is stored in the
/// same additive ToolRegistry that explicit-schema tools use. A handler may take a
/// leading const ToolCallContext& to learn which turn, round, and call it is running
/// for; the context is borrowed for the invocation only.
///
/// The template is unconstrained so that a rejected type fails with a diagnostic
/// naming the offending member and the reason; ToolArguments and ToolHandlerFor are
/// the SFINAE-friendly form of the same checks.
/// @tparam Args Complete reflected argument aggregate satisfying ToolArguments.
/// @tparam Handler Move-constructible callable satisfying ToolHandlerFor<Handler,
/// Args>.
/// @param registry Registry that receives the tool.
/// @param metadata Provider-visible tool name and description.
/// @param handler Callable invoked with Args moved by value, optionally preceded by
/// a const ToolCallContext& naming the call being serviced.
/// @return Success, or the immediate error ToolRegistry::add() reports.
template <typename Args, typename Handler>
[[nodiscard]] Status add(ToolRegistry& registry, ToolMetadata metadata,
                         Handler&& handler) {
  constexpr std::string_view arguments_problem =
      detail::tool_arguments_diagnostic<Args>();
  static_assert(arguments_problem.empty(), arguments_problem);
  if constexpr (!arguments_problem.empty()) {
    return {};
  } else {
    constexpr std::string_view handler_problem =
        detail::tool_handler_diagnostic<Handler, Args>();
    static_assert(handler_problem.empty(), handler_problem);
    if constexpr (!handler_problem.empty()) {
      return {};
    } else {
      return registry.add(
          ToolDefinition{
              .name = std::move(metadata.name),
              .description = std::move(metadata.description),
              .input_schema = Json{.text = std::string{input_schema_v<Args>}},
          },
          detail::make_tool_handler<Args>(std::forward<Handler>(handler)));
    }
  }
}

} // namespace scry::reflection
