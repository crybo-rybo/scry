#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <meta>
#include <optional>
#include <ranges>
#include <scry/detail/reflection_meta.hpp>
#include <string_view>
#include <type_traits>
#include <vector>

namespace scry::reflection::detail {

consteval void append_literal(std::vector<char>& output, const std::string_view value) {
  output.insert(output.end(), value.begin(), value.end());
}

consteval void append_hex_escape(std::vector<char>& output, const unsigned char value) {
  constexpr std::string_view hexadecimal = "0123456789abcdef";
  append_literal(output, "\\u00");
  output.push_back(hexadecimal[value >> 4U]);
  output.push_back(hexadecimal[value & 0x0FU]);
}

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

consteval void append_signed(std::vector<char>& output, const std::int64_t value) {
  if (value >= 0) {
    append_unsigned(output, static_cast<std::uint64_t>(value));
    return;
  }
  output.push_back('-');
  const auto magnitude = static_cast<std::uint64_t>(-(value + 1)) + std::uint64_t{1};
  append_unsigned(output, magnitude);
}

template <typename Integer>
consteval void append_integer(std::vector<char>& output, const Integer value) {
  if constexpr (std::is_signed_v<Integer>) {
    append_signed(output, static_cast<std::int64_t>(value));
  } else {
    append_unsigned(output, static_cast<std::uint64_t>(value));
  }
}

template <typename Type> consteval bool known_member_name(const std::string_view name) {
  bool known = false;
  static constexpr auto members = declared_members_of<Type>();
  template for (constexpr std::meta::info member : members) {
    if (name == std::meta::identifier_of(member)) {
      known = true;
    }
  }
  return known;
}

template <typename Type> consteval bool valid_tool_traits() {
  if constexpr (!requires { tool_traits<Type>::descriptions; }) {
    return false;
  } else {
    using Descriptions = std::remove_cvref_t<decltype(tool_traits<Type>::descriptions)>;
    if constexpr (!std::ranges::input_range<Descriptions> ||
                  !std::ranges::sized_range<Descriptions> ||
                  !std::ranges::random_access_range<Descriptions>) {
      return false;
    } else {
      using Entry = std::ranges::range_value_t<Descriptions>;
      if constexpr (!std::same_as<Entry, parameter_description>) {
        return false;
      } else {
        constexpr auto& descriptions = tool_traits<Type>::descriptions;
        for (std::size_t index = 0; index < std::ranges::size(descriptions); ++index) {
          if (!known_member_name<Type>(descriptions[index].member)) {
            return false;
          }
          for (std::size_t other = index + 1; other < std::ranges::size(descriptions);
               ++other) {
            if (descriptions[index].member == descriptions[other].member) {
              return false;
            }
          }
        }
        return true;
      }
    }
  }
}

template <typename Type, std::meta::info Member>
consteval std::optional<std::string_view> trait_description_for() {
  static_assert(valid_tool_traits<Type>(),
                "scry::reflection::tool_traits<T>::descriptions must be a range of "
                "parameter_description entries naming distinct reflected members");
  constexpr auto name = std::meta::identifier_of(Member);
  for (const auto& entry : tool_traits<Type>::descriptions) {
    if (entry.member == name) {
      return entry.text;
    }
  }
  return std::nullopt;
}

consteval bool is_description_annotation(const std::meta::info annotation) {
  if (!std::meta::is_annotation(annotation)) {
    return false;
  }
  const auto type = std::meta::type_of(annotation);
  return std::meta::has_template_arguments(type) &&
         std::meta::template_of(type) == ^^description;
}

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

consteval void append_description_key(std::vector<char>& output,
                                      const std::string_view text, bool& needs_comma) {
  if (needs_comma) {
    output.push_back(',');
  }
  append_literal(output, "\"description\":");
  append_quoted(output, text);
  needs_comma = true;
}

template <typename Type>
consteval void append_schema(std::vector<char>& output,
                             std::optional<std::string_view> description_text);

template <typename Owner, std::meta::info Member>
consteval void append_member_schema(std::vector<char>& output) {
  using MemberType = [:std::meta::type_of(Member):];
  static_assert(description_annotation_count<Member>() <= 1,
                "a reflected member may have at most one "
                "scry::reflection::description annotation");

  constexpr auto trait_description = trait_description_for<Owner, Member>();
  if constexpr (trait_description.has_value()) {
    append_schema<MemberType>(output, trait_description);
  } else {
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
}

template <typename Type>
consteval void
append_aggregate_schema(std::vector<char>& output,
                        const std::optional<std::string_view> description_text) {
  static_assert(valid_tool_traits<Type>(),
                "scry::reflection::tool_traits<T>::descriptions is invalid");

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
    append_member_schema<Type, member>(output);
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

template <typename Vector>
consteval void
append_vector_schema(std::vector<char>& output,
                     const std::optional<std::string_view> description_text) {
  using Element = typename vector_traits<Vector>::value_type;
  append_sequence_prefix(output, description_text);
  append_schema<Element>(output, std::nullopt);
  append_literal(output, ",\"type\":\"array\"}");
}

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

template <ToolArguments Args> consteval std::string_view make_input_schema() {
  std::vector<char> output{};
  append_schema<Args>(output, std::nullopt);
  const auto* storage = std::define_static_string(output);
  return {storage, output.size()};
}

} // namespace scry::reflection::detail

namespace scry::reflection {

template <ToolArguments Args>
inline constexpr std::string_view input_schema_v = detail::make_input_schema<Args>();

} // namespace scry::reflection
