#pragma once

#include <scrydantic/codec.hpp>
#include <scrydantic/error.hpp>
#include <scrydantic/json.hpp>
#include <string>
#include <string_view>
#include <utility>

// What Scry adds to Scrydantic's codec for its own wire types: an encode that
// returns text, and Scry's wording of a codec failure. Everything else in src/
// calls Scrydantic's public API directly.
namespace scry::detail {

// Encodes with scrydantic::encode_verbatim(), which skips the re-spelling that
// scrydantic::encode() applies to embedded payloads. Provider requests need
// this: the bytes of stored JSON payloads and the pre-spelled sampling numbers
// reach the body unchanged. Keys are still in canonical order, and strings still
// use the canonical escapes.
template <typename Type>
[[nodiscard]] scrydantic::Result<std::string> encode_text(const Type& value) {
  return scrydantic::encode_verbatim(value).transform(
      [](scrydantic::Json&& json) { return std::move(json.text); });
}

// Words a Scrydantic codec failure as a Scry diagnostic about `subject`. A failure
// located in the value carries `<path> <reason>` text, so it reads
// `Conversation document at $.messages[0].role is a required member`; a decode
// that found several issues has one such line for each, and the subject leads the
// first. A failure of the whole text, such as an encoded string that is not
// UTF-8, has no path and reads `<subject>: <reason>`.
[[nodiscard]] inline std::string
describe_codec_failure(const std::string_view subject, const scrydantic::Error& error) {
  std::string text{subject};
  text.append(error.message.starts_with('$') ? " at " : ": ").append(error.message);
  return text;
}

} // namespace scry::detail
