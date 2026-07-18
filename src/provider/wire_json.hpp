#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>
#include <optional>
#include <scry/error.hpp>
#include <string>
#include <string_view>

namespace scry::detail {

using WireValue = glz::generic_u64;

[[nodiscard]] Error make_provider_error(ErrorCategory category, std::string message,
                                        bool retryable = false);

[[nodiscard]] Result<WireValue> parse_wire_json(std::string_view input,
                                                ErrorCategory category,
                                                std::string_view failure_message);

[[nodiscard]] Result<std::string> write_wire_json(const WireValue& value,
                                                  ErrorCategory category,
                                                  std::string_view failure_message);

[[nodiscard]] const WireValue* wire_field(const WireValue& value,
                                          std::string_view name) noexcept;

[[nodiscard]] Result<std::string_view> required_wire_string(const WireValue& value,
                                                            std::string_view name);

[[nodiscard]] Result<const WireValue::array_t*>
required_wire_array(const WireValue& value, std::string_view name);

[[nodiscard]] Result<std::optional<std::string_view>>
optional_wire_string(const WireValue& value, std::string_view name);

[[nodiscard]] Result<std::optional<std::uint64_t>>
optional_wire_uint(const WireValue& value, std::string_view name);

} // namespace scry::detail
