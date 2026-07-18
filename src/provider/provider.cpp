#include "core/provider.hpp"

#include "provider/anthropic.hpp"
#include "provider/wire_json.hpp"

#include <memory>

namespace scry::detail {

Result<std::unique_ptr<ProviderAdapter>>
make_provider_adapter(const ProviderDialect dialect) {
  switch (dialect) {
  case ProviderDialect::anthropic:
    return std::make_unique<AnthropicAdapter>();
  case ProviderDialect::openai_compatible:
    return std::unexpected(
        make_provider_error(ErrorCategory::invalid_config,
                            "OpenAI-compatible providers are not available until M4"));
  }
  return std::unexpected(
      make_provider_error(ErrorCategory::invalid_config, "Unknown provider dialect"));
}

} // namespace scry::detail
