#pragma once

#include <memory>
#include <optional>
#include <scry/error.hpp>
#include <scry/events.hpp>
#include <scrydantic/codec.hpp>
#include <scrydantic/diagnostics.hpp>
#include <scrydantic/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace scry {

namespace detail {

/// Reports a strict decode of a typed answer as Scry reports a rejected answer:
/// ErrorCategory::invalid_argument, with Scrydantic's host and model texts and
/// its structured issues.
[[nodiscard]] inline Error answer_error(scrydantic::Error error) {
  return Error{
      .category = ErrorCategory::invalid_argument,
      .message = std::move(error.message),
      .model_message = std::move(error.model_message),
      .issues = std::move(error.issues),
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
/// model_message names each JSON path at fault and what the schema, a constraint,
/// or a scrydantic::check validator required there, exactly as a reflected
/// tool-argument failure does, so the model can correct itself. Its `issues` list
/// the same problems as data. Harness::send<Answer>() and Harness::ask<Answer>() use
/// this; call it directly to rename the response tool or reword its description before
/// passing it to Harness::send_structured().
///
/// The template is unconstrained so that a rejected type fails with a diagnostic
/// naming the offending member and the reason; scrydantic::ToolArguments is the
/// SFINAE-friendly form of the same check.
/// @tparam Answer Complete reflected answer aggregate satisfying
/// scrydantic::ToolArguments, since a provider's tool input is always a JSON
/// object.
/// @return The response format.
template <typename Answer> [[nodiscard]] ResponseFormat response_format();

namespace detail {

/// The answer a typed turn's validator accepted, beside the JSON it was decoded
/// from. Harness::ask<Answer>() and Harness::send<Answer>() report it rather than
/// decoding the answer again: a second decode would run the Answer's
/// scrydantic::check validators a second time, and one that reads host state
/// could refuse an answer the turn has already committed.
template <typename Answer> struct AcceptedAnswer {
  scrydantic::Json json;
  Answer value;
};

/// response_format<Answer>(), keeping each answer its validator accepts in
/// `accepted` when that is not null. The validator runs on the host thread that
/// pumps the turn, so the slot needs no lock.
template <typename Answer>
[[nodiscard]] ResponseFormat keeping_response_format(
    std::shared_ptr<std::optional<AcceptedAnswer<Answer>>> accepted) {
  return ResponseFormat{
      .schema =
          scrydantic::Json{.text = std::string{scrydantic::input_schema_v<Answer>}},
      .validate = [accepted =
                       std::move(accepted)](const scrydantic::Json& answer) -> Status {
        auto decoded = scrydantic::decode<Answer>(answer);
        if (!decoded) {
          return std::unexpected(answer_error(std::move(decoded.error())));
        }
        if (accepted != nullptr) {
          accepted->emplace(
              AcceptedAnswer<Answer>{.json = answer, .value = std::move(*decoded)});
        }
        return {};
      },
  };
}

/// What a typed turn reports to the host: the answer beside its completion.
///
/// `accepted` is the slot that keeping_response_format<Answer>() filled for this
/// turn. Its value is used when it is the answer the completion carries, which it
/// always is for a turn that completed; otherwise the answer is decoded from
/// Completion::structured.
template <typename Answer>
[[nodiscard]] Result<Answered<Answer>>
answered(Result<Completion> completion,
         std::optional<AcceptedAnswer<Answer>>& accepted) {
  if (!completion) {
    return std::unexpected(std::move(completion.error()));
  }
  if (!completion->structured) {
    return std::unexpected(Error{
        .category = ErrorCategory::invalid_state,
        .message = "a typed turn completed without an answer",
    });
  }
  if (accepted && accepted->json.text == completion->structured->text) {
    return Answered<Answer>{
        .value = std::move(accepted->value),
        .completion = std::move(*completion),
    };
  }
  auto value = scrydantic::decode<Answer>(*completion->structured);
  if (!value) {
    return std::unexpected(answer_error(std::move(value.error())));
  }
  return Answered<Answer>{
      .value = std::move(*value),
      .completion = std::move(*completion),
  };
}

} // namespace detail

template <typename Answer> ResponseFormat response_format() {
  constexpr std::string_view problem = scrydantic::tool_arguments_diagnostic<Answer>();
  static_assert(problem.empty(), problem);
  if constexpr (!problem.empty()) {
    return {};
  } else {
    return detail::keeping_response_format<Answer>(nullptr);
  }
}

} // namespace scry
