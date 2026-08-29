#pragma once

#include <string_view>

namespace scry::reflection::detail {

template <typename Output>
constexpr void append_json_literal(Output& output, const std::string_view value) {
  output.insert(output.end(), value.begin(), value.end());
}

template <typename Output>
constexpr void append_json_hex_escape(Output& output, const unsigned char value) {
  constexpr std::string_view hexadecimal = "0123456789abcdef";
  append_json_literal(output, "\\u00");
  output.push_back(hexadecimal[value >> 4U]);
  output.push_back(hexadecimal[value & 0x0FU]);
}

template <typename Output>
constexpr void append_json_string(Output& output, const std::string_view value) {
  output.push_back('"');
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    switch (character) {
    case '"':
      append_json_literal(output, "\\\"");
      break;
    case '\\':
      append_json_literal(output, "\\\\");
      break;
    case '\b':
      append_json_literal(output, "\\b");
      break;
    case '\f':
      append_json_literal(output, "\\f");
      break;
    case '\n':
      append_json_literal(output, "\\n");
      break;
    case '\r':
      append_json_literal(output, "\\r");
      break;
    case '\t':
      append_json_literal(output, "\\t");
      break;
    default:
      if (byte < 0x20U) {
        append_json_hex_escape(output, byte);
      } else {
        output.push_back(character);
      }
      break;
    }
  }
  output.push_back('"');
}

} // namespace scry::reflection::detail
