/// @file
/// @brief Internal construction helper for Scry's unified error value.
///
/// All implementation layers use this bottom-level helper so failures retain
/// the same public `Error` representation without introducing dependencies
/// between otherwise independent subsystems.

#pragma once

#include <scry/error.hpp>
#include <string>

namespace scry::detail {

/// @brief Constructs a Scry error with the common scalar fields initialized.
///
/// Keeping the factory in the core bottom layer lets machine, provider, and
/// transport code create the library's single error type without depending on
/// an unrelated subsystem such as JSON.
///
/// @param category Stable category used by callers and retry policy.
/// @param message Human-readable diagnostic. Callers must provide sanitized
///        text; this function does not redact content.
/// @param retryable Whether a later attempt may succeed.
/// @return An owning `Error` value with all unspecified metadata left empty.
/// @see Error
[[nodiscard]] Error make_error(ErrorCategory category, std::string message,
                               bool retryable = false);

} // namespace scry::detail
