#pragma once

#include <scrydantic/codec.hpp>
#include <scrydantic/detail/encode.hpp>
#include <scrydantic/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace scry::detail {

// Provider requests preserve stored JSON payload bytes and pre-spelled sampling
// numbers. Scrydantic's public encode() canonicalizes the whole document, so this
// one adapter uses its uncanonicalized encoder. All other reflected encoding and
// decoding uses the public API. Keep this dependency on Scrydantic internals
// isolated until it exposes a public verbatim encoding operation.
template <typename Type>
[[nodiscard]] scrydantic::Result<std::string> encode_text(const Type& value) {
  constexpr std::string_view problem = scrydantic::detail::encodable_diagnostic<Type>();
  static_assert(problem.empty(), problem);
  return scrydantic::detail::encode_value(value).transform(
      [](scrydantic::Json&& json) { return std::move(json.text); });
}

} // namespace scry::detail
