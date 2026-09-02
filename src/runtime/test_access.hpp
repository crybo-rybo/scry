/// @file
/// @brief Narrow test-only access to Harness dependency-injection seams.
///
/// Production construction selects the configured provider and Curl transport. Tests
/// use this internal facade to inject deterministic fakes and inspect snapshot timing
/// without widening the installed public API.

#pragma once

#include "core/provider.hpp"
#include "core/transport.hpp"

#include <memory>
#include <scry/config.hpp>
#include <scry/harness.hpp>

namespace scry::detail {

/// Friend facade exposing deterministic Harness construction to internal tests.
///
/// The class owns no state and exists solely to reach the private Harness constructor
/// and implementation observations sanctioned by the runtime test architecture.
class HarnessTestAccess final {
public:
  /// Creates a Harness around caller-supplied provider and transport implementations.
  ///
  /// The same Config validation and worker-start translation used by production
  /// construction still apply.
  ///
  /// @param config Candidate runtime configuration; normal validation still applies.
  /// @param provider Deterministic provider adapter to own.
  /// @param transport Deterministic transport seam to own.
  /// @return Harness, or an immediate configuration, dependency, or startup error.
  [[nodiscard]] static Result<Harness> create(Config config,
                                              std::unique_ptr<ProviderAdapter> provider,
                                              std::unique_ptr<Transport> transport);
  /// Reports whether the registry's frozen snapshot matches its working generation.
  ///
  /// @param harness Harness whose internal ToolRegistry cache is inspected.
  /// @return true when a current frozen registration/schema pair exists.
  [[nodiscard]] static bool has_current_tool_snapshot(const Harness& harness) noexcept;
};

} // namespace scry::detail
