#pragma once

#if !defined(__cpp_impl_reflection)
#error "Scry requires a compiler implementing P2996 (GCC 16 or newer)"
#endif

#include <scry/annotations.hpp>
#include <scry/detail/reflection_decode.hpp>
#include <scry/detail/reflection_encode.hpp>
#include <scry/detail/reflection_json.hpp>
#include <scry/detail/reflection_meta.hpp>
#include <scry/detail/reflection_schema.hpp>
#include <scry/error.hpp>
#include <scry/events.hpp>
#include <scry/json.hpp>
#include <scry/tool_registry.hpp>
#include <string>
#include <string_view>
#include <utility>

/// Scry's reflected codec and the vocabulary of reflected tools, built on P2996.
///
/// Tools themselves are registered through scry::ToolRegistry::add().
namespace scry::reflection {

/// Encodes a reflected value as Scry's canonical JSON.
///
/// This is the same value encoder used for reflected tool-handler results. The
/// returned text is canonical within the current Scry version but is not a
/// versioned archival format. A type outside Encodable fails to compile with the
/// offending member path and the reason.
/// @tparam Type Value from the Encodable family.
/// @param value Value to encode without modifying it.
/// @return Canonical JSON, or an ErrorCategory::tool codec error naming the path of a
/// non-finite number, an undeclared enumerator value, or an invalid scry::Json.
template <typename Type> [[nodiscard]] Result<Json> encode(const Type& value) {
  constexpr std::string_view problem = detail::encodable_diagnostic<Type>();
  static_assert(problem.empty(), problem);
  if constexpr (!problem.empty()) {
    return {};
  } else {
    return detail::encode_value(value).and_then(detail::canonicalize_encoded_json);
  }
}

/// Decodes a parsed JSON value into a reflected value, strictly.
///
/// The rules are the ones reflected tool arguments are decoded by. A failure is an
/// ErrorCategory::invalid_argument error whose `message` is `reflected JSON at `
/// followed by the JSON path of the offending value and what the type required
/// there, and whose `model_message` is that text without the prefix. A type outside
/// Decodable fails to compile with the offending member path and the reason.
/// @tparam Type Cv-unqualified value type from the Decodable family.
/// @param view Parsed JSON value to decode.
/// @return The decoded value, or the first decode failure.
template <typename Type> [[nodiscard]] Result<Type> decode(const JsonView& view) {
  constexpr std::string_view problem = detail::decodable_diagnostic<Type>();
  static_assert(problem.empty(), problem);
  if constexpr (!problem.empty()) {
    return std::unexpected(Error{});
  } else {
    auto decoded = detail::decode<Type>(view);
    if (!decoded) {
      decoded.error().category = ErrorCategory::invalid_argument;
    }
    return decoded;
  }
}

/// Parses JSON text and decodes it into a reflected value, strictly.
///
/// Text that is not JSON fails with ErrorCategory::invalid_argument and the messages
/// `reflected JSON text is not valid JSON` and `JSON text is not valid JSON`; every
/// other failure is reported as decode(const JsonView&) reports it.
/// @tparam Type Cv-unqualified value type from the Decodable family.
/// @param json JSON text to parse and decode.
/// @return The decoded value, or the parse or decode failure.
template <typename Type> [[nodiscard]] Result<Type> decode(const Json& json) {
  auto parsed = JsonView::parse(json);
  if (!parsed) {
    return std::unexpected(Error{
        .category = ErrorCategory::invalid_argument,
        .message = "reflected JSON text is not valid JSON",
        .model_message = "JSON text is not valid JSON",
    });
  }
  return decode<Type>(*parsed);
}

/// Builds the response format a typed turn asks for: the model answers with an
/// `Answer`.
///
/// The response tool is named `respond` and carries Scry's default description,
/// its input schema is input_schema_v<Answer>, and its validator decodes each
/// candidate answer strictly with decode<Answer>(). A rejected answer's
/// model_message names the JSON path at fault and what the schema required there,
/// exactly as a reflected tool-argument failure does, so the model can correct
/// itself. Harness::send<Answer>() and Harness::ask<Answer>() use this; call it
/// directly to rename the response tool or reword its description before passing
/// it to Harness::send().
///
/// The template is unconstrained so that a rejected type fails with a diagnostic
/// naming the offending member and the reason; ToolArguments is the
/// SFINAE-friendly form of the same check.
/// @tparam Answer Complete reflected answer aggregate satisfying ToolArguments,
/// since a provider's tool input is always a JSON object.
/// @return The response format.
template <typename Answer> [[nodiscard]] ResponseFormat response_format() {
  constexpr std::string_view problem = detail::tool_arguments_diagnostic<Answer>();
  static_assert(problem.empty(), problem);
  if constexpr (!problem.empty()) {
    return {};
  } else {
    return ResponseFormat{
        .schema = Json{.text = std::string{input_schema_v<Answer>}},
        .validate = [](const Json& answer) -> Status {
          auto decoded = reflection::decode<Answer>(answer);
          if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
          }
          return {};
        },
    };
  }
}

} // namespace scry::reflection
