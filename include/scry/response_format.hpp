#pragma once

#include <scry/error.hpp>
#include <scry/events.hpp>
#include <scrydantic/codec.hpp>
#include <scrydantic/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace scry {

namespace detail {

/// Reports a strict decode of a typed answer as Scry reports a rejected answer:
/// ErrorCategory::invalid_argument, with Scrydantic's host and model texts.
[[nodiscard]] inline Error answer_error(scrydantic::Error error) {
  return Error{
      .category = ErrorCategory::invalid_argument,
      .message = std::move(error.message),
      .model_message = std::move(error.model_message),
  };
}

} // namespace detail

/// Builds the response format a typed turn asks for: the model answers with an
/// `Answer`.
///
/// The response tool is named `respond` and carries Scry's default description,
/// its input schema is scrydantic::input_schema_v<Answer>, and its validator
/// decodes each candidate answer strictly with scrydantic::decode<Answer>(). A
/// rejected answer is an ErrorCategory::invalid_argument error whose
/// model_message names the JSON path at fault and what the schema required there,
/// exactly as a reflected tool-argument failure does, so the model can correct
/// itself. Harness::send<Answer>() and Harness::ask<Answer>() use this; call it
/// directly to rename the response tool or reword its description before passing
/// it to Harness::send_structured().
///
/// The template is unconstrained so that a rejected type fails with a diagnostic
/// naming the offending member and the reason; scrydantic::ToolArguments is the
/// SFINAE-friendly form of the same check.
/// @tparam Answer Complete reflected answer aggregate satisfying
/// scrydantic::ToolArguments, since a provider's tool input is always a JSON
/// object.
/// @return The response format.
template <typename Answer> [[nodiscard]] ResponseFormat response_format() {
  constexpr std::string_view problem =
      scrydantic::detail::tool_arguments_diagnostic<Answer>();
  static_assert(problem.empty(), problem);
  if constexpr (!problem.empty()) {
    return {};
  } else {
    return ResponseFormat{
        .schema =
            scrydantic::Json{.text = std::string{scrydantic::input_schema_v<Answer>}},
        .validate = [](const scrydantic::Json& answer) -> Status {
          auto decoded = scrydantic::decode<Answer>(answer);
          if (!decoded) {
            return std::unexpected(detail::answer_error(std::move(decoded.error())));
          }
          return {};
        },
    };
  }
}

} // namespace scry
