#pragma once

#include <cstddef>
#include <string_view>

/// Annotation vocabulary for Scry's reflected codec.
///
/// Every type here is a plain C++23 structural type, so this header needs neither
/// `<meta>` nor a reflection-enabled compiler; only writing a P3394 annotation,
/// `[[= value]]`, requires C++26. Member annotations are written before the member
/// declaration. Class annotations are written between the class-key and the class
/// name, `struct [[= scry::reflection::tag{"text"}]] TextBlock { ... };`, which is
/// the placement that appertains to the class itself.
namespace scry::reflection {

template <std::size_t Size> struct description;

/// Builds a description annotation from a string_view naming a text catalog entry.
///
/// The view must have static storage duration, because it is bound as a reference
/// template parameter. Usage:
/// `[[= scry::reflection::description_of<catalog::direction>()]]`.
/// @tparam View Annotation text to copy into the annotation payload.
/// @return Annotation equal to the one the literal form would build.
template <const std::string_view& View> [[nodiscard]] consteval auto description_of();

/// Fixed-string payload for Scry's P3394 parameter-description annotation.
///
/// Applies to data members; the text becomes the member schema's `description`.
/// @tparam Size Character-array extent including the null terminator.
template <std::size_t Size> struct description {
  /// Owned null-terminated annotation text.
  char text[Size]{};

  /// Captures a string literal at compile time.
  /// @param value Null-terminated annotation text.
  consteval description(const char (&value)[Size]) {
    for (std::size_t index = 0; index < Size; ++index) {
      text[index] = value[index];
    }
  }

  /// Returns the annotation without its null terminator.
  /// @return Non-owning view into text.
  [[nodiscard]] constexpr std::string_view view() const noexcept {
    static_assert(Size > 0);
    return {text, Size - 1};
  }

private:
  struct view_tag {};

  // The copy happens inside the type so a catalog-built annotation is still a
  // description<Size>, which is what the schema generator detects and what the
  // annotation NTTP rules require.
  consteval description(view_tag, const std::string_view value) {
    for (std::size_t index = 0; index < value.size(); ++index) {
      text[index] = value[index];
    }
  }

  template <const std::string_view& View> friend consteval auto description_of();
};

/// Deduces a description extent from a string literal.
/// @param value Annotation text whose extent is deduced.
template <std::size_t Size> description(const char (&value)[Size]) -> description<Size>;

template <const std::string_view& View> [[nodiscard]] consteval auto description_of() {
  using annotation = description<View.size() + 1>;
  return annotation{typename annotation::view_tag{}, View};
}

/// Overrides the JSON key of one data member.
///
/// The key replaces the member's identifier in encoding, decoding, generated
/// schemas, and decode-failure paths. Objects are written in lexical order of these
/// final keys, and two members of one class that end with the same key are a
/// compile-time error. Usage: `[[= scry::reflection::name{"max_tokens"}]]`.
/// @tparam Size Character-array extent including the null terminator.
template <std::size_t Size> struct name {
  static_assert(Size > 1, "scry::reflection::name must not be empty");

  /// Owned null-terminated key text.
  char text[Size]{};

  /// Captures a string literal at compile time.
  /// @param value Null-terminated key text.
  consteval name(const char (&value)[Size]) {
    for (std::size_t index = 0; index < Size; ++index) {
      text[index] = value[index];
    }
  }

  /// Returns the key without its null terminator.
  /// @return Non-owning view into text.
  [[nodiscard]] constexpr std::string_view view() const noexcept {
    return {text, Size - 1};
  }
};

/// Deduces a name extent from a string literal.
/// @param value Key text whose extent is deduced.
template <std::size_t Size> name(const char (&value)[Size]) -> name<Size>;

/// Names a class's discriminator value when it is an alternative of a `std::variant`.
///
/// A reflected `std::variant<A, B, ...>` is a tagged union: each alternative is a
/// plain aggregate with exactly one tag, the tags are distinct, and the encoded
/// object carries its alternative's tag under the key `"type"`. An alternative
/// may therefore not declare a member whose final key is `"type"`. Outside a
/// variant the tag has no effect. Usage:
/// `struct [[= scry::reflection::tag{"text"}]] TextBlock { std::string text; };`.
/// @tparam Size Character-array extent including the null terminator.
template <std::size_t Size> struct tag {
  static_assert(Size > 1, "scry::reflection::tag must not be empty");

  /// Owned null-terminated discriminator text.
  char text[Size]{};

  /// Captures a string literal at compile time.
  /// @param value Null-terminated discriminator text.
  consteval tag(const char (&value)[Size]) {
    for (std::size_t index = 0; index < Size; ++index) {
      text[index] = value[index];
    }
  }

  /// Returns the discriminator without its null terminator.
  /// @return Non-owning view into text.
  [[nodiscard]] constexpr std::string_view view() const noexcept {
    return {text, Size - 1};
  }
};

/// Deduces a tag extent from a string literal.
/// @param value Discriminator text whose extent is deduced.
template <std::size_t Size> tag(const char (&value)[Size]) -> tag<Size>;

/// Type of the skip_null annotation.
struct skip_null_t {};

/// Omits a disengaged `std::optional` member instead of writing `null`.
///
/// On a class it applies to every `std::optional` data member; on a data member it
/// applies to that member, which must be a `std::optional`. A member it applies to
/// is also optional on input: its absence decodes as a disengaged value, whatever
/// its initializer, so every encoded value decodes to itself, and generated
/// schemas do not list it as required. Without it a disengaged member is written
/// as `null`.
inline constexpr skip_null_t skip_null{};

/// Type of the emit_null annotation.
struct emit_null_t {};

/// Restores `null` output for one `std::optional` data member of a skip_null class.
inline constexpr emit_null_t emit_null{};

/// Type of the ignore_unknown annotation.
struct ignore_unknown_t {};

/// Makes decoding of a class ignore JSON members the class does not declare.
///
/// Applies to class types. The default rejects unknown members. Generated schemas
/// stay closed (`"additionalProperties":false`), because a schema describes what a
/// producer should send, not everything the decoder tolerates.
inline constexpr ignore_unknown_t ignore_unknown{};

} // namespace scry::reflection
