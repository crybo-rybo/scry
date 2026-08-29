#include "core/json_codec.hpp"

#include <iterator>
#include <memory>
#include <scry/detail/reflection_json.hpp>
#include <string>
#include <utility>

namespace scry::reflection::detail {

class JsonView::Document final {
public:
  explicit Document(scry::detail::JsonValue value) : root(std::move(value)) {}

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

Result<JsonView> parse_json(Json json) {
  auto value = scry::detail::parse_json(json.text, ErrorCategory::tool,
                                        "reflected tool arguments are not valid JSON");
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  auto document = std::make_shared<JsonView::Document>(std::move(*value));
  return JsonView{document, &document->root};
}

Result<Json> canonicalize_encoded_json(Json json) {
  return scry::detail::canonicalize_json(
      json, ErrorCategory::tool, "reflected value could not be encoded as JSON");
}

} // namespace scry::reflection::detail
