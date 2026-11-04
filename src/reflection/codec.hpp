#pragma once

#include <scrydantic/codec.hpp>
#include <scrydantic/detail/encode.hpp>
#include <scrydantic/error.hpp>
#include <scrydantic/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace scry::detail {

// Encodes without the canonicalizing pass that scrydantic::encode() ends with.
// Provider requests need this: the bytes of stored JSON payloads and the
// pre-spelled sampling numbers reach the body unchanged. Keys are still in
// canonical order, and strings still use the canonical escapes.
// TODO(https://github.com/crybo-rybo/scrydantic/issues/3): use a public verbatim
// encoder once Scrydantic exposes one.
template <typename Type>
[[nodiscard]] scrydantic::Result<std::string> encode_text(const Type& value) {
  constexpr std::string_view problem = scrydantic::detail::encodable_diagnostic<Type>();
  static_assert(problem.empty(), problem);
  return scrydantic::detail::encode_value(value).transform(
      [](scrydantic::Json&& json) { return std::move(json.text); });
}

// Words a Scrydantic codec failure as a Scry diagnostic about `subject`. A
// failure from decoding a JsonView reads `<subject> at $.messages[0].role is a
// required member`. An encode failure has no path-only text, so it reads
// `<subject>: reflected JSON at $.body is not valid JSON`.
// TODO(https://github.com/crybo-rybo/scrydantic/issues/7): word both the same
// way once encode failures carry the path-only text.
[[nodiscard]] inline std::string
describe_codec_failure(const std::string_view subject, const scrydantic::Error& error) {
  std::string text{subject};
  if (error.model_message.empty()) {
    text.append(": ").append(error.message);
  } else {
    text.append(" at ").append(error.model_message);
  }
  return text;
}

} // namespace scry::detail
