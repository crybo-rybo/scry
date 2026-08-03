#include "core/provider.hpp"

#include "provider/anthropic.hpp"
#include "provider/openai.hpp"

#include <memory>

namespace scry::detail {

std::unique_ptr<ProviderAdapter> make_provider_adapter(const ProviderDialect dialect) {
  if (dialect == ProviderDialect::openai_compatible) {
    return std::make_unique<OpenAiAdapter>();
  }
  return std::make_unique<AnthropicAdapter>();
}

} // namespace scry::detail
