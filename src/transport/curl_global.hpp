/// @file
/// @brief Process-wide libcurl initialization and capability validation.
///
/// Scry permits one global curl initialization attempt, shared immutably by all
/// Harnesses. The runtime must be new enough, globally thread-safe, and backed
/// by asynchronous DNS so bounded shutdown remains credible.

#pragma once

#include <cstdint>
#include <scry/error.hpp>

namespace scry::detail {

/// @brief Dependency-free snapshot of runtime libcurl capabilities.
///
/// Keeping this value free of curl declarations allows deterministic unit tests
/// of capability policy without initializing the process-global runtime.
struct CurlRuntimeCapabilities {
  std::uint32_t version_number{}; ///< Curl's packed hexadecimal version number.
  bool thread_safe{};             ///< `CURL_VERSION_THREADSAFE` is available.
  bool asynchronous_dns{};        ///< `CURL_VERSION_ASYNCHDNS` is available.
};

/// @brief Validates the minimum libcurl runtime contract.
/// @param capabilities Snapshot obtained after global initialization.
/// @return Success, or `invalid_config` naming the missing requirement.
[[nodiscard]] Status
validate_curl_runtime_capabilities(CurlRuntimeCapabilities capabilities);

/// @brief Initializes and validates process-wide libcurl exactly once.
///
/// The first result, including failure, is cached for the rest of the process.
/// Capability failure after successful initialization triggers immediate
/// cleanup; success is paired with exactly one cleanup at static teardown.
/// Function-static initialization provides synchronization without mutable
/// Harness-global state.
///
/// @return Cached initialization/capability status by value.
[[nodiscard]] Status curl_global_status();

} // namespace scry::detail
