#pragma once

#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <meta>
#include <scry/annotations.hpp>
#include <scry/detail/reflection_model.hpp>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/tool.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace scry::reflection::detail {

// The three value families. They share one checker and differ only in the leaves
// they admit and in whether a value must be storable, since only decoding builds
// values.
enum class value_family : std::uint8_t {
  supported,
  encodable,
  decodable,
};

consteval std::string display(const std::meta::info reflection) {
  return owned_text(std::meta::display_string_of(reflection));
}

consteval std::string at_path(const std::string& path, const std::string& text) {
  return path.empty() ? text : path + ": " + text;
}

// Decoding default-constructs a value, move-assigns decoded parts into it, and
// returns it by move.
consteval bool is_storable(const std::meta::info type) {
  return std::meta::is_default_constructible_type(type) &&
         std::meta::is_move_constructible_type(type) &&
         std::meta::is_move_assignable_type(type);
}

consteval bool is_qualified(const std::meta::info type) {
  const auto dealiased = std::meta::dealias(type);
  return std::meta::is_reference_type(dealiased) ||
         std::meta::is_const_type(dealiased) || std::meta::is_volatile_type(dealiased);
}

// Explains why a type is outside a value family. The empty string means it is
// inside. The text names the offending member or element by its C++ path, and it
// is what the static_assert at every reflected entry point prints, so a rejected
// type fails with the reason rather than with an unsatisfied constraint.
class value_checker {
public:
  explicit consteval value_checker(const value_family family) : family_{family} {}

  consteval std::string check(const std::meta::info reflected,
                              const std::string& path) {
    const auto type = plain_type(reflected);
    const auto kind = kind_of(type);
    if (kind == value_kind::unsupported) {
      return unsupported(reflected, path);
    }
    if (kind <= value_kind::enumeration) {
      return check_leaf(kind, reflected, path);
    }
    return check_composite(kind, type, path);
  }

private:
  value_family family_;
  // Aggregates on the path from the root to the value being checked.
  std::vector<std::meta::info> active_{};

  [[nodiscard]] consteval bool decodes() const {
    return family_ != value_family::encodable;
  }

  static consteval std::string problem(const std::string& path,
                                       const std::meta::info type,
                                       const std::string_view reason) {
    return at_path(path, display(type) + " " + owned_text(reason));
  }

  static consteval std::string member_path(const std::string& path,
                                           const std::meta::info owner,
                                           const std::meta::info member) {
    return (path.empty() ? display(owner) + "::" : path + ".") +
           owned_text(std::meta::identifier_of(member));
  }

  static consteval std::string element_path(const std::string& path,
                                            const std::meta::info container) {
    return (path.empty() ? display(container) : path) + "[]";
  }

  consteval std::string check_leaf(const value_kind kind, const std::meta::info shown,
                                   const std::string& path) const {
    switch (kind) {
    case value_kind::string_view:
      return family_ == value_family::encodable
                 ? std::string{}
                 : problem(path, shown,
                           "is encode-only borrowed text; decoding and schemas "
                           "take std::string");
    case value_kind::json:
      return family_ == value_family::supported
                 ? problem(path, shown,
                           "is raw JSON, which has no schema; it is Encodable and "
                           "Decodable but not a SupportedValue")
                 : std::string{};
    case value_kind::enumeration:
      return check_enum(plain_type(shown), path);
    default:
      return {};
    }
  }

  // `shown` is the reflection as the checker received it, so an alias the
  // reflection still carries is printed as spelled rather than dealiased. GCC's
  // type_of already dealiases member types, so a member's std::chrono::seconds is
  // printed as the duration specialization it names.
  static consteval std::string unsupported(const std::meta::info shown,
                                           const std::string& path) {
    const auto type = plain_type(shown);
    if (std::meta::is_integral_type(type)) {
      return problem(path, shown,
                     is_character_type(type)
                         ? "is a character type; use std::string for text or a "
                           "fixed-width integer for a number"
                         : "is wider than 64 bits");
    }
    if (std::meta::is_union_type(type)) {
      return problem(path, shown,
                     "is a union; use a tagged std::variant of aggregates");
    }
    if (std::meta::is_class_type(type) && !is_complete_class(type)) {
      return problem(path, shown, "is an incomplete type");
    }
    return problem(path, shown, "is not a supported reflected value");
  }

  static consteval std::string check_enum(const std::meta::info type,
                                          const std::string& path) {
    if (!std::meta::is_scoped_enum_type(type)) {
      return problem(path, type, "is an unscoped enum; reflected enums are enum class");
    }
    const auto enumerators = std::meta::enumerators_of(type);
    if (enumerators.empty()) {
      return problem(path, type, "has no enumerators");
    }
    for (std::size_t left = 0; left < enumerators.size(); ++left) {
      for (std::size_t right = left + 1; right < enumerators.size(); ++right) {
        if (std::meta::constant_of(enumerators[left]) ==
            std::meta::constant_of(enumerators[right])) {
          return problem(path, type,
                         "has enumerators " +
                             owned_text(std::meta::identifier_of(enumerators[left])) +
                             " and " +
                             owned_text(std::meta::identifier_of(enumerators[right])) +
                             " with one value, so that value has no single name");
        }
      }
    }
    return {};
  }

  consteval std::string check_composite(const value_kind kind,
                                        const std::meta::info type,
                                        const std::string& path) {
    if (kind == value_kind::aggregate) {
      return check_aggregate(type, path);
    }
    if (decodes() && !is_storable(type)) {
      return problem(path, type,
                     "must be default-constructible, move-constructible, and "
                     "move-assignable");
    }
    if (kind == value_kind::variant) {
      return check_variant(type, path);
    }
    const auto element = element_of(type);
    if (is_qualified(element)) {
      return problem(path, type, "has a cv-qualified or reference element");
    }
    if (kind == value_kind::optional) {
      return kind_of(element) == value_kind::optional
                 ? problem(path, type,
                           "nests std::optional directly; one optional layer is the "
                           "only nullable form")
                 : check(element, path);
    }
    if (kind == value_kind::vector && plain_type(element) == (^^bool)) {
      return problem(path, type,
                     "is std::vector<bool>, whose elements are not addressable; use "
                     "std::vector<std::uint8_t>");
    }
    return check(element, element_path(path, type));
  }

  consteval std::string check_aggregate(const std::meta::info type,
                                        const std::string& path) {
    if (std::ranges::contains(active_, type)) {
      return problem(path, type, "is reachable from its own members");
    }
    if (!std::meta::bases_of(type, std::meta::access_context::unchecked()).empty()) {
      return problem(path, type, "has a base class");
    }
    if (auto text = check_class_annotations(type, path); !text.empty()) {
      return text;
    }
    active_.push_back(type);
    for (const auto member : data_members_of(type)) {
      if (auto text = check_member(type, member, path); !text.empty()) {
        return text;
      }
    }
    active_.pop_back();
    // After the members, so a const member is reported as such rather than as
    // the missing assignment it causes.
    if (decodes() && !is_storable(type)) {
      return problem(path, type,
                     "must be default-constructible, move-constructible, and "
                     "move-assignable");
    }
    return check_keys(type, path);
  }

  static consteval std::string check_class_annotations(const std::meta::info type,
                                                       const std::string& path) {
    if (annotation_count(type, ^^tag) > 1) {
      return problem(path, type, "has more than one scry::reflection::tag annotation");
    }
    if (has_annotation(type, ^^name) || has_annotation(type, ^^emit_null_t)) {
      return problem(path, type,
                     "carries scry::reflection::name or emit_null, which apply to "
                     "data members");
    }
    if (has_annotation(type, ^^tool)) {
      return problem(path, type,
                     "carries scry::reflection::tool, which applies to functions");
    }
    return {};
  }

  consteval std::string check_member(const std::meta::info owner,
                                     const std::meta::info member,
                                     const std::string& path) {
    if (!std::meta::has_identifier(member)) {
      return problem(path, owner, "has an unnamed member");
    }
    const auto here = member_path(path, owner, member);
    const auto type = std::meta::dealias(std::meta::type_of(member));
    if (!std::meta::is_public(member)) {
      return at_path(here, "is not a public member");
    }
    if (std::meta::is_bit_field(member)) {
      return at_path(here, "is a bit-field");
    }
    // Encoding only reads, so an encode-only aggregate may borrow a value through
    // a reference member, as it borrows text through std::string_view.
    if (decodes() && std::meta::is_reference_type(type)) {
      return at_path(here, "is a reference member, which decoding cannot assign");
    }
    if (std::meta::is_volatile_type(std::meta::remove_reference(type))) {
      return at_path(here, "is a volatile member");
    }
    if (decodes() && std::meta::is_const_type(type)) {
      return at_path(here, "is a const member, which decoding cannot assign");
    }
    if (auto text = check_member_annotations(member, here); !text.empty()) {
      return text;
    }
    return check(std::meta::type_of(member), here);
  }

  static consteval std::string check_member_annotations(const std::meta::info member,
                                                        const std::string& here) {
    if (annotation_count(member, ^^description) > 1) {
      return at_path(here, "a reflected member may have at most one "
                           "scry::reflection::description annotation");
    }
    if (annotation_count(member, ^^name) > 1) {
      return at_path(here, "has more than one scry::reflection::name annotation");
    }
    const bool skip = has_annotation(member, ^^skip_null_t);
    const bool emit = has_annotation(member, ^^emit_null_t);
    if ((skip || emit) && kind_of(std::meta::type_of(member)) != value_kind::optional) {
      return at_path(here, "is not a std::optional, so skip_null and emit_null do not "
                           "apply to it");
    }
    if (skip && emit) {
      return at_path(here, "carries both skip_null and emit_null");
    }
    if (has_annotation(member, ^^tag) || has_annotation(member, ^^ignore_unknown_t)) {
      return at_path(here, "carries scry::reflection::tag or ignore_unknown, which "
                           "apply to class types");
    }
    if (has_annotation(member, ^^tool)) {
      return at_path(here, "carries scry::reflection::tool, which applies to "
                           "functions");
    }
    return {};
  }

  // Runs after every member passed, so each has an identifier and a valid key.
  static consteval std::string check_keys(const std::meta::info type,
                                          const std::string& path) {
    const auto members = data_members_of(type);
    for (std::size_t left = 0; left < members.size(); ++left) {
      for (std::size_t right = left + 1; right < members.size(); ++right) {
        const auto key = json_key_of(members[left]);
        if (key == json_key_of(members[right])) {
          return problem(
              path, type,
              "has members " + owned_text(std::meta::identifier_of(members[left])) +
                  " and " + owned_text(std::meta::identifier_of(members[right])) +
                  " that both map to the JSON key \"" + owned_text(key) + "\"");
        }
      }
    }
    return {};
  }

  consteval std::string check_variant(const std::meta::info type,
                                      const std::string& path) {
    const auto alternatives = alternatives_of(type);
    for (const auto alternative : alternatives) {
      if (auto text = check_alternative(type, alternative, path); !text.empty()) {
        return text;
      }
    }
    for (std::size_t left = 0; left < alternatives.size(); ++left) {
      for (std::size_t right = left + 1; right < alternatives.size(); ++right) {
        if (*tag_of(alternatives[left]) == *tag_of(alternatives[right])) {
          return problem(path, type,
                         "has alternatives " + display(alternatives[left]) + " and " +
                             display(alternatives[right]) + " that share the tag \"" +
                             owned_text(*tag_of(alternatives[left])) + "\"");
        }
      }
    }
    return {};
  }

  consteval std::string check_alternative(const std::meta::info variant,
                                          const std::meta::info alternative,
                                          const std::string& path) {
    const auto name_of = display(alternative);
    if (is_qualified(alternative) || kind_of(alternative) != value_kind::aggregate) {
      return problem(path, variant,
                     "has the alternative " + name_of +
                         ", which is not a plain aggregate; a reflected variant is a "
                         "tagged union of aggregates");
    }
    if (!has_annotation(plain_type(alternative), ^^tag)) {
      return problem(path, variant,
                     "has the alternative " + name_of +
                         " without a scry::reflection::tag annotation; a reflected "
                         "variant is a tagged union of aggregates");
    }
    // A nested alternative is named after the variant's path ("Doc::blocks[]<Text>");
    // a root one starts its own ("Text::body").
    const auto here = path.empty() ? std::string{} : path + "<" + name_of + ">";
    if (auto text = check(alternative, here); !text.empty()) {
      return text;
    }
    for (const auto member : data_members_of(plain_type(alternative))) {
      if (json_key_of(member) == tag_key) {
        return at_path(member_path(here, plain_type(alternative), member),
                       "has the JSON key \"type\", which carries the variant's tag");
      }
    }
    return {};
  }
};

consteval std::string value_problem(const std::meta::info type,
                                    const value_family family) {
  return value_checker{family}.check(type, {});
}

consteval std::string tool_arguments_problem(const std::meta::info type) {
  if (is_qualified(type)) {
    return display(type) + " is cv-qualified or a reference; tool arguments are a "
                           "plain aggregate type";
  }
  if (kind_of(type) != value_kind::aggregate) {
    return display(type) + " is not a plain aggregate; tool arguments decode from one "
                           "JSON object";
  }
  return value_problem(type, value_family::supported);
}

// Formats a rejection for static_assert. An empty view means the type satisfies
// the concept.
consteval std::string_view unsatisfied(const std::string_view subject,
                                       const std::string_view concept_name,
                                       const std::string& reason) {
  if (reason.empty()) {
    return {};
  }
  return static_string(owned_text(subject) + " does not satisfy scry::reflection::" +
                       owned_text(concept_name) + ": " + reason);
}

template <typename Type> consteval std::string_view supported_diagnostic() {
  return unsatisfied(display(^^Type), "SupportedValue",
                     value_problem(^^Type, value_family::supported));
}

template <typename Type> consteval std::string_view encodable_diagnostic() {
  return unsatisfied(display(^^Type), "Encodable",
                     value_problem(^^Type, value_family::encodable));
}

// Decoding produces a value, so the target is a cv-unqualified object type.
template <typename Type> consteval std::string_view decodable_diagnostic() {
  return unsatisfied(display(^^Type), "Decodable",
                     is_qualified(^^Type)
                         ? display(^^Type) + " is cv-qualified or a reference; decode "
                                             "into the unqualified type"
                         : value_problem(^^Type, value_family::decodable));
}

template <typename Type> consteval std::string_view tool_arguments_diagnostic() {
  return unsatisfied(display(^^Type), "ToolArguments", tool_arguments_problem(^^Type));
}

template <typename Type> struct expected_traits {
  static constexpr bool recognized = false;
};

template <typename Value> struct expected_traits<std::expected<Value, scry::Error>> {
  static constexpr bool recognized = true;
  using value_type = Value;
};

// A result that tells the model only that the call succeeded: `void`, or a
// Status that may instead carry the handler's error. Either is sent as `{}`.
consteval bool is_acknowledgement(const std::meta::info result) {
  const auto type = plain_type(result);
  return type == (^^void) || type == plain_type(^^scry::Status);
}

// Why a tool's return type cannot be sent to the model; empty when it can. The
// text follows "returns", so it completes a sentence about the handler.
consteval std::string result_type_problem(const std::meta::info result) {
  if (std::meta::is_reference_type(result)) {
    return "the reference " + display(result) + "; a reflected tool returns a value";
  }
  if (is_acknowledgement(result)) {
    return {};
  }
  const auto type = plain_type(result);
  const bool is_result =
      is_specialization_of(type, ^^std::expected) &&
      plain_type(std::meta::template_arguments_of(type)[1]) == (^^scry::Error);
  const auto reason =
      value_problem(is_result ? std::meta::template_arguments_of(type)[0] : result,
                    value_family::supported);
  if (reason.empty()) {
    return {};
  }
  return display(result) +
         ", which is not void, scry::Status, a SupportedValue, or a Result of one: " +
         reason;
}

template <typename Result>
consteval std::string handler_result_problem(const std::string& handler) {
  const auto reason = result_type_problem(^^Result);
  return reason.empty() ? std::string{} : handler + " returns " + reason;
}

template <typename Handler, typename Args>
consteval std::string tool_handler_problem() {
  using Callable = std::decay_t<Handler>;
  using Context = const scry::ToolCallContext&;
  const auto handler = display(std::meta::dealias(^^Callable));
  if constexpr (!std::constructible_from<Callable, Handler> ||
                !std::move_constructible<Callable>) {
    return handler + " cannot be stored: a handler must be move-constructible";
  } else if constexpr (std::invocable<Callable&, Context, Args>) {
    // A handler that accepts the context is invoked that way, so its result type
    // is the one that has to be encodable. Checking the plain form as well would
    // accept a callable whose two arities disagree about their return.
    return handler_result_problem<std::invoke_result_t<Callable&, Context, Args>>(
        handler);
  } else if constexpr (std::invocable<Callable&, Args>) {
    return handler_result_problem<std::invoke_result_t<Callable&, Args>>(handler);
  } else {
    return handler + " is not invocable as (" + display(^^Args) +
           ") or as (const scry::ToolCallContext&, " + display(^^Args) +
           "); a context parameter must come first";
  }
}

template <typename Handler, typename Args>
consteval std::string_view tool_handler_diagnostic() {
  return unsatisfied(display(std::meta::dealias(^^std::decay_t<Handler>)),
                     "ToolHandlerFor<" + display(^^Args) + ">",
                     tool_handler_problem<Handler, Args>());
}

} // namespace scry::reflection::detail

namespace scry::reflection {

/// Values supported by reflected schema generation and strict marshalling.
///
/// Supported shapes are a deliberately closed matrix: booleans, bounded
/// non-character integers, finite floats, strings, scoped enums, one optional layer,
/// vectors except `vector<bool>`, fixed arrays, recursively supported plain
/// aggregates, and tagged variants of such aggregates, each alternative carrying a
/// distinct scry::reflection::tag. Every supported value has a generated schema,
/// encodes, and decodes. Reflected entry points report the member path and the
/// reason a type falls outside this family.
template <typename Type>
concept SupportedValue =
    detail::value_problem(^^std::remove_cvref_t<Type>, detail::value_family::supported)
        .empty();

/// Values reflection::encode() accepts: SupportedValue plus the encode-only leaves.
///
/// `std::string_view` is written as a JSON string, for wire text borrowed from
/// elsewhere. `scry::Json` is checked for validity and spliced verbatim. Aggregates
/// here need not be default-constructible, movable, or free of `const` members, and
/// may borrow a member through a reference, since encoding only reads them. Neither
/// leaf has a schema.
template <typename Type>
concept Encodable =
    detail::value_problem(^^std::remove_cvref_t<Type>, detail::value_family::encodable)
        .empty();

/// Values reflection::decode() produces: SupportedValue plus `scry::Json`.
///
/// A `scry::Json` member captures the canonical text of whatever JSON value sits at
/// its position, `null` included, so a document can carry an opaque payload through
/// a typed decode. `scry::Json` has no schema. Decoding produces a value, so the
/// target itself must be a cv-unqualified object type, as reflection::decode()
/// requires.
template <typename Type>
concept Decodable =
    !detail::is_qualified(^^Type) &&
    detail::value_problem(^^Type, detail::value_family::decodable).empty();

/// Complete plain aggregates accepted as reflected tool arguments.
template <typename Type>
concept ToolArguments = detail::tool_arguments_problem(^^Type).empty();

/// Callables accepted for a particular reflected argument aggregate.
///
/// Handlers receive Args by value and may return a supported value directly or a Result
/// of one.
template <typename Handler, typename Args>
concept ToolHandlerFor =
    ToolArguments<Args> && detail::tool_handler_problem<Handler, Args>().empty();

} // namespace scry::reflection
