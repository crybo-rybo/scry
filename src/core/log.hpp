#pragma once

// Compile-time diagnostic logging for the SCRY_ENABLE_LOGGING build. Call
// sites use SCRY_LOG("..."), which compiles to nothing in ordinary builds.
// Logged lines carry coarse lifecycle facts only — turn ids, tool names,
// attempt counts, error categories. Prompt text, tool arguments, tool
// results, and credentials must never reach a log line (ERR-004).

#if defined(SCRY_ENABLE_LOGGING)

#include <format>
#include <scry/error.hpp>
#include <string_view>
#include <utility>

namespace scry::detail {

// Appends one timestamped line to the diagnostic log file named by the
// SCRY_LOG_FILE environment variable. When that variable is unset or empty no
// file is opened and every line is dropped. Logging failures are swallowed:
// diagnostics must never alter library behavior.
void log_line(std::string_view message) noexcept;

[[nodiscard]] constexpr std::string_view
error_category_name(const ErrorCategory category) noexcept {
  switch (category) {
  case ErrorCategory::invalid_config:
    return "invalid_config";
  case ErrorCategory::invalid_state:
    return "invalid_state";
  case ErrorCategory::invalid_argument:
    return "invalid_argument";
  case ErrorCategory::busy:
    return "busy";
  case ErrorCategory::authentication:
    return "authentication";
  case ErrorCategory::rate_limit:
    return "rate_limit";
  case ErrorCategory::network:
    return "network";
  case ErrorCategory::protocol:
    return "protocol";
  case ErrorCategory::resource_limit:
    return "resource_limit";
  case ErrorCategory::tool:
    return "tool";
  case ErrorCategory::max_tool_rounds:
    return "max_tool_rounds";
  case ErrorCategory::cancelled:
    return "cancelled";
  }
  return "unknown";
}

template <typename... Args>
void log_event(const std::format_string<Args...> fmt, Args&&... args) noexcept {
  try {
    log_line(std::format(fmt, std::forward<Args>(args)...));
  } catch (...) { // NOLINT(bugprone-empty-catch)
    // Formatting can only fail on allocation; drop the line rather than throw.
  }
}

} // namespace scry::detail

#define SCRY_LOG(...) ::scry::detail::log_event(__VA_ARGS__)

#else

#define SCRY_LOG(...) static_cast<void>(0)

#endif
