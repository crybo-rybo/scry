#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <meta>
#include <optional>
#include <scry/detail/reflection_json_string.hpp>
#include <scry/detail/reflection_meta.hpp>
#include <scry/detail/reflection_model.hpp>
#include <string_view>
#include <vector>

// Schema generation is a consteval function of a type's reflection. It runs only
// for types the SupportedValue check has admitted.
namespace scry::reflection::detail {

using schema_text = std::optional<std::string_view>;

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

// Schema keys are emitted in sorted order, so "description" comes first in most
// schemas but after the "additionalProperties" of an aggregate and the "anyOf" of an
// optional or a variant. These two helpers cover both placements.

consteval void append_schema_prologue(std::vector<char>& output,
                                      const schema_text description_text) {
  output.push_back('{');
  if (description_text.has_value()) {
    append_json_literal(output, "\"description\":");
    append_json_string(output, *description_text);
    output.push_back(',');
  }
}

consteval void append_description_key(std::vector<char>& output,
                                      const schema_text description_text) {
  if (!description_text.has_value()) {
    return;
  }
  append_json_literal(output, ",\"description\":");
  append_json_string(output, *description_text);
}

consteval void append_schema(std::vector<char>& output, std::meta::info type,
                             schema_text description_text);

consteval void append_described_type(std::vector<char>& output,
                                     const schema_text description_text,
                                     const std::string_view type) {
  append_schema_prologue(output, description_text);
  append_json_literal(output, "\"type\":");
  append_json_string(output, type);
  output.push_back('}');
}

consteval void append_integer_schema(std::vector<char>& output,
                                     const std::meta::info type,
                                     const schema_text description_text) {
  const auto bits = std::meta::size_of(type) * 8U;
  const bool is_signed = std::meta::is_signed_type(type);
  const auto magnitude_bits = is_signed ? bits - 1U : bits;
  const auto maximum = magnitude_bits == 64U
                           ? std::numeric_limits<std::uint64_t>::max()
                           : (std::uint64_t{1} << magnitude_bits) - 1U;
  append_schema_prologue(output, description_text);
  append_json_literal(output, "\"maximum\":");
  append_unsigned(output, maximum);
  append_json_literal(output, ",\"minimum\":");
  if (is_signed) {
    // The lowest value is -(maximum + 1), whose magnitude an unsigned holds.
    output.push_back('-');
    append_unsigned(output, maximum + 1U);
  } else {
    output.push_back('0');
  }
  append_json_literal(output, ",\"type\":\"integer\"}");
}

consteval void append_enum_schema(std::vector<char>& output, const std::meta::info type,
                                  const schema_text description_text) {
  append_schema_prologue(output, description_text);
  append_json_literal(output, "\"enum\":[");
  bool first = true;
  for (const auto enumerator : std::meta::enumerators_of(type)) {
    if (!first) {
      output.push_back(',');
    }
    append_json_string(output, std::meta::identifier_of(enumerator));
    first = false;
  }
  append_json_literal(output, "],\"type\":\"string\"}");
}

consteval void append_sequence_schema(std::vector<char>& output,
                                      const std::meta::info type,
                                      const schema_text description_text) {
  append_schema_prologue(output, description_text);
  append_json_literal(output, "\"items\":");
  append_schema(output, element_of(type), std::nullopt);
  if (kind_of(type) == value_kind::array) {
    const auto size =
        std::meta::extract<std::size_t>(std::meta::template_arguments_of(type)[1]);
    append_json_literal(output, ",\"maxItems\":");
    append_unsigned(output, size);
    append_json_literal(output, ",\"minItems\":");
    append_unsigned(output, size);
  }
  append_json_literal(output, ",\"type\":\"array\"}");
}

// The "type" property a tagged alternative's schema gains.
consteval void append_tag_property(std::vector<char>& output,
                                   const std::string_view tag_text) {
  append_json_string(output, tag_key);
  append_json_literal(output, ":{\"enum\":[");
  append_json_string(output, tag_text);
  append_json_literal(output, "],\"type\":\"string\"}");
}

// Writes the `properties` members, or with `required_only` the `required` names,
// merging the tag key into its sorted position when there is one.
consteval void append_object_keys(std::vector<char>& output, const std::meta::info type,
                                  const schema_text tag_text,
                                  const bool required_only) {
  bool first = true;
  bool tag_pending = tag_text.has_value();
  const auto separate = [&output, &first] {
    if (!first) {
      output.push_back(',');
    }
    first = false;
  };
  const auto flush_tag = [&](const bool before) {
    if (tag_pending && before) {
      separate();
      if (required_only) {
        append_json_string(output, tag_key);
      } else {
        append_tag_property(output, *tag_text);
      }
      tag_pending = false;
    }
  };
  for (const auto& field : fields_of(type)) {
    flush_tag(field.key_view() > tag_key);
    if (required_only && !field.required) {
      continue;
    }
    separate();
    append_json_string(output, field.key_view());
    if (!required_only) {
      output.push_back(':');
      append_schema(output, std::meta::type_of(field.member),
                    annotated_text(field.member, ^^description));
    }
  }
  flush_tag(true);
}

consteval void append_object_schema(std::vector<char>& output,
                                    const std::meta::info type,
                                    const schema_text description_text,
                                    const schema_text tag_text) {
  append_json_literal(output, "{\"additionalProperties\":false");
  append_description_key(output, description_text);
  append_json_literal(output, ",\"properties\":{");
  append_object_keys(output, type, tag_text, false);
  append_json_literal(output, "},\"required\":[");
  append_object_keys(output, type, tag_text, true);
  append_json_literal(output, "],\"type\":\"object\"}");
}

consteval void append_alternatives(std::vector<char>& output,
                                   const std::meta::info variant) {
  bool first = true;
  for (const auto alternative : alternatives_of(variant)) {
    if (!first) {
      output.push_back(',');
    }
    append_object_schema(output, plain_type(alternative), std::nullopt,
                         tag_of(alternative));
    first = false;
  }
}

// An optional variant joins null to the variant's own anyOf rather than nesting one
// anyOf inside another.
consteval void append_optional_schema(std::vector<char>& output,
                                      const std::meta::info type,
                                      const schema_text description_text) {
  const auto element = plain_type(element_of(type));
  append_json_literal(output, "{\"anyOf\":[");
  if (kind_of(element) == value_kind::variant) {
    append_alternatives(output, element);
  } else {
    append_schema(output, element, std::nullopt);
  }
  append_json_literal(output, ",{\"type\":\"null\"}]");
  append_description_key(output, description_text);
  output.push_back('}');
}

consteval void append_variant_schema(std::vector<char>& output,
                                     const std::meta::info type,
                                     const schema_text description_text) {
  append_json_literal(output, "{\"anyOf\":[");
  append_alternatives(output, type);
  output.push_back(']');
  append_description_key(output, description_text);
  output.push_back('}');
}

consteval void append_schema(std::vector<char>& output, const std::meta::info reflected,
                             const schema_text description_text) {
  const auto type = plain_type(reflected);
  switch (kind_of(type)) {
  case value_kind::boolean:
    return append_described_type(output, description_text, "boolean");
  case value_kind::integer:
    return append_integer_schema(output, type, description_text);
  case value_kind::floating:
    return append_described_type(output, description_text, "number");
  case value_kind::string:
    return append_described_type(output, description_text, "string");
  case value_kind::enumeration:
    return append_enum_schema(output, type, description_text);
  case value_kind::optional:
    return append_optional_schema(output, type, description_text);
  case value_kind::vector:
  case value_kind::array:
    return append_sequence_schema(output, type, description_text);
  case value_kind::variant:
    return append_variant_schema(output, type, description_text);
  case value_kind::aggregate:
    return append_object_schema(output, type, description_text, std::nullopt);
  default:
    // Unreachable: the SupportedValue check admits no other kind.
    throw std::meta::exception(u8"reflected schema generation reached a type "
                               u8"outside SupportedValue",
                               type);
  }
}

consteval std::string_view make_schema(const std::meta::info type) {
  std::vector<char> output{};
  append_schema(output, type, std::nullopt);
  const auto* storage = std::define_static_string(output);
  return {storage, output.size()};
}

template <typename Value> consteval std::string_view checked_schema() {
  constexpr std::string_view problem = supported_diagnostic<Value>();
  static_assert(problem.empty(), problem);
  if constexpr (problem.empty()) {
    return make_schema(^^Value);
  } else {
    return {};
  }
}

template <typename Args> consteval std::string_view checked_input_schema() {
  constexpr std::string_view problem = tool_arguments_diagnostic<Args>();
  static_assert(problem.empty(), problem);
  if constexpr (problem.empty()) {
    return make_schema(^^Args);
  } else {
    return {};
  }
}

} // namespace scry::reflection::detail

namespace scry::reflection {

/// Canonical provider-neutral JSON Schema generated for any supported reflected value.
///
/// Handler result types are supported roots too. Scry never sends a result schema to a
/// model; it is here so a host can export its own tool contracts. A type outside
/// SupportedValue fails to compile with the offending member path and the reason.
/// @tparam Value Type satisfying SupportedValue.
template <typename Value>
inline constexpr std::string_view schema_v = detail::checked_schema<Value>();

/// Canonical provider-neutral JSON Schema generated for a reflected argument aggregate.
/// @tparam Args Type satisfying ToolArguments.
template <typename Args>
inline constexpr std::string_view input_schema_v = detail::checked_input_schema<Args>();

} // namespace scry::reflection
