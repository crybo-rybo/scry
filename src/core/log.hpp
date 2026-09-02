/// @file
/// @brief Compile-time optional, privacy-preserving diagnostic logging.
///
/// Call sites use `SCRY_LOG(...)`, which disappears entirely unless Scry was
/// built with `SCRY_ENABLE_LOGGING`. Logged messages may contain coarse
/// lifecycle facts only—never credentials, prompts, tool arguments, or tool
/// results (SCRY-ERR-004).

#pragma once

#if defined(SCRY_ENABLE_LOGGING)

#include <format>
#include <scry/error.hpp>
#include <string_view>
#include <utility>

namespace scry::detail {

/// @brief Appends one timestamped diagnostic line when an explicit sink exists.
///
/// `SCRY_LOG_FILE` names the destination. When it is unset or empty, no file is
/// opened and every line is dropped. All failures are swallowed so diagnostics
/// can never alter library behavior.
///
/// @param message Sanitized lifecycle diagnostic to append. The view is
///        borrowed for this call only.
void log_line(std::string_view message) noexcept;

/// @brief Returns the stable diagnostic spelling of an error category.
/// @param category Category to render.
/// @return A static string; unknown underlying values map to `"unknown"`.
[[nodiscard]] constexpr std::string_view
error_category_name(const ErrorCategory category) noexcept {
  switch (category) {
  case ErrorCategory::invalid_config:
    return "invalid_config";
  case ErrorCategory::invalid_state:
    return "invalid_state";
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

/// @brief Formats and writes a diagnostic event without affecting control flow.
/// @tparam Args Format argument types.
/// @param fmt Compile-time checked format string.
/// @param args Values to format; callers must not pass sensitive content.
/// @note Allocation/formatting failures are deliberately ignored.
template <typename... Args>
void log_event(const std::format_string<Args...> fmt, Args&&... args) noexcept {
  try {
    log_line(std::format(fmt, std::forward<Args>(args)...));
  } catch (...) { // NOLINT(bugprone-empty-catch)
    // Formatting can only fail on allocation; drop the line rather than throw.
  }
}

} // namespace scry::detail

/// @brief Emits a sanitized diagnostic in logging-enabled builds.
#define SCRY_LOG(...) ::scry::detail::log_event(__VA_ARGS__)

#else

/// @brief Compiles diagnostic call sites away in ordinary builds.
#define SCRY_LOG(...) static_cast<void>(0)

#endif
