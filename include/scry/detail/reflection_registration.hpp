#pragma once

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
  requires(supported_handler_result_impl<Return>())
[[nodiscard]] Result<Json> encode_handler_result(Return&& result) {
  using ResultType = std::remove_cvref_t<Return>;
  if constexpr (expected_traits<ResultType>::recognized) {
    using Value = typename expected_traits<ResultType>::value_type;
    if (!result) {
      return std::unexpected(std::move(result.error()));
    }
    return encode<Value>(*result);
  } else {
    return encode<ResultType>(result);
  }
}

template <ToolArguments Args, typename Handler>
  requires ToolHandlerFor<Handler, Args>
[[nodiscard]] Result<Json> invoke_and_encode(Handler& handler, Args args) {
  decltype(auto) result = std::invoke(handler, std::move(args));
  return encode_handler_result<decltype(result)>(
      std::forward<decltype(result)>(result));
}

template <ToolArguments Args, typename Handler>
  requires ToolHandlerFor<Handler, Args>
[[nodiscard]] ToolHandler make_tool_handler(Handler&& handler) {
  using Callable = std::decay_t<Handler>;
  return ToolHandler{[callable = Callable{std::forward<Handler>(handler)}](
                         Json input) mutable -> Result<Json> {
    auto arguments = decode_arguments<Args>(std::move(input));
    if (!arguments) {
      return std::unexpected(std::move(arguments.error()));
    }
    return invoke_and_encode<Args>(callable, std::move(*arguments));
  }};
}

} // namespace scry::reflection::detail

namespace scry::reflection {

template <ToolArguments Args, typename Handler>
  requires ToolHandlerFor<Handler, Args>
[[nodiscard]] Status add(ToolRegistry& registry, ToolMetadata metadata,
                         Handler&& handler, ToolRegistrationOptions options) {
  return registry.add(
      ToolDefinition{
          .name = std::move(metadata.name),
          .description = std::move(metadata.description),
          .input_schema = Json{.text = std::string{input_schema_v<Args>}},
      },
      detail::make_tool_handler<Args>(std::forward<Handler>(handler)), options);
}

template <ToolArguments Args, typename Handler>
  requires ToolHandlerFor<Handler, Args>
[[nodiscard]] Status add(ToolRegistry& registry, ToolMetadata metadata,
                         Handler&& handler) {
  return add<Args>(registry, std::move(metadata), std::forward<Handler>(handler),
                   ToolRegistrationOptions{});
}

} // namespace scry::reflection
