#pragma once

/**
 * @file reflection_meta.hpp
 * @brief Compile-time type classification and concepts for reflected values and tools.
 *
 * This header is the policy center for Scry's optional reflection component. It uses
 * P2996 metadata to recognize the deliberately closed SCRY-TOOL-010 value family and
 * rejects shapes whose JSON mapping would be ambiguous or whose objects could not be
 * decoded safely. Schema generation, decoding, encoding, and registration all consume
 * these same predicates so they cannot drift into different type contracts.
 */

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

/// Provider-visible metadata for a reflected tool registration.
///
/// The argument schema is supplied separately by input_schema_v, so this aggregate
/// contains only values common to explicit ToolDefinition registration.
struct ToolMetadata {
  /// Nonempty tool name, unique within the receiving ToolRegistry.
  std::string name{};
  /// Optional human-readable tool description exposed to the model.
  std::string description{};
};

/// Fixed-string payload for Scry's P3394 member-description annotation.
///
/// Use as `[[=scry::reflection::description{"..."}]]` on a reflected aggregate
/// member. Schema generation accepts at most one such annotation per member and emits
/// its text as the JSON Schema `description`. This annotation is the sole description
/// source for reflected parameters.
/// @tparam Size Character-array extent including the null terminator.
template <std::size_t Size> struct description {
  /// Owned null-terminated annotation text.
  char text[Size]{};

  /// Captures a string literal at compile time, including its terminator.
  /// @param value Null-terminated annotation text.
  consteval description(const char (&value)[Size]) { std::ranges::copy(value, text); }

  /// Returns the annotation without its null terminator.
  /// @return Non-owning view into text.
  [[nodiscard]] constexpr std::string_view view() const noexcept {
    static_assert(Size > 0);
    return {text, Size - 1};
  }
};

/// Deduces a description payload extent from a string literal.
/// @param value Annotation text whose extent is deduced.
/// @return A description specialization whose storage includes the terminator.
template <std::size_t Size> description(const char (&value)[Size]) -> description<Size>;

/// Internal compile-time classification shared by schema and codec generation.
namespace detail {

/// Detects the primary non-optional case and exposes no element type.
/// @tparam Type Candidate type after cv/ref removal.
template <typename Type> struct optional_traits {
  /// Whether Type is a recognized `std::optional` specialization.
  static constexpr bool recognized = false;
};

/// Extracts the value type from a recognized `std::optional`.
/// @tparam Value Optional element type.
template <typename Value> struct optional_traits<std::optional<Value>> {
  /// Marks this specialization as recognized.
  static constexpr bool recognized = true;
  /// Element stored by the optional.
  using value_type = Value;
};

/// Detects the primary non-vector case and exposes no element type.
/// @tparam Type Candidate type after cv/ref removal.
template <typename Type> struct vector_traits {
  /// Whether Type is a recognized `std::vector` specialization.
  static constexpr bool recognized = false;
};

/// Extracts the element type from a recognized `std::vector` specialization.
/// @tparam Value Vector element type.
/// @tparam Allocator Vector allocator type; it does not affect JSON representation.
template <typename Value, typename Allocator>
struct vector_traits<std::vector<Value, Allocator>> {
  /// Marks this specialization as recognized.
  static constexpr bool recognized = true;
  /// Element stored by the vector.
  using value_type = Value;
};

/// Detects the primary non-array case and exposes no element metadata.
/// @tparam Type Candidate type after cv/ref removal.
template <typename Type> struct array_traits {
  /// Whether Type is a recognized `std::array` specialization.
  static constexpr bool recognized = false;
};

/// Extracts element type and fixed extent from a recognized `std::array`.
/// @tparam Value Array element type.
/// @tparam Size Compile-time array extent.
template <typename Value, std::size_t Size>
struct array_traits<std::array<Value, Size>> {
  /// Marks this specialization as recognized.
  static constexpr bool recognized = true;
  /// Element stored by the array.
  using value_type = Value;
  /// Fixed number of JSON array elements required by the mapping.
  static constexpr std::size_t size = Size;
};

/// Whether an integral type is one of C++'s character code-unit types.
///
/// Character integers are deliberately excluded because Scry maps text only through
/// `std::string`; silently treating a code unit as a JSON number would be surprising.
/// @tparam Type Candidate integral type.
template <typename Type>
inline constexpr bool is_character_integer_v =
    std::same_as<Type, char> || std::same_as<Type, signed char> ||
    std::same_as<Type, unsigned char> || std::same_as<Type, wchar_t> ||
    std::same_as<Type, char8_t> || std::same_as<Type, char16_t> ||
    std::same_as<Type, char32_t>;

/// Whether Type is a supported non-character integer no wider than `std::uint64_t`.
/// @tparam Type Candidate type.
template <typename Type>
inline constexpr bool is_supported_integer_v = [] {
  if constexpr (!std::integral<Type>) {
    return false;
  } else {
    return !std::same_as<Type, bool> && !is_character_integer_v<Type> &&
           sizeof(Type) <= sizeof(std::uint64_t);
  }
}();

/// Whether Type is one of the finite-at-runtime floating types in the public matrix.
/// @tparam Type Candidate type.
template <typename Type>
inline constexpr bool is_supported_float_v =
    std::same_as<Type, float> || std::same_as<Type, double>;

/// Whether Type is a scoped enum eligible for reflected name mapping.
///
/// Enumerator presence and unique underlying values are checked separately by
/// supported_value_impl().
/// @tparam Type Candidate type.
template <typename Type>
inline constexpr bool is_supported_enum_v =
    std::is_enum_v<Type> && std::is_scoped_enum_v<Type>;

/// Returns a static compile-time array of Type's data members in lexical name order.
///
/// Canonical schema and JSON-object encoding use this ordering; decoding instead uses
/// declaration order because assignment order does not affect canonical input lookup.
/// @tparam Type Reflected aggregate type.
/// @return Static array of member reflection values sorted by identifier.
template <typename Type> consteval auto sorted_members_of() {
  auto members = std::meta::nonstatic_data_members_of(
      ^^Type, std::meta::access_context::unchecked());
  std::ranges::sort(members, {}, [](const std::meta::info member) {
    return std::meta::identifier_of(member);
  });
  return std::define_static_array(members);
}

/// Returns Type's non-static data members in declaration order.
/// @tparam Type Reflected aggregate type.
/// @return Static array of member reflection values.
template <typename Type> consteval auto declared_members_of() {
  return std::define_static_array(std::meta::nonstatic_data_members_of(
      ^^Type, std::meta::access_context::unchecked()));
}

/// Returns Type's direct bases under an unchecked reflection access context.
/// @tparam Type Candidate aggregate type.
/// @return Static array of base-specifier reflection values.
template <typename Type> consteval auto declared_bases_of() {
  return std::define_static_array(
      std::meta::bases_of(^^Type, std::meta::access_context::unchecked()));
}

/// Returns Type's enumerators in declaration order.
/// @tparam Type Reflected enum type.
/// @return Static array of enumerator reflection values.
template <typename Type> consteval auto declared_enumerators_of() {
  return std::define_static_array(std::meta::enumerators_of(^^Type));
}

/// Checks that no two enumerators of Type share an underlying value.
///
/// Aliases are rejected because encoding an underlying enum value back to one exact
/// JSON name would otherwise be ambiguous.
/// @tparam Type Scoped enum candidate.
/// @return true only when every declared enumerator has a distinct value.
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

/// Recursively implements the SupportedValue concept while tracking aggregate cycles.
/// @tparam Type Candidate value type.
/// @tparam Seen Aggregate types already visited on the current recursion path.
/// @return true when Type has one unambiguous schema/decode/encode mapping.
template <typename Type, typename... Seen> consteval bool supported_value_impl();

/// Validates one optional layer and recursively validates its element type.
/// @tparam Optional Recognized `std::optional` specialization.
/// @tparam Seen Aggregate recursion path forwarded to the element check.
/// @return true when Optional has the construction/assignment operations needed by the
/// decoder and its non-optional element is supported.
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

/// Validates a vector specialization and recursively validates its element type.
///
/// Every `vector<bool, Allocator>` specialization is rejected because proxy references
/// do not preserve the ordinary element assignment model used by the codec.
/// @tparam Vector Recognized `std::vector` specialization.
/// @tparam Seen Aggregate recursion path forwarded to the element check.
/// @return true when Vector can be constructed/moved/assigned and its element is
/// supported.
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

/// Validates a fixed array and recursively validates its element type.
/// @tparam Array Recognized `std::array` specialization.
/// @tparam Seen Aggregate recursion path forwarded to the element check.
/// @return true when Array can be constructed/moved/assigned and its element is
/// supported.
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

/// Validates the structural contract for a reflected aggregate.
///
/// Supported aggregates are complete default-initializable, movable, non-union plain
/// aggregates with no bases. Every member must be public, named, writable,
/// non-bit-field, and recursively supported. Seen prevents infinitely recursive
/// by-value shapes from satisfying the concept.
/// @tparam Type Candidate aggregate type.
/// @tparam Seen Aggregate recursion path preceding Type.
/// @return true when Type is safe for strict memberwise decoding and encoding.
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

/// Classifies a type against Scry's closed reflected-value family.
///
/// Json itself is rejected to keep the reflected API typed; dynamic JSON belongs at the
/// explicit ToolRegistry boundary. Containers and aggregates recurse through the same
/// classifier, and enum aliases/nested optionals are rejected explicitly.
/// @tparam Type Candidate value type; cv/ref qualifiers are ignored for classification.
/// @tparam Seen Aggregate types already visited on this recursion path.
/// @return true exactly for the values covered by SCRY-TOOL-010.
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

/// Implements the stricter root constraint for reflected tool arguments.
///
/// Unlike a general SupportedValue, a tool-argument root must be an unqualified plain
/// aggregate so provider arguments always have a closed JSON-object schema.
/// @tparam Type Candidate argument type as written at the call site.
/// @return true when Type is an unqualified supported aggregate root.
template <typename Type> consteval bool tool_arguments_impl() {
  using Args = std::remove_cvref_t<Type>;
  if constexpr (!std::same_as<Type, Args> || std::same_as<Args, scry::Json> ||
                !std::is_aggregate_v<Args>) {
    return false;
  } else {
    return supported_aggregate_impl<Args>();
  }
}

/// Detects the primary non-Result case for reflected handler returns.
/// @tparam Type Candidate decayed return type.
template <typename Type> struct expected_traits {
  /// Whether Type is exactly Scry's supported expected error shape.
  static constexpr bool recognized = false;
};

/// Extracts a success value from `std::expected<Value, scry::Error>`.
/// @tparam Value Expected success value.
template <typename Value> struct expected_traits<std::expected<Value, scry::Error>> {
  /// Marks this expected specialization as a recognized Scry Result shape.
  static constexpr bool recognized = true;
  /// Success value encoded when the result contains a value.
  using value_type = Value;
};

/// Validates a reflected handler's direct or expected-wrapped result type.
///
/// References are rejected because encoding must not depend on borrowed lifetime.
/// Status, void, raw Json, futures, and awaitables fail through the SupportedValue
/// classification rather than creating parallel execution semantics.
/// @tparam Result Exact invocation result type, including reference qualifiers.
/// @return true when the result can be synchronously encoded by the reflection codec.
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

/// Validates construction, invocation, and return encoding for a typed handler.
/// @tparam Handler Handler type and value category presented to add().
/// @tparam Args Valid reflected tool-argument aggregate.
/// @return true when a decayed owned callable can be constructed from Handler, invoked
/// with Args by value, and synchronously encoded.
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

/// Values supported by reflected schema generation and strict marshalling.
///
/// Supported shapes are the closed matrix in SCRY-TOOL-010: booleans, bounded
/// non-character integers, finite floats, strings, scoped enums, one optional layer,
/// vectors except `vector<bool>`, fixed arrays, and recursively supported plain
/// aggregates. Runtime values remain subject to codec checks such as finite floating
/// values and declared enum membership.
///
/// cv/ref qualifiers at a direct encode() call are normalized for classification; the
/// schema and codec still operate on the underlying value type.
/// @tparam Type Candidate value type.
template <typename Type>
concept SupportedValue = detail::supported_value_impl<std::remove_cvref_t<Type>>();

/// Complete plain aggregates accepted as reflected tool arguments.
///
/// The root type must be unqualified, default-initializable, moveable, non-union, and
/// inheritance-free. Every reflected member must be public, named, writable,
/// non-bit-field, and a SupportedValue. A default member initializer controls omission;
/// `std::optional` independently controls JSON nullability.
/// @tparam Type Candidate root argument type.
template <typename Type>
concept ToolArguments = detail::tool_arguments_impl<Type>();

/// Callables accepted for a particular reflected argument aggregate.
///
/// Handlers are owned by value, invoked as mutable lvalues, receive Args by value, and
/// may return a supported value directly or a Result of one. Move-only captures are
/// supported. Borrowed/reference, asynchronous, void, Status, and raw-Json returns are
/// intentionally rejected; use explicit ToolRegistry registration for dynamic shapes.
/// @tparam Handler Callable type and value category presented for registration.
/// @tparam Args Reflected argument aggregate passed by value.
template <typename Handler, typename Args>
concept ToolHandlerFor =
    ToolArguments<Args> && detail::tool_handler_impl<Handler, Args>();

} // namespace scry::reflection
