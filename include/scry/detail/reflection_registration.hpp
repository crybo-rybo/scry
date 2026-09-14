#pragma once

#include <concepts>
#include <functional>
#include <scry/detail/reflection_codec.hpp>
#include <scry/detail/reflection_meta.hpp>
#include <scry/detail/reflection_schema.hpp>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/tool_registry.hpp>
#include <scry/unique_function.hpp>
#include <type_traits>
#include <utility>

namespace scry::reflection::detail {

template <typename Return>
  requires(supported_handler_result_impl<std::remove_cvref_t<Return>>())
[[nodiscard]] Result<Json> encode_handler_result(Return&& result) {
  using ResultType = std::remove_cvref_t<Return>;
  if constexpr (expected_traits<ResultType>::recognized) {
    using Value = typename expected_traits<ResultType>::value_type;
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
[[nodiscard]] Result<Json>
invoke_and_encode(Handler& handler, const ToolCallContext& context, Args args) {
  if constexpr (std::invocable<Handler&, const ToolCallContext&, Args>) {
    return encode_handler_result(std::invoke(handler, context, std::move(args)));
  } else {
    return encode_handler_result(std::invoke(handler, std::move(args)));
  }
}

template <ToolArguments Args, typename Handler>
  requires ToolHandlerFor<Handler, Args>
[[nodiscard]] ContextualToolHandler make_tool_handler(Handler&& handler) {
  using Callable = std::decay_t<Handler>;
  return ContextualToolHandler{
      [callable = Callable{std::forward<Handler>(handler)}](
          const ToolCallContext& context, Json input) mutable -> Result<Json> {
        auto arguments = decode_arguments<Args>(std::move(input));
        if (!arguments) {
          return std::unexpected(std::move(arguments.error()));
        }
        return invoke_and_encode<Args>(callable, context, std::move(*arguments));
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
/// @tparam Args Complete reflected argument aggregate.
/// @tparam Handler Move-constructible callable satisfying ToolHandlerFor<Handler,
/// Args>.
/// @param registry Harness-owned registry that receives the tool.
/// @param metadata Provider-visible tool name and description.
/// @param handler Callable invoked with Args moved by value, optionally preceded by
/// a const ToolCallContext& naming the call being serviced.
/// @return Success, or the explicit registry's immediate validation error.
template <ToolArguments Args, typename Handler>
  requires ToolHandlerFor<Handler, Args>
[[nodiscard]] Status add(ToolRegistry& registry, ToolMetadata metadata,
                         Handler&& handler) {
  return registry.add(
      ToolDefinition{
          .name = std::move(metadata.name),
          .description = std::move(metadata.description),
          .input_schema = Json{.text = std::string{input_schema_v<Args>}},
      },
      detail::make_tool_handler<Args>(std::forward<Handler>(handler)));
}

} // namespace scry::reflection
