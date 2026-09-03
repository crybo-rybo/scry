#include "core/json_codec.hpp"

#include <iterator>
#include <memory>
#include <scry/json.hpp>
#include <string>
#include <utility>

namespace scry {
namespace {

void append_control_escape(std::string& output, const unsigned char value) {
  constexpr char hexadecimal[] = "0123456789abcdef";
  output.append("\\u00");
  output.push_back(hexadecimal[value >> 4U]);
  output.push_back(hexadecimal[value & 0x0FU]);
}

} // namespace

class JsonView::Document final {
public:
  detail::JsonValue root{};
};

JsonView::JsonView(std::shared_ptr<const Document> document, const void* value) noexcept
    : document_(std::move(document)), value_(value) {}

// The value is parsed in place because GCC 14 reports a maybe-uninitialized false
// positive inside std::variant when a JsonValue is moved into the document.
Result<JsonView> JsonView::parse(const Json& json) {
  auto document = std::make_shared<Document>();
  if (auto status = detail::parse_json_into(document->root, json.text,
                                            ErrorCategory::invalid_argument,
                                            "JSON text is not valid");
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  return JsonView{document, &document->root};
}

JsonKind JsonView::kind() const noexcept {
  const auto* value = static_cast<const detail::JsonValue*>(value_);
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
  const auto* value = static_cast<const detail::JsonValue*>(value_);
  if (value == nullptr || !value->is_boolean()) {
    return std::nullopt;
  }
  return value->get_boolean();
}

std::optional<std::int64_t> JsonView::signed_integer() const noexcept {
  const auto* value = static_cast<const detail::JsonValue*>(value_);
  if (value == nullptr || !value->is_int64()) {
    return std::nullopt;
  }
  return value->get<std::int64_t>();
}

std::optional<std::uint64_t> JsonView::unsigned_integer() const noexcept {
  const auto* value = static_cast<const detail::JsonValue*>(value_);
  if (value == nullptr || !value->is_uint64()) {
    return std::nullopt;
  }
  return value->get<std::uint64_t>();
}

std::optional<double> JsonView::number() const noexcept {
  const auto* value = static_cast<const detail::JsonValue*>(value_);
  if (value == nullptr || !value->is_number()) {
    return std::nullopt;
  }
  return value->as_number();
}

std::optional<std::string_view> JsonView::string() const noexcept {
  const auto* value = static_cast<const detail::JsonValue*>(value_);
  if (value == nullptr || !value->is_string()) {
    return std::nullopt;
  }
  return value->get_string();
}

std::size_t JsonView::size() const noexcept {
  const auto* value = static_cast<const detail::JsonValue*>(value_);
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
  const auto* value = static_cast<const detail::JsonValue*>(value_);
  if (value == nullptr || !value->is_array() || index >= value->get_array().size()) {
    return std::nullopt;
  }
  return JsonView{document_, &value->get_array()[index]};
}

std::optional<std::string_view>
JsonView::key_at(const std::size_t index) const noexcept {
  const auto* value = static_cast<const detail::JsonValue*>(value_);
  if (value == nullptr || !value->is_object() || index >= value->get_object().size()) {
    return std::nullopt;
  }
  auto entry = value->get_object().begin();
  std::advance(entry, static_cast<std::ptrdiff_t>(index));
  return entry->first;
}

std::optional<JsonView> JsonView::find(const std::string_view name) const noexcept {
  const auto* value = static_cast<const detail::JsonValue*>(value_);
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

std::string escape_json_string(const std::string_view value) {
  std::string output{};
  output.reserve(value.size() + 2U);
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
  return output;
}

} // namespace scry
