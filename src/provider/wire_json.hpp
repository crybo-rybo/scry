#pragma once

// Glaze-reflected encoding for the provider request wire structs. It sits outside
// the kernel because it reflects over request-shaped aggregates rather than
// parsing bytes; the kernel's own codec (kernel/json/codec.hpp) never includes it.

#include "kernel/error.hpp"

#include <glaze/glaze.hpp>
#include <scry/error.hpp>
#include <string>
#include <string_view>

namespace scry::detail {

// Stored JSON text spliced into a larger document as a nested value rather than
// re-parsed into a JsonValue first. A member of this type writes its bytes
// verbatim; a std::string_view member next to it writes the same bytes as a
// quoted, escaped JSON string.
using JsonText = glz::raw_json_view;

// Options for every wire write. Glaze's default writer has no \u00XX form for a
// control byte outside \b \f \n \r \t and puts two NUL bytes in its place, which
// is not JSON; this option writes the escape instead, as the kernel codec does.
struct JsonWriteOptions : glz::opts {
  bool escape_control_characters = true;
};
inline constexpr JsonWriteOptions json_write_options{};

// Encodes a typed wire aggregate straight to JSON text. Glaze reflects a plain
// aggregate member by member in declaration order, so a wire struct whose
// members are declared alphabetically leaves the encoder in the same canonical
// key order a JsonValue would have produced - without building the tree.
template <class Wire>
[[nodiscard]] Result<std::string>
write_wire_json(const Wire& wire, const ErrorCategory category,
                const std::string_view failure_message) {
  std::string text{};
  if (glz::write<json_write_options>(wire, text)) {
    return std::unexpected(make_error(category, std::string{failure_message}));
  }
  return text;
}

} // namespace scry::detail
