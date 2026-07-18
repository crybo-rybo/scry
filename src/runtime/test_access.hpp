#pragma once

#include "core/provider.hpp"
#include "core/transport.hpp"

#include <memory>
#include <scry/config.hpp>
#include <scry/harness.hpp>

namespace scry::detail {

class HarnessTestAccess final {
public:
  [[nodiscard]] static Result<Harness> create(Config config,
                                              std::unique_ptr<ProviderAdapter> provider,
                                              std::unique_ptr<Transport> transport);
};

} // namespace scry::detail
