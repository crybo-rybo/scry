#pragma once

#include "core/provider.hpp"
#include "core/transport.hpp"

#include <cstdint>
#include <memory>
#include <scry/config.hpp>
#include <scry/harness.hpp>

namespace scry::detail {

class HarnessTestAccess final {
public:
  [[nodiscard]] static Result<Harness> create(Config config,
                                              std::unique_ptr<ProviderAdapter> provider,
                                              std::unique_ptr<Transport> transport,
                                              std::uint64_t retry_jitter_seed = 0);
  [[nodiscard]] static bool has_current_tool_snapshot(const Harness& harness) noexcept;
};

} // namespace scry::detail
