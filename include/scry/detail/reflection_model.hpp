#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <optional>
#include <scry/annotations.hpp>
#include <scry/detail/reflection_json_string.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The reflected type model shared by the family checks, the codec, and the schema
// generator. Everything here is a consteval function of std::meta::info, so a
// question such as "which JSON key does this member use" has one answer no matter
// which of those three asks it.
namespace scry::reflection::detail {

// The key under which a tagged variant alternative carries its discriminator.
inline constexpr std::string_view tag_key = "type";

// The closed set of shapes the codec knows. Leaves come first, so a check can split
// them from the composites with one comparison.
enum class value_kind : std::uint8_t {
  unsupported,
  boolean,
  integer,
  floating,
  string,
  string_view,
  json,
  enumeration,
  optional,
  vector,
  array,
  variant,
  aggregate,
};

// Strips aliases, references, and cv-qualifiers so reflections compare by identity.
consteval std::meta::info plain_type(const std::meta::info type) {
  return std::meta::dealias(std::meta::remove_cvref(std::meta::dealias(type)));
}

// ^^Template<Args> names a specialization without instantiating it, and GCC's
// class queries answer for the uninstantiated class until something needs it to
// be complete. size_of does, so it runs first; for an incomplete type it throws.
consteval bool is_complete_class(const std::meta::info type) {
  try {
    static_cast<void>(std::meta::size_of(type));
  } catch (const std::meta::exception&) {
    return false;
  }
  return std::meta::is_complete_type(type);
}

consteval bool is_specialization_of(const std::meta::info type,
                                    const std::meta::info template_reflection) {
  return std::meta::has_template_arguments(type) &&
         std::meta::template_of(type) == template_reflection;
}

consteval bool is_character_type(const std::meta::info type) {
  // Each reflection is parenthesized: ^^char && ... would parse as ^^(char&&).
  for (const auto character : {(^^char), (^^signed char), (^^unsigned char),
                               (^^wchar_t), (^^char8_t), (^^char16_t), (^^char32_t)}) {
    if (type == character) {
      return true;
    }
  }
  return false;
}

consteval value_kind template_kind_of(const std::meta::info type) {
  if (is_specialization_of(type, ^^std::optional)) {
    return value_kind::optional;
  }
  if (is_specialization_of(type, ^^std::vector)) {
    return value_kind::vector;
  }
  if (is_specialization_of(type, ^^std::array)) {
    return value_kind::array;
  }
  if (is_specialization_of(type, ^^std::variant)) {
    return value_kind::variant;
  }
  if (std::meta::is_class_type(type) && is_complete_class(type) &&
      std::meta::is_aggregate_type(type)) {
    return value_kind::aggregate;
  }
  return value_kind::unsupported;
}

// Classifies a type by shape alone. Whether the shape is admitted, and for which
// family, is the checker's decision, not this function's.
consteval value_kind kind_of(const std::meta::info reflected) {
  const auto type = plain_type(reflected);
  if (type == (^^bool)) {
    return value_kind::boolean;
  }
  if (std::meta::is_integral_type(type)) {
    return is_character_type(type) || std::meta::size_of(type) > sizeof(std::uint64_t)
               ? value_kind::unsupported
               : value_kind::integer;
  }
  if (type == (^^float) || type == (^^double)) {
    return value_kind::floating;
  }
  if (type == plain_type(^^std::string)) {
    return value_kind::string;
  }
  if (type == plain_type(^^std::string_view)) {
    return value_kind::string_view;
  }
  if (type == (^^scry::Json)) {
    return value_kind::json;
  }
  if (std::meta::is_enum_type(type)) {
    return value_kind::enumeration;
  }
  return template_kind_of(type);
}

consteval std::vector<std::meta::info> data_members_of(const std::meta::info type) {
  return std::meta::nonstatic_data_members_of(type,
                                              std::meta::access_context::unchecked());
}

// The first template argument of an optional, vector, or array.
consteval std::meta::info element_of(const std::meta::info container) {
  return std::meta::template_arguments_of(plain_type(container))[0];
}

consteval std::vector<std::meta::info> alternatives_of(const std::meta::info variant) {
  return std::meta::template_arguments_of(plain_type(variant));
}

// ---- Annotations -------------------------------------------------------------

// `what` reflects either an annotation type (skip_null_t) or an annotation class
// template (name), whose every specialization matches.
consteval bool annotation_matches(const std::meta::info annotation,
                                  const std::meta::info what) {
  const auto type = plain_type(std::meta::type_of(annotation));
  return std::meta::is_template(what) ? is_specialization_of(type, what) : type == what;
}

consteval std::size_t annotation_count(const std::meta::info entity,
                                       const std::meta::info what) {
  std::size_t count = 0;
  for (const auto annotation : std::meta::annotations_of(entity)) {
    if (annotation_matches(annotation, what)) {
      ++count;
    }
  }
  return count;
}

consteval bool has_annotation(const std::meta::info entity,
                              const std::meta::info what) {
  return annotation_count(entity, what) != 0;
}

// A fixed-string annotation's text, recovered without naming its extent: the
// annotation value becomes the template argument of a variable template that
// points at the text of that very template-parameter object, so the view has
// static storage duration. std::string_view itself is not structural, which is
// why the variable holds a pointer and a size.
struct static_text {
  const char* data{};
  std::size_t size{};
};

template <auto Annotation>
inline constexpr static_text annotation_text_v{Annotation.text,
                                               sizeof(Annotation.text) - 1};

consteval std::string_view annotation_text(const std::meta::info annotation) {
  const auto argument = std::meta::constant_of(annotation);
  const auto text = std::meta::extract<static_text>(
      std::meta::substitute(^^annotation_text_v, {
                                                     argument}));
  return {text.data, text.size};
}

consteval std::optional<std::string_view>
annotated_text(const std::meta::info entity,
               const std::meta::info template_reflection) {
  for (const auto annotation : std::meta::annotations_of(entity)) {
    if (annotation_matches(annotation, template_reflection)) {
      return annotation_text(annotation);
    }
  }
  return std::nullopt;
}

// The JSON key a data member is written under: its name annotation, or else its
// identifier.
consteval std::string_view json_key_of(const std::meta::info member) {
  if (const auto key = annotated_text(member, ^^name)) {
    return *key;
  }
  return std::meta::identifier_of(member);
}

consteval std::optional<std::string_view> tag_of(const std::meta::info type) {
  return annotated_text(plain_type(type), ^^tag);
}

consteval bool ignores_unknown(const std::meta::info type) {
  return has_annotation(plain_type(type), ^^ignore_unknown_t);
}

consteval bool omits_null(const std::meta::info owner, const std::meta::info member) {
  if (kind_of(std::meta::type_of(member)) != value_kind::optional) {
    return false;
  }
  if (has_annotation(member, ^^skip_null_t)) {
    return true;
  }
  return !has_annotation(member, ^^emit_null_t) && has_annotation(owner, ^^skip_null_t);
}

// ---- Field layout ------------------------------------------------------------

// Everything the codec and the schema generator need to know about one data
// member, resolved once. It is a structural type so a class's fields can be one
// static array that `template for` walks; string members point at static strings
// because std::string_view is not structural.
struct field_layout {
  std::meta::info member{};
  const char* key{};
  std::size_t key_size{};
  // Encoding omits the member while it is a disengaged optional, and decoding
  // reads its absence as disengaged.
  bool omit_null{};
  // Decoding rejects an input without the member.
  bool required{};

  [[nodiscard]] consteval std::string_view key_view() const noexcept {
    return {key, key_size};
  }
};

consteval field_layout field_of(const std::meta::info owner,
                                const std::meta::info member) {
  const auto key = json_key_of(member);
  const bool omit = omits_null(owner, member);
  return field_layout{
      .member = member,
      .key = std::define_static_string(key),
      .key_size = key.size(),
      .omit_null = omit,
      .required = !omit && !std::meta::has_default_member_initializer(member),
  };
}

// A class's fields in lexical order of their final JSON keys, which is the
// canonical order objects are written in.
consteval std::vector<field_layout> fields_of(const std::meta::info type) {
  std::vector<field_layout> fields{};
  for (const auto member : data_members_of(plain_type(type))) {
    fields.push_back(field_of(plain_type(type), member));
  }
  std::ranges::sort(fields, {},
                    [](const field_layout& field) { return field.key_view(); });
  return fields;
}

template <typename Type>
inline constexpr auto fields_v = std::define_static_array(fields_of(^^Type));

// Decoding visits members in declaration order, which decides the failure it
// reports when several members are wrong. This is an array of member reflections
// rather than a second array of field_layout: GCC 16 rejects a define_static_array
// of consteval-only class elements whose value equals one it already made, which
// the two orders of a one-member class would be.
template <typename Type>
inline constexpr auto declared_members_v =
    std::define_static_array(data_members_of(plain_type(^^Type)));

template <typename Variant>
inline constexpr auto alternatives_v =
    std::define_static_array(alternatives_of(^^Variant));

template <typename Enum>
inline constexpr auto enumerators_v =
    std::define_static_array(std::meta::enumerators_of(^^Enum));

// ---- Static text -------------------------------------------------------------

// Copies static text one character at a time. Under AddressSanitizer GCC 16
// cannot constant-fold the null checks std::string's pointer-taking constructor
// and append make on reflected static strings (identifiers, display strings,
// define_static_string arrays), so consteval code never hands them one.
consteval std::string owned_text(const std::string_view text) {
  std::string copy{};
  for (const char character : text) {
    copy.push_back(character);
  }
  return copy;
}

consteval std::string_view static_string(const std::string& text) {
  return {std::define_static_string(text), text.size()};
}

// `"key":`, escaped once at compile time so encoding a member appends one literal.
consteval std::string_view member_prefix(const std::string_view key) {
  std::string text{};
  append_json_string(text, key);
  text.push_back(':');
  return static_string(text);
}

// `"type":"<tag>"`, the member a tagged alternative's object gains.
consteval std::string_view tag_member(const std::meta::info alternative) {
  std::string text{};
  append_json_string(text, tag_key);
  text.push_back(':');
  append_json_string(text, *tag_of(alternative));
  return static_string(text);
}

consteval std::string_view joined_names(const std::vector<std::string_view>& names) {
  std::string text{};
  for (const auto entry : names) {
    if (!text.empty()) {
      text.append(", ");
    }
    text += owned_text(entry);
  }
  return static_string(text);
}

// The declared enumerator names, as the decode diagnostic lists them.
consteval std::string_view enumerator_names(const std::meta::info type) {
  std::vector<std::string_view> names{};
  for (const auto enumerator : std::meta::enumerators_of(plain_type(type))) {
    names.push_back(std::meta::identifier_of(enumerator));
  }
  return joined_names(names);
}

// The declared tags of a variant's alternatives, in declaration order.
consteval std::string_view tag_names(const std::meta::info variant) {
  std::vector<std::string_view> names{};
  for (const auto alternative : alternatives_of(variant)) {
    names.push_back(*tag_of(alternative));
  }
  return joined_names(names);
}

} // namespace scry::reflection::detail
