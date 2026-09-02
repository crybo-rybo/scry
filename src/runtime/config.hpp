/// @file
/// @brief Internal validation for the public runtime configuration aggregate.
///
/// Configuration is validated before provider, transport, or worker-thread state is
/// created. Keeping that policy at this boundary lets every downstream component assume
/// it received a coherent, provider-compatible Config.

#pragma once

#include <scry/config.hpp>
#include <scry/error.hpp>

namespace scry::detail {

/// Validates every cross-field and provider-specific Config invariant.
///
/// This check is synchronous and side-effect free. It covers endpoint and
/// authentication syntax, dialect-specific sampling and reasoning rules, retry policy
/// coherence, and nonzero transport/resource bounds.
///
/// @param config Candidate configuration supplied to Harness::create().
/// @return Success, or ErrorCategory::invalid_config for the first violation.
[[nodiscard]] Status validate_config(const Config& config);

} // namespace scry::detail
