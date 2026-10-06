#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <meta>
#include <optional>
#include <scry/detail/reflection_codec.hpp>
#include <scry/detail/reflection_json_string.hpp>
#include <scry/detail/reflection_meta.hpp>
#include <scry/detail/reflection_model.hpp>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

// Strict reflected decoding. Every function here is reached only through an entry
// point that has already checked the type against a value family, so none of them
// repeats the check.
namespace scry::reflection::detail {

template <typename Type>
[[nodiscard]] codec_result<Type> read_value(const JsonView& view);

template <typename Integer>
[[nodiscard]] codec_result<Integer> read_integer(const JsonView& view) {
  const auto narrow = [](const auto value) -> codec_result<Integer> {
    if (!std::in_range<Integer>(value)) {
      return codec_fail("is outside the integer range");
    }
    return static_cast<Integer>(value);
  };
  if (const auto value = view.signed_integer()) {
    return narrow(*value);
  }
  if (const auto value = view.unsigned_integer()) {
    return narrow(*value);
  }
  return codec_fail("must be an integer");
}

template <typename Float>
[[nodiscard]] codec_result<Float> read_float(const JsonView& view) {
  long double value = 0.0L;
  switch (view.kind()) {
  case JsonKind::signed_integer:
    value = static_cast<long double>(*view.signed_integer());
    break;
  case JsonKind::unsigned_integer:
    value = static_cast<long double>(*view.unsigned_integer());
    break;
  case JsonKind::number:
    value = static_cast<long double>(*view.number());
    break;
  default:
    return codec_fail("must be a number");
  }

  const auto maximum = static_cast<long double>(std::numeric_limits<Float>::max());
  if (!std::isfinite(value) || value < -maximum || value > maximum) {
    return codec_fail("must be a finite in-range number");
  }
  const auto converted = static_cast<Float>(value);
  if (!std::isfinite(converted) || (value != 0.0L && converted == Float{0})) {
    return codec_fail("must be a finite in-range number");
  }
  return converted;
}

// The enumerator names are already in the generated schema's `enum` array, so
// repeating them in the failure keeps a retrying model from having to guess.
template <typename Enum>
[[nodiscard]] codec_result<Enum> read_enum(const JsonView& view) {
  if (view.kind() != JsonKind::string) {
    return codec_fail("must be an enumerator name");
  }
  const auto text = *view.string();
  std::optional<Enum> value{};
  template for (constexpr std::meta::info enumerator : enumerators_v<Enum>) {
    if (text == std::meta::identifier_of(enumerator)) {
      value = std::meta::extract<Enum>(std::meta::constant_of(enumerator));
    }
  }
  if (!value.has_value()) {
    return codec_fail(std::string{"is not a declared enumerator; must be one of: "} +
                      std::string{enumerator_names(^^Enum)});
  }
  return *value;
}

template <typename Type>
[[nodiscard]] bool is_declared_key(const std::string_view key) {
  static constexpr auto keys = [] {
    std::array<std::string_view, fields_v<Type>.size()> sorted{};
    for (std::size_t index = 0; index < sorted.size(); ++index) {
      sorted[index] = fields_v<Type>[index].key_view();
    }
    return sorted;
  }();
  return std::ranges::binary_search(keys, key);
}

// Tagged is true when the object is a variant alternative, whose discriminator
// member the variant has already read.
template <typename Type, bool Tagged>
[[nodiscard]] codec_result<void> reject_unknown_keys(const JsonView& view) {
  // key_at cannot be empty here: the view is an object and index is in range.
  for (std::size_t index = 0; index < view.size(); ++index) {
    const auto key = *view.key_at(index);
    if ((!Tagged || key != tag_key) && !is_declared_key<Type>(key)) {
      std::string reason{"contains unknown member "};
      append_json_string(reason, key);
      return codec_fail(std::move(reason));
    }
  }
  return {};
}

template <typename Type, bool Tagged>
[[nodiscard]] codec_result<Type> read_object(const JsonView& view) {
  if (view.kind() != JsonKind::object) {
    return codec_fail("must be an object");
  }
  if constexpr (!ignores_unknown(^^Type)) {
    if (auto known = reject_unknown_keys<Type, Tagged>(view); !known) {
      return std::unexpected(std::move(known.error()));
    }
  }

  Type object{};
  std::optional<codec_failure> failure{};
  template for (constexpr std::meta::info member : declared_members_v<Type>) {
    constexpr field_layout field = field_of(^^Type, member);
    constexpr std::string_view key = field.key_view();
    using Member = std::remove_cv_t<typename[:std::meta::type_of(member):]>;
    if (!failure.has_value()) {
      const auto found = view.find(key);
      if (found.has_value()) {
        auto decoded = read_value<Member>(*found);
        if (decoded) {
          object.[:member:] = std::move(*decoded);
        } else {
          failure = at_member(std::move(decoded.error()), key);
        }
      } else if constexpr (field.omit_null) {
        object.[:member:].reset();
      } else if constexpr (field.required) {
        failure = at_member(codec_failure{.reason = "is a required member"}, key);
      }
    }
  }

  if (failure.has_value()) {
    return std::unexpected(std::move(*failure));
  }
  return object;
}

template <typename Variant>
[[nodiscard]] codec_result<Variant> read_variant(const JsonView& view) {
  static constexpr std::string_view tags = tag_names(^^Variant);
  if (view.kind() != JsonKind::object) {
    return codec_fail("must be an object");
  }
  const auto discriminator = view.find(tag_key);
  if (!discriminator.has_value()) {
    return std::unexpected(at_member(
        codec_failure{.reason = std::string{"is a required member; must be one of: "} +
                                std::string{tags}},
        tag_key));
  }

  const auto selected = discriminator->string();
  std::optional<codec_result<Variant>> decoded{};
  template for (constexpr std::meta::info alternative : alternatives_v<Variant>) {
    using Alternative = typename[:alternative:];
    if (!decoded.has_value() && selected == *tag_of(alternative)) {
      decoded = read_object<Alternative, true>(view).transform([](Alternative&& value) {
        return Variant{std::in_place_type<Alternative>, std::move(value)};
      });
    }
  }
  if (!decoded.has_value()) {
    return std::unexpected(at_member(
        codec_failure{.reason = std::string{"must be one of: "} + std::string{tags}},
        tag_key));
  }
  return std::move(*decoded);
}

template <typename Optional>
[[nodiscard]] codec_result<Optional> read_optional(const JsonView& view) {
  if (view.kind() == JsonKind::null) {
    return Optional{std::nullopt};
  }
  return read_value<typename Optional::value_type>(view).transform(
      [](typename Optional::value_type&& value) { return Optional{std::move(value)}; });
}

template <typename Vector>
[[nodiscard]] codec_result<Vector> read_vector(const JsonView& view) {
  if (view.kind() != JsonKind::array) {
    return codec_fail("must be an array");
  }
  Vector values{};
  values.reserve(view.size());
  for (std::size_t index = 0; index < view.size(); ++index) {
    auto decoded = read_value<typename Vector::value_type>(*view.at(index));
    if (!decoded) {
      return std::unexpected(at_element(std::move(decoded.error()), index));
    }
    values.push_back(std::move(*decoded));
  }
  return values;
}

template <typename Array>
[[nodiscard]] codec_result<Array> read_array(const JsonView& view) {
  Array values{};
  if (view.kind() != JsonKind::array || view.size() != values.size()) {
    return codec_fail("must be an array of the declared fixed size");
  }
  for (std::size_t index = 0; index < values.size(); ++index) {
    auto decoded = read_value<typename Array::value_type>(*view.at(index));
    if (!decoded) {
      return std::unexpected(at_element(std::move(decoded.error()), index));
    }
    values[index] = std::move(*decoded);
  }
  return values;
}

// Booleans, strings, and raw JSON: the leaves that need no numeric range.
template <typename Type>
[[nodiscard]] codec_result<Type> read_plain_leaf(const JsonView& view) {
  if constexpr (std::same_as<Type, bool>) {
    if (view.kind() != JsonKind::boolean) {
      return codec_fail("must be a boolean");
    }
    return *view.boolean();
  } else if constexpr (std::same_as<Type, std::string>) {
    if (view.kind() != JsonKind::string) {
      return codec_fail("must be a string");
    }
    return std::string{*view.string()};
  } else {
    return view.to_json();
  }
}

template <typename Type> codec_result<Type> read_value(const JsonView& view) {
  constexpr auto kind = kind_of(^^Type);
  if constexpr (kind == value_kind::integer) {
    return read_integer<Type>(view);
  } else if constexpr (kind == value_kind::floating) {
    return read_float<Type>(view);
  } else if constexpr (kind == value_kind::enumeration) {
    return read_enum<Type>(view);
  } else if constexpr (kind == value_kind::optional) {
    return read_optional<Type>(view);
  } else if constexpr (kind == value_kind::vector) {
    return read_vector<Type>(view);
  } else if constexpr (kind == value_kind::array) {
    return read_array<Type>(view);
  } else if constexpr (kind == value_kind::variant) {
    return read_variant<Type>(view);
  } else if constexpr (kind == value_kind::aggregate) {
    return read_object<Type, false>(view);
  } else {
    return read_plain_leaf<Type>(view);
  }
}

// Decodes a value checked against a family, reporting failures as
// ErrorCategory::tool with a schema-derived model_message rooted at `path`.
template <typename Type>
[[nodiscard]] Result<Type> decode(const JsonView& view, const std::string& path = "$") {
  auto decoded = read_value<Type>(view);
  if (!decoded) {
    return std::unexpected(decode_failure_error(path, decoded.error()));
  }
  return std::move(*decoded);
}

template <ToolArguments Args>
[[nodiscard]] Result<Args> decode_arguments(const Json& input) {
  auto parsed = JsonView::parse(input);
  if (!parsed) {
    return std::unexpected(Error{
        .category = ErrorCategory::tool,
        .message = "reflected tool arguments are not valid JSON",
        .model_message = "tool arguments are not valid JSON",
    });
  }
  return decode<Args>(*parsed);
}

} // namespace scry::reflection::detail
