#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <scry/turn_id.hpp>
#include <string>

namespace scry {

enum class ErrorCategory : std::uint8_t {
  invalid_config,
  invalid_state,
  busy,
  authentication,
  rate_limit,
  network,
  protocol,
  resource_limit,
  tool,
  max_tool_rounds,
  cancelled,
};

struct Error {
  ErrorCategory category{ErrorCategory::invalid_state};
  std::string message{};
  std::string provider_detail{};
  bool retryable{false};
  std::optional<TurnId> turn_id{};
  std::uint32_t attempt{};
  std::string provider_request_id{};
};

template <typename T> using Result = std::expected<T, Error>;

using Status = Result<void>;

} // namespace scry
