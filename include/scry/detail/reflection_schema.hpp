#pragma once

/**
 * @file reflection_schema.hpp
 * @brief Constant-evaluated canonical JSON Schema generation for reflected arguments.
 *
 * The generator emits Scry's closed provider-neutral JSON Schema 2020-12 subset
 * directly into compile-time character storage. Object keys, property names, and
 * required names are lexical; enum values retain declaration order. Nested aggregate
 * schemas are closed and inline, with no references, definitions, titles, or defaults.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <meta>
#include <optional>
#include <scry/detail/reflection_meta.hpp>
#include <string_view>
#include <type_traits>
#include <vector>

namespace scry::reflection::detail {

/// Appends bytes that are already valid JSON syntax to compile-time output.
/// @param output Schema buffer under construction.
/// @param value Literal bytes to append unchanged.
consteval void append_literal(std::vector<char>& output, const std::string_view value) {
  output.insert(output.end(), value.begin(), value.end());
}

/// Appends a lowercase `\\u00xx` escape for one ASCII control byte.
/// @param output Schema buffer under construction.
/// @param value Byte below 0x20 requiring a JSON escape.
consteval void append_hex_escape(std::vector<char>& output, const unsigned char value) {
  constexpr std::string_view hexadecimal = "0123456789abcdef";
  append_literal(output, "\\u00");
  output.push_back(hexadecimal[value >> 4U]);
  output.push_back(hexadecimal[value & 0x0FU]);
}

/// Appends a quoted JSON string with canonical control-character escaping.
/// @param output Schema buffer under construction.
/// @param value Unquoted bytes to encode.
consteval void append_quoted(std::vector<char>& output, const std::string_view value) {
  output.push_back('"');
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    switch (character) {
    case '"':
      append_literal(output, "\\\"");
      break;
    case '\\':
      append_literal(output, "\\\\");
      break;
    case '\b':
      append_literal(output, "\\b");
      break;
    case '\f':
      append_literal(output, "\\f");
      break;
    case '\n':
      append_literal(output, "\\n");
      break;
    case '\r':
      append_literal(output, "\\r");
      break;
    case '\t':
      append_literal(output, "\\t");
      break;
    default:
      if (byte < 0x20U) {
        append_hex_escape(output, byte);
      } else {
        output.push_back(character);
      }
      break;
    }
  }
  output.push_back('"');
}

/// Appends an unsigned decimal integer without allocation or locale dependence.
/// @param output Schema buffer under construction.
/// @param value Integer to render in base ten.
consteval void append_unsigned(std::vector<char>& output, std::uint64_t value) {
  std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 2U> digits{};
  std::size_t size = 0;
  do {
    digits[size] = static_cast<char>('0' + (value % 10U));
    ++size;
    value /= 10U;
  } while (value != 0U);
  while (size != 0U) {
    --size;
    output.push_back(digits[size]);
  }
}

/// Appends a signed decimal integer, including the full `int64_t` minimum.
/// @param output Schema buffer under construction.
/// @param value Integer to render in base ten.
consteval void append_signed(std::vector<char>& output, const std::int64_t value) {
  if (value >= 0) {
    append_unsigned(output, static_cast<std::uint64_t>(value));
    return;
  }
  output.push_back('-');
  const auto magnitude = static_cast<std::uint64_t>(-(value + 1)) + std::uint64_t{1};
  append_unsigned(output, magnitude);
}

/// Appends any supported integer using its signedness-correct decimal representation.
/// @tparam Integer Supported integral type no wider than 64 bits.
/// @param output Schema buffer under construction.
/// @param value Integer value to render.
template <typename Integer>
consteval void append_integer(std::vector<char>& output, const Integer value) {
  if constexpr (std::is_signed_v<Integer>) {
    append_signed(output, static_cast<std::int64_t>(value));
  } else {
    append_unsigned(output, static_cast<std::uint64_t>(value));
  }
}

/// Recognizes a specialization of scry::reflection::description in P3394 metadata.
/// @param annotation Candidate annotation reflection value.
/// @return true only for a Scry description annotation.
consteval bool is_description_annotation(const std::meta::info annotation) {
  if (!std::meta::is_annotation(annotation)) {
    return false;
  }
  const auto type = std::meta::type_of(annotation);
  return std::meta::has_template_arguments(type) &&
         std::meta::template_of(type) == ^^description;
}

/// Counts Scry description annotations attached to one reflected member.
/// @tparam Member Reflection value naming a non-static data member.
/// @return Number of matching annotations.
template <std::meta::info Member> consteval std::size_t description_annotation_count() {
  std::size_t count = 0;
  static constexpr auto annotations =
      std::define_static_array(std::meta::annotations_of(Member));
  template for (constexpr std::meta::info annotation : annotations) {
    if constexpr (is_description_annotation(annotation)) {
      ++count;
    }
  }
  return count;
}

/// Appends a JSON Schema `description` member while maintaining object punctuation.
/// @param output Schema buffer under construction.
/// @param text Description text to quote and emit.
/// @param needs_comma Whether an earlier object member exists; set true on return.
consteval void append_description_key(std::vector<char>& output,
                                      const std::string_view text, bool& needs_comma) {
  if (needs_comma) {
    output.push_back(',');
  }
  append_literal(output, "\"description\":");
  append_quoted(output, text);
  needs_comma = true;
}

/// Emits the complete schema for one supported reflected value.
/// @tparam Type Supported value type whose shape determines the schema.
/// @param output Schema buffer under construction.
/// @param description_text Optional member-level description to attach to this schema.
template <typename Type>
consteval void append_schema(std::vector<char>& output,
                             std::optional<std::string_view> description_text);

/// Emits one aggregate member's schema and optional P3394 description.
///
/// More than one Scry description annotation produces a stable compile-time diagnostic.
/// @tparam Member Reflection value naming the member to describe.
/// @param output Schema buffer under construction.
template <std::meta::info Member>
consteval void append_member_schema(std::vector<char>& output) {
  using MemberType = [:std::meta::type_of(Member):];
  static_assert(description_annotation_count<Member>() <= 1,
                "a reflected member may have at most one "
                "scry::reflection::description annotation");

  static constexpr auto annotations =
      std::define_static_array(std::meta::annotations_of(Member));
  bool emitted = false;
  template for (constexpr std::meta::info annotation : annotations) {
    if constexpr (is_description_annotation(annotation)) {
      using Annotation = [:std::meta::type_of(annotation):];
      constexpr auto value = std::meta::extract<Annotation>(annotation);
      append_schema<MemberType>(output, value.view());
      emitted = true;
    }
  }
  if (!emitted) {
    append_schema<MemberType>(output, std::nullopt);
  }
}

/// Emits a closed inline object schema for a reflected aggregate.
///
/// Properties and required names use lexical member order. A member is omitted from
/// `required` only when its C++ declaration has a default member initializer; JSON
/// nullability is independently represented by optional schemas.
/// @tparam Type Supported aggregate type.
/// @param output Schema buffer under construction.
/// @param description_text Optional description inherited from the containing member.
template <typename Type>
consteval void
append_aggregate_schema(std::vector<char>& output,
                        const std::optional<std::string_view> description_text) {
  output.push_back('{');
  append_literal(output, "\"additionalProperties\":false");
  if (description_text.has_value()) {
    append_literal(output, ",\"description\":");
    append_quoted(output, *description_text);
  }
  append_literal(output, ",\"properties\":{");

  bool first = true;
  static constexpr auto members = sorted_members_of<Type>();
  template for (constexpr std::meta::info member : members) {
    if (!first) {
      output.push_back(',');
    }
    append_quoted(output, std::meta::identifier_of(member));
    output.push_back(':');
    append_member_schema<member>(output);
    first = false;
  }

  append_literal(output, "},\"required\":[");
  first = true;
  template for (constexpr std::meta::info member : members) {
    if constexpr (!std::meta::has_default_member_initializer(member)) {
      if (!first) {
        output.push_back(',');
      }
      append_quoted(output, std::meta::identifier_of(member));
      first = false;
    }
  }
  append_literal(output, "],\"type\":\"object\"}");
}

/// Emits a scalar schema containing only optional description and JSON type.
/// @param output Schema buffer under construction.
/// @param description_text Optional member description.
/// @param type JSON Schema primitive type name.
consteval void
append_described_type(std::vector<char>& output,
                      const std::optional<std::string_view> description_text,
                      const std::string_view type) {
  output.push_back('{');
  bool needs_comma = false;
  if (description_text.has_value()) {
    append_description_key(output, *description_text, needs_comma);
  }
  if (needs_comma) {
    output.push_back(',');
  }
  append_literal(output, "\"type\":");
  append_quoted(output, type);
  output.push_back('}');
}

/// Emits an integer schema with exact C++ minimum and maximum bounds.
/// @tparam Integer Supported integral type supplying numeric limits.
/// @param output Schema buffer under construction.
/// @param description_text Optional member description.
template <typename Integer>
consteval void
append_integer_schema(std::vector<char>& output,
                      const std::optional<std::string_view> description_text) {
  output.push_back('{');
  bool needs_comma = false;
  if (description_text.has_value()) {
    append_description_key(output, *description_text, needs_comma);
  }
  if (needs_comma) {
    output.push_back(',');
  }
  append_literal(output, "\"maximum\":");
  append_integer(output, std::numeric_limits<Integer>::max());
  append_literal(output, ",\"minimum\":");
  append_integer(output, std::numeric_limits<Integer>::lowest());
  append_literal(output, ",\"type\":\"integer\"}");
}

/// Emits a string enum schema preserving C++ enumerator declaration order.
/// @tparam Enum Supported scoped enum with unique values.
/// @param output Schema buffer under construction.
/// @param description_text Optional member description.
template <typename Enum>
consteval void
append_enum_schema(std::vector<char>& output,
                   const std::optional<std::string_view> description_text) {
  output.push_back('{');
  bool needs_comma = false;
  if (description_text.has_value()) {
    append_description_key(output, *description_text, needs_comma);
  }
  if (needs_comma) {
    output.push_back(',');
  }
  append_literal(output, "\"enum\":[");
  bool first = true;
  static constexpr auto enumerators = declared_enumerators_of<Enum>();
  template for (constexpr std::meta::info enumerator : enumerators) {
    if (!first) {
      output.push_back(',');
    }
    append_quoted(output, std::meta::identifier_of(enumerator));
    first = false;
  }
  append_literal(output, "],\"type\":\"string\"}");
}

/// Emits a nullable `anyOf` schema for one supported optional layer.
///
/// The description belongs to the optional member as a whole, not only its non-null
/// alternative.
/// @tparam Optional Recognized supported optional specialization.
/// @param output Schema buffer under construction.
/// @param description_text Optional member description.
template <typename Optional>
consteval void
append_optional_schema(std::vector<char>& output,
                       const std::optional<std::string_view> description_text) {
  using Element = typename optional_traits<Optional>::value_type;
  append_literal(output, "{\"anyOf\":[");
  append_schema<Element>(output, std::nullopt);
  append_literal(output, ",{\"type\":\"null\"}]");
  if (description_text.has_value()) {
    append_literal(output, ",\"description\":");
    append_quoted(output, *description_text);
  }
  output.push_back('}');
}

/// Starts an array schema and emits its optional description plus `items` key.
/// @param output Schema buffer under construction.
/// @param description_text Optional member description.
consteval void
append_sequence_prefix(std::vector<char>& output,
                       const std::optional<std::string_view> description_text) {
  output.push_back('{');
  if (description_text.has_value()) {
    append_literal(output, "\"description\":");
    append_quoted(output, *description_text);
    output.push_back(',');
  }
  append_literal(output, "\"items\":");
}

/// Emits an unbounded array schema for a supported vector specialization.
/// @tparam Vector Recognized vector whose element schema is recursively supported.
/// @param output Schema buffer under construction.
/// @param description_text Optional member description.
template <typename Vector>
consteval void
append_vector_schema(std::vector<char>& output,
                     const std::optional<std::string_view> description_text) {
  using Element = typename vector_traits<Vector>::value_type;
  append_sequence_prefix(output, description_text);
  append_schema<Element>(output, std::nullopt);
  append_literal(output, ",\"type\":\"array\"}");
}

/// Emits an exact-length array schema for a supported `std::array` specialization.
/// @tparam Array Recognized fixed array whose element schema is recursively supported.
/// @param output Schema buffer under construction.
/// @param description_text Optional member description.
template <typename Array>
consteval void
append_array_schema(std::vector<char>& output,
                    const std::optional<std::string_view> description_text) {
  using Element = typename array_traits<Array>::value_type;
  append_sequence_prefix(output, description_text);
  append_schema<Element>(output, std::nullopt);
  append_literal(output, ",\"maxItems\":");
  append_unsigned(output, array_traits<Array>::size);
  append_literal(output, ",\"minItems\":");
  append_unsigned(output, array_traits<Array>::size);
  append_literal(output, ",\"type\":\"array\"}");
}

// Dispatches through the SupportedValue classification documented on the declaration
// above. The static assertion supplies a stable diagnostic if an internal caller
// bypasses the public concept.
template <typename Type>
consteval void append_schema(std::vector<char>& output,
                             const std::optional<std::string_view> description_text) {
  using Value = std::remove_cvref_t<Type>;
  static_assert(SupportedValue<Value>,
                "reflection schema generation received an unsupported type");

  if constexpr (std::same_as<Value, bool>) {
    append_described_type(output, description_text, "boolean");
  } else if constexpr (is_supported_integer_v<Value>) {
    append_integer_schema<Value>(output, description_text);
  } else if constexpr (is_supported_float_v<Value>) {
    append_described_type(output, description_text, "number");
  } else if constexpr (std::same_as<Value, std::string>) {
    append_described_type(output, description_text, "string");
  } else if constexpr (is_supported_enum_v<Value>) {
    append_enum_schema<Value>(output, description_text);
  } else if constexpr (optional_traits<Value>::recognized) {
    append_optional_schema<Value>(output, description_text);
  } else if constexpr (vector_traits<Value>::recognized) {
    append_vector_schema<Value>(output, description_text);
  } else if constexpr (array_traits<Value>::recognized) {
    append_array_schema<Value>(output, description_text);
  } else {
    append_aggregate_schema<Value>(output, description_text);
  }
}

/// Materializes one argument schema in compiler-managed static string storage.
/// @tparam Args Complete reflected tool-argument aggregate.
/// @return View with static lifetime over minified canonical schema bytes.
template <ToolArguments Args> consteval std::string_view make_input_schema() {
  std::vector<char> output{};
  append_schema<Args>(output, std::nullopt);
  const auto* storage = std::define_static_string(output);
  return {storage, output.size()};
}

} // namespace scry::reflection::detail

namespace scry::reflection {

/// Canonical provider-neutral JSON Schema generated for a reflected argument aggregate.
///
/// The value is produced entirely during constant evaluation and has static storage
/// duration. It is the exact schema text passed to explicit ToolRegistry registration
/// by reflection::add(); there is no runtime schema cache or alternate representation.
/// @tparam Args Type satisfying ToolArguments.
template <ToolArguments Args>
inline constexpr std::string_view input_schema_v = detail::make_input_schema<Args>();

} // namespace scry::reflection
