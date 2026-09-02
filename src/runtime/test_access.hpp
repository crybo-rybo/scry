#pragma once

#include "core/provider.hpp"
#include "core/transport.hpp"
#include "runtime/worker.hpp"

#include <cstdint>
#include <memory>
#include <scry/config.hpp>
#include <scry/harness.hpp>

namespace scry::detail {

class HarnessTestAccess final {
public:
  /// Builds a Harness over injected components. `time` replaces the worker's
  /// steady clock and retry wait; the public Harness::create always passes the
  /// default, which is the real clock.
  [[nodiscard]] static Result<Harness> create(Config config,
                                              std::unique_ptr<ProviderAdapter> provider,
                                              std::unique_ptr<Transport> transport,
                                              std::uint64_t retry_jitter_seed = 0,
                                              WorkerTimeSource time = {});
  [[nodiscard]] static bool has_current_tool_snapshot(const Harness& harness) noexcept;
};

} // namespace scry::detail
