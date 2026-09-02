/// @file
/// @brief Implements the optional reflection component's Scry-owned JSON view bridge.
///
/// The C++26 templates operate on JsonView rather than exposing the private
/// Glaze-backed representation. Each parsed view shares document lifetime, and
/// reflected output is canonicalized through the same core codec used by explicit tools
/// and persistence.

#include "core/json_codec.hpp"

#include <iterator>
#include <memory>
#include <scry/detail/reflection_json.hpp>
#include <string>
#include <utility>

namespace scry::reflection::detail {
namespace {

/// Appends the canonical JSON `\u00xx` escape for one ASCII control byte.
///
/// @param output Destination JSON string under construction.
/// @param value Byte below 0x20 that requires escaping.
void append_control_escape(std::string& output, const unsigned char value) {
  constexpr char hexadecimal[] = "0123456789abcdef";
  output.append("\\u00");
  output.push_back(hexadecimal[value >> 4U]);
  output.push_back(hexadecimal[value & 0x0FU]);
}

} // namespace

/// Shared owner of the parsed tree referenced by a family of JsonView values.
///
/// Child views store raw pointers into @ref root for inexpensive recursive decoding
/// while retaining this document through a shared pointer. The parsed tree is never
/// mutated, so those pointers remain stable for every descendant view's lifetime.
class JsonView::Document final {
public:
  /// Takes ownership of one fully parsed canonical JSON tree.
  ///
  /// @param value Root representation transferred from the core codec.
  explicit Document(scry::detail::JsonValue value) : root(std::move(value)) {}

  /// Immutable-by-convention tree backing the root and all descendant views.
  scry::detail::JsonValue root{};
};

JsonView::JsonView(std::shared_ptr<const Document> document, const void* value) noexcept
    : document_(std::move(document)), value_(value) {}

JsonKind JsonView::kind() const noexcept {
  const auto* value = static_cast<const scry::detail::JsonValue*>(value_);
  if (value == nullptr || value->is_null()) {
    return JsonKind::null;
  }
  if (value->is_boolean()) {
    return JsonKind::boolean;
  }
  if (value->is_int64()) {
    return JsonKind::signed_integer;
  }
  if (value->is_uint64()) {
    return JsonKind::unsigned_integer;
  }
  if (value->is_number()) {
    return JsonKind::number;
  }
  if (value->is_string()) {
    return JsonKind::string;
  }
  if (value->is_array()) {
    return JsonKind::array;
  }
  return JsonKind::object;
}

std::optional<bool> JsonView::boolean() const noexcept {
  const auto* value = static_cast<const scry::detail::JsonValue*>(value_);
  if (value == nullptr || !value->is_boolean()) {
    return std::nullopt;
  }
  return value->get_boolean();
}

std::optional<std::int64_t> JsonView::signed_integer() const noexcept {
  const auto* value = static_cast<const scry::detail::JsonValue*>(value_);
  if (value == nullptr || !value->is_int64()) {
    return std::nullopt;
  }
  return value->get<std::int64_t>();
}

std::optional<std::uint64_t> JsonView::unsigned_integer() const noexcept {
  const auto* value = static_cast<const scry::detail::JsonValue*>(value_);
  if (value == nullptr || !value->is_uint64()) {
    return std::nullopt;
  }
  return value->get<std::uint64_t>();
}

std::optional<double> JsonView::number() const noexcept {
  const auto* value = static_cast<const scry::detail::JsonValue*>(value_);
  if (value == nullptr || !value->is_number()) {
    return std::nullopt;
  }
  return value->as_number();
}

std::optional<std::string_view> JsonView::string() const noexcept {
  const auto* value = static_cast<const scry::detail::JsonValue*>(value_);
  if (value == nullptr || !value->is_string()) {
    return std::nullopt;
  }
  return value->get_string();
}

std::size_t JsonView::size() const noexcept {
  const auto* value = static_cast<const scry::detail::JsonValue*>(value_);
  if (value == nullptr) {
    return 0;
  }
  if (value->is_array()) {
    return value->get_array().size();
  }
  if (value->is_object()) {
    return value->get_object().size();
  }
  return 0;
}

std::optional<JsonView> JsonView::at(const std::size_t index) const noexcept {
  const auto* value = static_cast<const scry::detail::JsonValue*>(value_);
  if (value == nullptr || !value->is_array() || index >= value->get_array().size()) {
    return std::nullopt;
  }
  return JsonView{document_, &value->get_array()[index]};
}

std::optional<std::string_view>
JsonView::key_at(const std::size_t index) const noexcept {
  const auto* value = static_cast<const scry::detail::JsonValue*>(value_);
  if (value == nullptr || !value->is_object() || index >= value->get_object().size()) {
    return std::nullopt;
  }
  auto entry = value->get_object().begin();
  std::advance(entry, static_cast<std::ptrdiff_t>(index));
  return entry->first;
}

std::optional<JsonView> JsonView::find(const std::string_view name) const noexcept {
  const auto* value = static_cast<const scry::detail::JsonValue*>(value_);
  if (value == nullptr || !value->is_object()) {
    return std::nullopt;
  }
  const auto& object = value->get_object();
  const auto found = object.find(name);
  if (found == object.end()) {
    return std::nullopt;
  }
  return JsonView{document_, &found->second};
}

/// Parses reflected tool arguments into a lifetime-owning immutable view tree.
///
/// @param json Scry-owned serialized JSON transferred to the bridge.
/// @return Root view, or ErrorCategory::tool when the input is malformed.
Result<JsonView> parse_json(Json json) {
  auto value = scry::detail::parse_json(json.text, ErrorCategory::tool,
                                        "reflected tool arguments are not valid JSON");
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  auto document = std::make_shared<JsonView::Document>(std::move(*value));
  return JsonView{document, &document->root};
}

/// Appends one string using canonical JSON quoting and control-character escapes.
///
/// The function appends rather than replacing so the compile-time reflection encoder
/// can compose arrays and objects without constructing intermediate JSON values.
///
/// @param output Destination JSON text under construction.
/// @param value Unquoted string bytes to encode.
void append_json_string(std::string& output, const std::string_view value) {
  output.push_back('"');
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    switch (character) {
    case '"':
      output.append("\\\"");
      break;
    case '\\':
      output.append("\\\\");
      break;
    case '\b':
      output.append("\\b");
      break;
    case '\f':
      output.append("\\f");
      break;
    case '\n':
      output.append("\\n");
      break;
    case '\r':
      output.append("\\r");
      break;
    case '\t':
      output.append("\\t");
      break;
    default:
      if (byte < 0x20U) {
        append_control_escape(output, byte);
      } else {
        output.push_back(character);
      }
      break;
    }
  }
  output.push_back('"');
}

/// Validates and canonicalizes JSON assembled by a reflected value encoder.
///
/// @param json Encoded reflected value transferred to the core codec.
/// @return Canonical JSON, or ErrorCategory::tool for an invalid representation.
Result<Json> canonicalize_encoded_json(Json json) {
  return scry::detail::canonicalize_json(
      json, ErrorCategory::tool, "reflected value could not be encoded as JSON");
}

} // namespace scry::reflection::detail
