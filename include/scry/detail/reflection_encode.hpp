#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <meta>
#include <scry/detail/reflection_codec.hpp>
#include <scry/detail/reflection_json.hpp>
#include <scry/detail/reflection_json_string.hpp>
#include <scry/detail/reflection_model.hpp>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

// Reflected encoding. The writer emits objects in canonical key order directly, so
// its output differs from Scry's canonical form only in number spelling and in the
// bytes of spliced scry::Json payloads. Every function here is reached only through
// an entry point that has already checked the type against a value family.
namespace scry::reflection::detail {

template <typename Type>
[[nodiscard]] codec_result<void> write_value(std::string& output, const Type& value);

template <typename Number>
[[nodiscard]] codec_result<void> write_number(std::string& output, const Number value) {
  // Wide enough for any 64-bit integer and for the shortest round-trip form of any
  // double (at most 24 characters), so to_chars cannot run out of room.
  std::array<char, 32> buffer{};
  if constexpr (std::floating_point<Number>) {
    if (!std::isfinite(value)) {
      return codec_fail("must be finite");
    }
  }
  // No precision argument: the shortest text that reads back as the same value.
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  output.append(buffer.data(), result.ptr);
  return {};
}

template <typename Enum>
[[nodiscard]] codec_result<void> write_enum(std::string& output, const Enum value) {
  bool found = false;
  template for (constexpr std::meta::info enumerator : enumerators_v<Enum>) {
    constexpr auto candidate =
        std::meta::extract<Enum>(std::meta::constant_of(enumerator));
    if (value == candidate) {
      append_json_string(output, std::meta::identifier_of(enumerator));
      found = true;
    }
  }
  if (!found) {
    return codec_fail("is not a declared enumerator value");
  }
  return {};
}

// A scry::Json value is checked and then written byte for byte; the caller owns
// its spelling.
[[nodiscard]] inline codec_result<void> write_json_value(std::string& output,
                                                         const Json& value) {
  if (!is_valid_json_text(value.text)) {
    return codec_fail("is not valid JSON");
  }
  output.append(value.text);
  return {};
}

template <typename Sequence>
[[nodiscard]] codec_result<void> write_sequence(std::string& output,
                                                const Sequence& value) {
  output.push_back('[');
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index != 0) {
      output.push_back(',');
    }
    if (auto status = write_value(output, value[index]); !status) {
      return std::unexpected(at_element(std::move(status.error()), index));
    }
  }
  output.push_back(']');
  return {};
}

// Tagged is true when the object is a variant alternative, whose tag member joins
// its members at its lexical position.
template <typename Type, bool Tagged>
[[nodiscard]] codec_result<void> write_object(std::string& output, const Type& value) {
  output.push_back('{');
  bool first = true;
  const auto separate = [&output, &first] {
    if (!first) {
      output.push_back(',');
    }
    first = false;
  };
  [[maybe_unused]] bool tag_pending = Tagged;
  const auto write_tag = [&] {
    if constexpr (Tagged) {
      if (tag_pending) {
        separate();
        output.append(tag_member(^^Type));
        tag_pending = false;
      }
    }
  };
  codec_result<void> status{};
  template for (constexpr field_layout field : fields_v<Type>) {
    constexpr std::meta::info member = field.member;
    constexpr std::string_view key = field.key_view();
    if constexpr (key > tag_key) {
      write_tag();
    }
    bool present = true;
    if constexpr (field.omit_null) {
      present = value.[:member:].has_value();
    }
    if (status && present) {
      separate();
      output.append(member_prefix(key));
      status = write_value(output, value.[:member:]);
      if (!status) {
        status = std::unexpected(at_member(std::move(status.error()), key));
      }
    }
  }
  if (!status) {
    return status;
  }
  write_tag();
  output.push_back('}');
  return {};
}

template <typename Variant>
[[nodiscard]] codec_result<void> write_variant(std::string& output,
                                               const Variant& value) {
  if (value.valueless_by_exception()) {
    return codec_fail("is a variant left valueless by an exception");
  }
  return std::visit(
      [&output]<typename Alternative>(const Alternative& alternative) {
        return write_object<Alternative, true>(output, alternative);
      },
      value);
}

template <typename Type>
codec_result<void> write_value(std::string& output, const Type& value) {
  constexpr auto kind = kind_of(^^Type);
  if constexpr (kind == value_kind::boolean) {
    output.append(value ? "true" : "false");
    return {};
  } else if constexpr (kind == value_kind::integer || kind == value_kind::floating) {
    return write_number(output, value);
  } else if constexpr (kind == value_kind::string || kind == value_kind::string_view) {
    append_json_string(output, value);
    return {};
  } else if constexpr (kind == value_kind::json) {
    return write_json_value(output, value);
  } else if constexpr (kind == value_kind::enumeration) {
    return write_enum(output, value);
  } else if constexpr (kind == value_kind::optional) {
    if (!value.has_value()) {
      output.append("null");
      return {};
    }
    return write_value(output, *value);
  } else if constexpr (kind == value_kind::vector || kind == value_kind::array) {
    return write_sequence(output, value);
  } else if constexpr (kind == value_kind::variant) {
    return write_variant(output, value);
  } else {
    return write_object<Type, false>(output, value);
  }
}

// Encodes a value checked against a family without canonicalizing the result.
// Failures are host-only ErrorCategory::tool diagnostics rooted at `$`.
template <typename Type> [[nodiscard]] Result<Json> encode_value(const Type& value) {
  std::string output{};
  if (auto status = write_value(output, value); !status) {
    return std::unexpected(encode_failure_error(status.error()));
  }
  return Json{.text = std::move(output)};
}

} // namespace scry::reflection::detail
