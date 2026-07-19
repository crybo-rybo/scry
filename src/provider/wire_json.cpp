#include "provider/wire_json.hpp"

#include <utility>

namespace scry::detail {
namespace {

[[nodiscard]] Error field_error(const std::string_view name,
                                const std::string_view expected) {
  return make_provider_error(ErrorCategory::protocol,
                             "Provider payload field '" + std::string{name} +
                                 "' must be " + std::string{expected});
}

} // namespace

Error make_provider_error(const ErrorCategory category, std::string message,
                          const bool retryable) {
  return Error{
      .category = category,
      .message = std::move(message),
      .retryable = retryable,
  };
}

Result<WireValue> parse_wire_json(const std::string_view input,
                                  const ErrorCategory category,
                                  const std::string_view failure_message) {
  WireValue value{};
  if (glz::read_json(value, input)) {
    return std::unexpected(make_provider_error(category, std::string{failure_message}));
  }
  return value;
}

Result<std::string> write_wire_json(const WireValue& value,
                                    const ErrorCategory category,
                                    const std::string_view failure_message) {
  auto encoded = glz::write_json(value);
  if (!encoded) {
    return std::unexpected(make_provider_error(category, std::string{failure_message}));
  }
  return std::move(*encoded);
}

const WireValue* wire_field(const WireValue& value,
                            const std::string_view name) noexcept {
  if (!value.is_object()) {
    return nullptr;
  }
  const auto& object = value.get_object();
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

Result<std::string_view> required_wire_string(const WireValue& value,
                                              const std::string_view name) {
  const auto* field = wire_field(value, name);
  if (field == nullptr || !field->is_string()) {
    return std::unexpected(field_error(name, "a string"));
  }
  return field->get_string();
}

Result<const WireValue::array_t*> required_wire_array(const WireValue& value,
                                                      const std::string_view name) {
  const auto* field = wire_field(value, name);
  if (field == nullptr || !field->is_array()) {
    return std::unexpected(field_error(name, "an array"));
  }
  return &field->get_array();
}

Result<std::optional<std::string_view>>
optional_wire_string(const WireValue& value, const std::string_view name) {
  const auto* field = wire_field(value, name);
  if (field == nullptr || field->is_null()) {
    return std::optional<std::string_view>{};
  }
  if (!field->is_string()) {
    return std::unexpected(field_error(name, "a string or null"));
  }
  return std::optional<std::string_view>{field->get_string()};
}

Result<std::optional<std::uint64_t>> optional_wire_uint(const WireValue& value,
                                                        const std::string_view name) {
  const auto* field = wire_field(value, name);
  if (field == nullptr || field->is_null()) {
    return std::optional<std::uint64_t>{};
  }
  if (!field->is_uint64()) {
    return std::unexpected(field_error(name, "an unsigned integer"));
  }
  return std::optional<std::uint64_t>{field->get<std::uint64_t>()};
}

} // namespace scry::detail
