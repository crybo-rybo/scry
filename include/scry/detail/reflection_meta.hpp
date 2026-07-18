#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <meta>
#include <optional>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace scry::reflection {

struct ToolMetadata {
  std::string name{};
  std::string description{};
};

struct parameter_description {
  std::string_view member{};
  std::string_view text{};
};

template <typename Args> struct tool_traits {
  static constexpr std::array<parameter_description, 0> descriptions{};
};

template <std::size_t Size> struct description {
  char text[Size]{};

  consteval description(const char (&value)[Size]) { std::ranges::copy(value, text); }

  [[nodiscard]] constexpr std::string_view view() const noexcept {
    static_assert(Size > 0);
    return {text, Size - 1};
  }
};

template <std::size_t Size> description(const char (&)[Size]) -> description<Size>;

namespace detail {

template <typename Type> struct optional_traits {
  static constexpr bool recognized = false;
};

template <typename Value> struct optional_traits<std::optional<Value>> {
  static constexpr bool recognized = true;
  using value_type = Value;
};

template <typename Type> struct vector_traits {
  static constexpr bool recognized = false;
};

template <typename Value, typename Allocator>
struct vector_traits<std::vector<Value, Allocator>> {
  static constexpr bool recognized = true;
  using value_type = Value;
};

template <typename Type> struct array_traits {
  static constexpr bool recognized = false;
};

template <typename Value, std::size_t Size>
struct array_traits<std::array<Value, Size>> {
  static constexpr bool recognized = true;
  using value_type = Value;
  static constexpr std::size_t size = Size;
};

template <typename Type>
inline constexpr bool is_character_integer_v =
    std::same_as<Type, char> || std::same_as<Type, signed char> ||
    std::same_as<Type, unsigned char> || std::same_as<Type, wchar_t> ||
    std::same_as<Type, char8_t> || std::same_as<Type, char16_t> ||
    std::same_as<Type, char32_t>;

template <typename Type>
inline constexpr bool is_supported_integer_v = [] {
  if constexpr (!std::integral<Type>) {
    return false;
  } else {
    return !std::same_as<Type, bool> && !is_character_integer_v<Type> &&
           sizeof(Type) <= sizeof(std::uint64_t);
  }
}();

template <typename Type>
inline constexpr bool is_supported_float_v =
    std::same_as<Type, float> || std::same_as<Type, double>;

template <typename Type>
inline constexpr bool is_supported_enum_v =
    std::is_enum_v<Type> && std::is_scoped_enum_v<Type>;

template <typename Type> consteval auto sorted_members_of() {
  auto members = std::meta::nonstatic_data_members_of(
      ^^Type, std::meta::access_context::unchecked());
  std::ranges::sort(members, {}, [](const std::meta::info member) {
    return std::meta::identifier_of(member);
  });
  return std::define_static_array(members);
}

template <typename Type> consteval auto declared_members_of() {
  return std::define_static_array(std::meta::nonstatic_data_members_of(
      ^^Type, std::meta::access_context::unchecked()));
}

template <typename Type> consteval auto declared_bases_of() {
  return std::define_static_array(
      std::meta::bases_of(^^Type, std::meta::access_context::unchecked()));
}

template <typename Type> consteval auto declared_enumerators_of() {
  return std::define_static_array(std::meta::enumerators_of(^^Type));
}

template <typename Type> consteval bool enum_values_are_unique() {
  bool unique = true;
  static constexpr auto enumerators = declared_enumerators_of<Type>();
  template for (constexpr std::meta::info left : enumerators) {
    constexpr auto left_value = std::meta::extract<Type>(std::meta::constant_of(left));
    template for (constexpr std::meta::info right : enumerators) {
      constexpr auto right_value =
          std::meta::extract<Type>(std::meta::constant_of(right));
      if constexpr (left != right && left_value == right_value) {
        unique = false;
      }
    }
  }
  return unique;
}

template <typename Type, typename... Seen> consteval bool supported_value_impl();

template <typename Optional, typename... Seen>
consteval bool supported_optional_impl() {
  using Element = typename optional_traits<Optional>::value_type;
  if constexpr (!std::same_as<Element, std::remove_cvref_t<Element>> ||
                optional_traits<Element>::recognized ||
                !std::is_default_constructible_v<Optional> ||
                !std::is_move_constructible_v<Optional> ||
                !std::is_move_assignable_v<Optional>) {
    return false;
  } else {
    return supported_value_impl<Element, Seen...>();
  }
}

template <typename Vector, typename... Seen> consteval bool supported_vector_impl() {
  using Element = typename vector_traits<Vector>::value_type;
  if constexpr (!std::same_as<Element, std::remove_cvref_t<Element>> ||
                std::same_as<Element, bool> ||
                !std::is_default_constructible_v<Vector> ||
                !std::is_move_constructible_v<Vector> ||
                !std::is_move_assignable_v<Vector>) {
    return false;
  } else {
    return supported_value_impl<Element, Seen...>();
  }
}

template <typename Array, typename... Seen> consteval bool supported_array_impl() {
  using Element = typename array_traits<Array>::value_type;
  if constexpr (!std::same_as<Element, std::remove_cvref_t<Element>> ||
                !std::is_default_constructible_v<Array> ||
                !std::is_move_constructible_v<Array> ||
                !std::is_move_assignable_v<Array>) {
    return false;
  } else {
    return supported_value_impl<Element, Seen...>();
  }
}

template <typename Type, typename... Seen> consteval bool supported_aggregate_impl() {
  if constexpr (!std::is_aggregate_v<Type> || std::is_union_v<Type> ||
                !std::is_default_constructible_v<Type> ||
                !std::is_move_constructible_v<Type> ||
                !std::is_move_assignable_v<Type>) {
    return false;
  } else {
    static constexpr auto bases = declared_bases_of<Type>();
    if constexpr (!bases.empty()) {
      return false;
    }

    bool supported = true;
    static constexpr auto members = declared_members_of<Type>();
    template for (constexpr std::meta::info member : members) {
      using Member = [:std::meta::type_of(member):];
      if constexpr (!std::meta::has_identifier(member) ||
                    !std::meta::is_public(member) || std::meta::is_bit_field(member) ||
                    std::is_reference_v<Member> || std::is_const_v<Member> ||
                    std::is_volatile_v<Member>) {
        supported = false;
      } else if constexpr (!supported_value_impl<Member, Seen..., Type>()) {
        supported = false;
      }
    }
    return supported;
  }
}

template <typename Type, typename... Seen> consteval bool supported_value_impl() {
  using Value = std::remove_cvref_t<Type>;
  if constexpr (std::same_as<Value, scry::Json>) {
    return false;
  } else if constexpr (std::same_as<Value, bool> || is_supported_integer_v<Value> ||
                       is_supported_float_v<Value> ||
                       std::same_as<Value, std::string>) {
    return true;
  } else if constexpr (is_supported_enum_v<Value>) {
    static constexpr auto enumerators = declared_enumerators_of<Value>();
    return !enumerators.empty() && enum_values_are_unique<Value>();
  } else if constexpr (optional_traits<Value>::recognized) {
    return supported_optional_impl<Value, Seen...>();
  } else if constexpr (vector_traits<Value>::recognized) {
    return supported_vector_impl<Value, Seen...>();
  } else if constexpr (array_traits<Value>::recognized) {
    return supported_array_impl<Value, Seen...>();
  } else if constexpr (std::is_aggregate_v<Value>) {
    if constexpr ((std::same_as<Value, Seen> || ...)) {
      return false;
    } else {
      return supported_aggregate_impl<Value, Seen...>();
    }
  } else {
    return false;
  }
}

template <typename Type> consteval bool tool_arguments_impl() {
  using Args = std::remove_cvref_t<Type>;
  if constexpr (!std::same_as<Type, Args> || std::same_as<Args, scry::Json> ||
                !std::is_aggregate_v<Args>) {
    return false;
  } else {
    return supported_aggregate_impl<Args>();
  }
}

template <typename Type> struct expected_traits {
  static constexpr bool recognized = false;
};

template <typename Value> struct expected_traits<std::expected<Value, scry::Error>> {
  static constexpr bool recognized = true;
  using value_type = Value;
};

template <typename Result> consteval bool supported_handler_result_impl() {
  using Return = std::remove_cvref_t<Result>;
  if constexpr (std::is_reference_v<Result>) {
    return false;
  } else if constexpr (expected_traits<Return>::recognized) {
    using Value = typename expected_traits<Return>::value_type;
    return supported_value_impl<Value>();
  } else {
    return supported_value_impl<Return>();
  }
}

template <typename Handler, typename Args> consteval bool tool_handler_impl() {
  using Callable = std::decay_t<Handler>;
  if constexpr (!std::constructible_from<Callable, Handler> ||
                !std::move_constructible<Callable> ||
                !std::invocable<Callable&, Args>) {
    return false;
  } else {
    return supported_handler_result_impl<std::invoke_result_t<Callable&, Args>>();
  }
}

} // namespace detail

template <typename Type>
concept SupportedValue = detail::supported_value_impl<std::remove_cvref_t<Type>>();

template <typename Type>
concept ToolArguments = detail::tool_arguments_impl<Type>();

template <typename Handler, typename Args>
concept ToolHandlerFor =
    ToolArguments<Args> && detail::tool_handler_impl<Handler, Args>();

} // namespace scry::reflection
