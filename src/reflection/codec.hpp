#pragma once

#include <scry/detail/reflection_codec.hpp>
#include <scry/detail/reflection_decode.hpp>
#include <scry/detail/reflection_encode.hpp>
#include <scry/detail/reflection_meta.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <utility>

// The library's own entry points into the reflected codec. The public
// reflection::encode() and decode() report a failure as a tool-facing Error; these
// return the codec's failure, a JSON path and a reason, so each caller words its
// own diagnostic under its own ErrorCategory.
namespace scry::detail {

using CodecFailure = reflection::detail::codec_failure;

template <typename Type> using CodecResult = reflection::detail::codec_result<Type>;

// The failure as `$.messages[0].role is a required member`.
[[nodiscard]] inline std::string describe(const CodecFailure& failure) {
  std::string text{"$"};
  text.append(failure.path);
  text.push_back(' ');
  text.append(failure.reason);
  return text;
}

// Decodes by the rules reflection::decode() applies.
template <typename Type>
[[nodiscard]] CodecResult<Type> decode_value(const JsonView& view) {
  constexpr std::string_view problem = reflection::detail::decodable_diagnostic<Type>();
  static_assert(problem.empty(), problem);
  return reflection::detail::read_value<Type>(view);
}

// Encodes without the canonicalizing pass reflection::encode() ends with. Keys are
// in canonical order and strings carry the canonical escapes, so the text is
// canonical apart from number spelling and the bytes of spliced scry::Json
// payloads, each of which the codec has validated.
template <typename Type>
[[nodiscard]] CodecResult<std::string> encode_text(const Type& value) {
  constexpr std::string_view problem = reflection::detail::encodable_diagnostic<Type>();
  static_assert(problem.empty(), problem);
  std::string output{};
  if (auto status = reflection::detail::write_value(output, value); !status) {
    return std::unexpected(std::move(status.error()));
  }
  return output;
}

} // namespace scry::detail
