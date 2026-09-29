#include "kernel/kernel.hpp"

#include "kernel/error.hpp"
#include "kernel/json/document.hpp"

#include <memory>
#include <scry/json.hpp>
#include <string>
#include <utility>

namespace scry {
namespace {

// JsonView stores its node type-erased, as a shared_ptr<const void> aliasing the
// parsed document, so the public header stays free of the kernel's types; every
// accessor recovers the real node through here.
[[nodiscard]] const detail::json::Value* node(const void* value) noexcept {
  return static_cast<const detail::json::Value*>(value);
}

} // namespace

JsonView::JsonView(std::shared_ptr<const void> value) noexcept
    : value_(std::move(value)) {}

Result<JsonView> JsonView::parse(const Json& json) {
  auto document = detail::json::parse(json.text);
  if (!document) {
    return std::unexpected(
        detail::make_error(ErrorCategory::invalid_argument, "JSON text is not valid"));
  }
  return JsonView{std::shared_ptr<const void>{
      std::make_shared<const detail::json::Value>(std::move(*document))}};
}

JsonKind JsonView::kind() const noexcept {
  const auto* value = node(value_.get());
  return value == nullptr ? JsonKind::null : value->kind();
}

std::optional<bool> JsonView::boolean() const noexcept {
  const auto* value = node(value_.get());
  return value == nullptr ? std::nullopt : value->boolean();
}

std::optional<std::int64_t> JsonView::signed_integer() const noexcept {
  const auto* value = node(value_.get());
  return value == nullptr ? std::nullopt : value->signed_integer();
}

std::optional<std::uint64_t> JsonView::unsigned_integer() const noexcept {
  const auto* value = node(value_.get());
  return value == nullptr ? std::nullopt : value->unsigned_integer();
}

std::optional<double> JsonView::number() const noexcept {
  const auto* value = node(value_.get());
  return value == nullptr ? std::nullopt : value->number();
}

std::optional<std::string_view> JsonView::string() const noexcept {
  const auto* value = node(value_.get());
  return value == nullptr ? std::nullopt : value->string();
}

std::size_t JsonView::size() const noexcept {
  const auto* value = node(value_.get());
  return value == nullptr ? 0 : value->size();
}

std::optional<JsonView> JsonView::at(const std::size_t index) const noexcept {
  const auto* value = node(value_.get());
  const auto* elements = value == nullptr ? nullptr : value->array();
  if (elements == nullptr || index >= elements->size()) {
    return std::nullopt;
  }
  return JsonView{std::shared_ptr<const void>{value_, &(*elements)[index]}};
}

std::optional<std::string_view>
JsonView::key_at(const std::size_t index) const noexcept {
  const auto* value = node(value_.get());
  const auto* members = value == nullptr ? nullptr : value->object();
  if (members == nullptr || index >= members->size()) {
    return std::nullopt;
  }
  return std::string_view{(*members)[index].first};
}

std::optional<JsonView> JsonView::find(const std::string_view name) const noexcept {
  const auto* value = node(value_.get());
  const auto* found = value == nullptr ? nullptr : value->find(name);
  if (found == nullptr) {
    return std::nullopt;
  }
  return JsonView{std::shared_ptr<const void>{value_, found}};
}

Json JsonView::to_json() const {
  const auto* value = node(value_.get());
  if (value == nullptr) {
    return Json{.text = "null"};
  }
  return Json{.text = detail::json::write(*value)};
}

std::string escape_json_string(const std::string_view value) {
  return detail::json::quote(value);
}

} // namespace scry
