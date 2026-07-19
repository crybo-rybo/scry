#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <meta>
#include <optional>
#include <scry/detail/reflection_json.hpp>
#include <scry/detail/reflection_meta.hpp>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace scry::reflection::detail {

[[nodiscard]] inline Error codec_error(const std::string_view path,
                                       const std::string_view message) {
  std::string text{"reflected JSON at "};
  text.append(path);
  text.push_back(' ');
  text.append(message);
  return Error{
      .category = ErrorCategory::tool,
      .message = std::move(text),
  };
}

[[nodiscard]] inline std::string member_path(const std::string& path,
                                             const std::string_view member) {
  std::string result = path;
  result.push_back('.');
  result.append(member);
  return result;
}

[[nodiscard]] inline std::string element_path(const std::string& path,
                                              const std::size_t index) {
  std::string result = path;
  result.push_back('[');
  result.append(std::to_string(index));
  result.push_back(']');
  return result;
}

template <typename Type>
  requires SupportedValue<Type> && std::same_as<Type, std::remove_cvref_t<Type>>
[[nodiscard]] Result<Type> decode(const JsonView& view, std::string path = "$");

template <typename Integer>
  requires is_supported_integer_v<Integer>
[[nodiscard]] Result<Integer> decode_integer(const JsonView& view,
                                             const std::string& path) {
  if (view.kind() == JsonKind::signed_integer) {
    const auto value = *view.signed_integer();
    if constexpr (std::is_signed_v<Integer>) {
      if (value < static_cast<std::int64_t>(std::numeric_limits<Integer>::lowest()) ||
          value > static_cast<std::int64_t>(std::numeric_limits<Integer>::max())) {
        return std::unexpected(codec_error(path, "is outside the integer range"));
      }
    } else {
      if (value < 0 ||
          static_cast<std::uint64_t>(value) >
              static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
        return std::unexpected(codec_error(path, "is outside the integer range"));
      }
    }
    return static_cast<Integer>(value);
  }

  if (view.kind() == JsonKind::unsigned_integer) {
    const auto value = *view.unsigned_integer();
    if (value > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
      return std::unexpected(codec_error(path, "is outside the integer range"));
    }
    return static_cast<Integer>(value);
  }

  return std::unexpected(codec_error(path, "must be an integer"));
}

template <typename Float>
  requires is_supported_float_v<Float>
[[nodiscard]] Result<Float> decode_float(const JsonView& view,
                                         const std::string& path) {
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
    return std::unexpected(codec_error(path, "must be a number"));
  }

  const auto maximum = static_cast<long double>(std::numeric_limits<Float>::max());
  if (!std::isfinite(value) || value < -maximum || value > maximum) {
    return std::unexpected(codec_error(path, "must be a finite in-range number"));
  }
  const auto converted = static_cast<Float>(value);
  if (!std::isfinite(converted) || (value != 0.0L && converted == Float{0})) {
    return std::unexpected(codec_error(path, "must be a finite in-range number"));
  }
  return converted;
}

template <typename Enum>
  requires is_supported_enum_v<Enum>
// GCC 16 attributes compiler-generated template dispatch to this definition.
[[nodiscard]] Result<Enum>
decode_enum(const JsonView& view, // GCOVR_EXCL_LINE: no source decision
            const std::string& path) {
  if (view.kind() != JsonKind::string) {
    return std::unexpected(codec_error(path, "must be an enumerator name"));
  }

  const auto name = *view.string();
  std::optional<Enum> value{};
  static constexpr auto enumerators = declared_enumerators_of<Enum>();
  template for (constexpr std::meta::info enumerator : enumerators) {
    if (name == std::meta::identifier_of(enumerator)) {
      value = std::meta::extract<Enum>(std::meta::constant_of(enumerator));
    }
  }
  if (!value.has_value()) {
    return std::unexpected(codec_error(path, "is not a declared enumerator"));
  }
  return *value;
}

template <typename Type>
[[nodiscard]] bool is_reflected_member(const std::string_view name) {
  bool known = false;
  static constexpr auto members = declared_members_of<Type>();
  template for (constexpr std::meta::info member : members) {
    if (name == std::meta::identifier_of(member)) {
      known = true;
    }
  }
  return known;
}

template <typename Type>
  requires SupportedValue<Type> && std::is_aggregate_v<Type>
[[nodiscard]] Result<Type> decode_aggregate(const JsonView& view,
                                            const std::string& path) {
  if (view.kind() != JsonKind::object) {
    return std::unexpected(codec_error(path, "must be an object"));
  }

  for (std::size_t index = 0; index < view.size(); ++index) {
    const auto key = view.key_at(index);
    if (key.has_value() && !is_reflected_member<Type>(*key)) {
      std::string message{"contains unknown member "};
      append_json_string(message, *key);
      return std::unexpected(codec_error(path, message));
    }
  }

  Type object{};
  std::optional<Error> failure{};
  static constexpr auto members = declared_members_of<Type>();
  template for (constexpr std::meta::info member : members) {
    if (!failure.has_value()) {
      constexpr auto name = std::meta::identifier_of(member);
      const auto field = view.find(name);
      if (!field.has_value()) {
        if constexpr (!std::meta::has_default_member_initializer(member)) {
          failure = codec_error(member_path(path, name), "is a required member");
        }
      } else {
        using Member = [:std::meta::type_of(member):];
        auto decoded = decode<Member>(*field, member_path(path, name));
        if (!decoded) {
          failure = std::move(decoded.error());
        } else {
          object.[:member:] = std::move(*decoded);
        }
      }
    }
  }

  if (failure.has_value()) {
    return std::unexpected(std::move(*failure));
  }
  return object;
}

template <typename Optional>
[[nodiscard]] Result<Optional> decode_optional(const JsonView& view, std::string path) {
  using Element = typename optional_traits<Optional>::value_type;
  if (view.kind() == JsonKind::null) {
    return Optional{std::nullopt};
  }
  auto decoded = decode<Element>(view, std::move(path));
  if (!decoded) {
    return std::unexpected(std::move(decoded.error()));
  }
  return Optional{std::move(*decoded)};
}

template <typename Vector>
[[nodiscard]] Result<Vector> decode_vector(const JsonView& view,
                                           const std::string& path) {
  using Element = typename vector_traits<Vector>::value_type;
  if (view.kind() != JsonKind::array) {
    return std::unexpected(codec_error(path, "must be an array"));
  }
  Vector values{};
  values.reserve(view.size());
  for (std::size_t index = 0; index < view.size(); ++index) {
    auto decoded = decode<Element>(*view.at(index), element_path(path, index));
    if (!decoded) {
      return std::unexpected(std::move(decoded.error()));
    }
    values.push_back(std::move(*decoded));
  }
  return values;
}

template <typename Array>
[[nodiscard]] Result<Array> decode_array(const JsonView& view,
                                         const std::string& path) {
  using Element = typename array_traits<Array>::value_type;
  if (view.kind() != JsonKind::array || view.size() != array_traits<Array>::size) {
    return std::unexpected(
        codec_error(path, "must be an array of the declared fixed size"));
  }
  Array values{};
  for (std::size_t index = 0; index < values.size(); ++index) {
    auto decoded = decode<Element>(*view.at(index), element_path(path, index));
    if (!decoded) {
      return std::unexpected(std::move(decoded.error()));
    }
    values[index] = std::move(*decoded);
  }
  return values;
}

template <typename Type>
  requires SupportedValue<Type> && std::same_as<Type, std::remove_cvref_t<Type>>
Result<Type> decode(const JsonView& view, std::string path) {
  if constexpr (std::same_as<Type, bool>) {
    if (view.kind() != JsonKind::boolean) {
      return std::unexpected(codec_error(path, "must be a boolean"));
    }
    return *view.boolean();
  } else if constexpr (is_supported_integer_v<Type>) {
    return decode_integer<Type>(view, path);
  } else if constexpr (is_supported_float_v<Type>) {
    return decode_float<Type>(view, path);
  } else if constexpr (std::same_as<Type, std::string>) {
    if (view.kind() != JsonKind::string) {
      return std::unexpected(codec_error(path, "must be a string"));
    }
    return std::string{*view.string()};
  } else if constexpr (is_supported_enum_v<Type>) {
    return decode_enum<Type>(view, path);
  } else if constexpr (optional_traits<Type>::recognized) {
    return decode_optional<Type>(view, std::move(path));
  } else if constexpr (vector_traits<Type>::recognized) {
    return decode_vector<Type>(view, path);
  } else if constexpr (array_traits<Type>::recognized) {
    return decode_array<Type>(view, path);
  } else {
    return decode_aggregate<Type>(view, path);
  }
}

template <ToolArguments Args> [[nodiscard]] Result<Args> decode_arguments(Json input) {
  auto parsed = parse_json(std::move(input));
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  return decode<Args>(*parsed);
}

template <typename Type>
  requires SupportedValue<Type> && std::same_as<Type, std::remove_cvref_t<Type>>
[[nodiscard]] Status append_encoded(std::string& output, const Type& value,
                                    const std::string& path);

template <typename Number>
[[nodiscard]] Status append_number(std::string& output, const Number value,
                                   const std::string& path) {
  std::array<char, 128> buffer{};
  std::to_chars_result result{};
  if constexpr (std::integral<Number>) {
    result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  } else {
    if (!std::isfinite(value)) {
      return std::unexpected(codec_error(path, "must be finite"));
    }
    result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                           std::chars_format::general,
                           std::numeric_limits<Number>::max_digits10);
  }
  if (result.ec != std::errc{}) {
    return std::unexpected(codec_error(path, "could not be encoded as JSON"));
  }
  output.append(buffer.data(), result.ptr);
  return {};
}

template <typename Enum>
  requires is_supported_enum_v<Enum>
[[nodiscard]] Status append_enum(std::string& output, const Enum value,
                                 const std::string& path) {
  bool found = false;
  static constexpr auto enumerators = declared_enumerators_of<Enum>();
  template for (constexpr std::meta::info enumerator : enumerators) {
    constexpr auto candidate =
        std::meta::extract<Enum>(std::meta::constant_of(enumerator));
    if (value == candidate) {
      append_json_string(output, std::meta::identifier_of(enumerator));
      found = true;
    }
  }
  if (!found) {
    return std::unexpected(codec_error(path, "is not a declared enumerator value"));
  }
  return {};
}

template <typename Type>
  requires SupportedValue<Type> && std::is_aggregate_v<Type>
[[nodiscard]] Status append_aggregate(std::string& output, const Type& value,
                                      const std::string& path) {
  output.push_back('{');
  bool first = true;
  Status status{};
  static constexpr auto members = sorted_members_of<Type>();
  template for (constexpr std::meta::info member : members) {
    if (status) {
      if (!first) {
        output.push_back(',');
      }
      constexpr auto name = std::meta::identifier_of(member);
      append_json_string(output, name);
      output.push_back(':');
      using Member = [:std::meta::type_of(member):];
      status =
          append_encoded<Member>(output, value.[:member:], member_path(path, name));
      first = false;
    }
  }
  if (!status) {
    return status;
  }
  output.push_back('}');
  return {};
}

template <typename Type>
  requires SupportedValue<Type> && std::same_as<Type, std::remove_cvref_t<Type>>
Status append_encoded(std::string& output, const Type& value, const std::string& path) {
  if constexpr (std::same_as<Type, bool>) {
    output.append(value ? "true" : "false");
    return {};
  } else if constexpr (is_supported_integer_v<Type> || is_supported_float_v<Type>) {
    return append_number(output, value, path);
  } else if constexpr (std::same_as<Type, std::string>) {
    append_json_string(output, value);
    return {};
  } else if constexpr (is_supported_enum_v<Type>) {
    return append_enum(output, value, path);
  } else if constexpr (optional_traits<Type>::recognized) {
    if (!value.has_value()) {
      output.append("null");
      return {};
    }
    using Element = typename optional_traits<Type>::value_type;
    return append_encoded<Element>(output, *value, path);
  } else if constexpr (vector_traits<Type>::recognized ||
                       array_traits<Type>::recognized) {
    output.push_back('[');
    bool first = true;
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (!first) {
        output.push_back(',');
      }
      using Element = typename Type::value_type;
      auto status =
          append_encoded<Element>(output, value[index], element_path(path, index));
      if (!status) {
        return status;
      }
      first = false;
    }
    output.push_back(']');
    return {};
  } else {
    return append_aggregate(output, value, path);
  }
}

template <typename Type>
  requires SupportedValue<Type> && std::same_as<Type, std::remove_cvref_t<Type>>
[[nodiscard]] Result<Json> encode(const Type& value) {
  std::string output{};
  auto status = append_encoded<Type>(output, value, "$");
  if (!status) {
    return std::unexpected(std::move(status.error()));
  }
  return Json{.text = std::move(output)};
}

} // namespace scry::reflection::detail
