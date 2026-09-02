#pragma once

/**
 * @file reflection_registration.hpp
 * @brief Typed-handler adaptation and lowering to the explicit ToolRegistry substrate.
 *
 * Registration owns no second runtime registry. These templates decode provider JSON
 * into Args, invoke the application handler by value on the update thread, encode its
 * typed result, and erase that wrapper into the ordinary C++23 ToolHandler.
 */

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

/// Encodes a reflected handler's direct value or Scry Result value.
///
/// An error already returned by the handler is propagated unchanged; a successful
/// expected value and a direct value both use the same reflected encoder.
/// @tparam Return Exact handler result type and value category.
/// @param result Handler result to inspect and encode.
/// @return Encoded JSON, or the handler/codec Error.
template <typename Return>
  requires(supported_handler_result_impl<Return>())
[[nodiscard]] Result<Json> encode_handler_result(Return&& result) {
  using ResultType = std::remove_cvref_t<Return>;
  if constexpr (expected_traits<ResultType>::recognized) {
    using Value = typename expected_traits<ResultType>::value_type;
    if (!result) {
      return std::unexpected(std::move(result.error()));
    }
    return encode_value<Value>(*result);
  } else {
    return encode_value<ResultType>(result);
  }
}

/// Invokes one typed handler with moved arguments and encodes its result.
/// @tparam Args Complete reflected tool-argument aggregate.
/// @tparam Handler Owned callable type satisfying ToolHandlerFor.
/// @param handler Mutable lvalue callable retained by the erased wrapper.
/// @param args Fully decoded arguments transferred into the handler.
/// @return Encoded handler value, or a handler/codec Error.
template <ToolArguments Args, typename Handler>
  requires ToolHandlerFor<Handler, Args>
[[nodiscard]] Result<Json> invoke_and_encode(Handler& handler, Args args) {
  decltype(auto) result = std::invoke(handler, std::move(args));
  return encode_handler_result<decltype(result)>(
      std::forward<decltype(result)>(result));
}

/// Builds the move-only JSON-to-JSON adapter stored by ToolRegistry.
///
/// Each invocation strictly parses/decodes before application code runs. Decode failure
/// skips the handler. The decayed callable is owned inside the returned ToolHandler, so
/// move-only captures remain supported and no application callable crosses to the
/// worker thread.
/// @tparam Args Complete reflected tool-argument aggregate.
/// @tparam Handler Callable type and value category supplied to registration.
/// @param handler Callable transferred into the erased wrapper.
/// @return Move-only explicit handler accepting Scry-owned Json.
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

/// Registers a typed reflected tool.
///
/// The argument schema is generated as input_schema_v<Args>, incoming JSON is decoded
/// strictly, and the typed return is encoded back to JSON. Registration lowers to the
/// same additive ToolRegistry used by explicit-schema tools. The callable consequently
/// executes synchronously on the Harness::update() thread and is snapshotted under the
/// same accepted-turn lifetime rules.
///
/// Args is passed to the handler as an rvalue, enabling ownership transfer from
/// members. Handler errors, decode errors, encode errors, and caught exceptions follow
/// the explicit tool-dispatch path as bounded model-visible tool results. Dynamic
/// schemas or unsupported return shapes should use ToolRegistry::add() directly.
/// @tparam Args Complete reflected argument aggregate.
/// @tparam Handler Move-constructible callable satisfying ToolHandlerFor<Handler,
/// Args>.
/// @param registry Harness-owned registry that receives the lowered tool.
/// @param metadata Provider-visible tool name and description.
/// @param handler Callable invoked with Args moved by value.
/// @return Success, or the explicit registry's immediate inactive/duplicate/validation
/// error. The metadata and handler are consumed by the attempt.
/// @see input_schema_v
/// @see encode
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
