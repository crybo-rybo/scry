#include "core/provider.hpp"

#include "provider/anthropic.hpp"
#include "provider/openai.hpp"

#include <memory>
#include <utility>

namespace scry::detail {

std::unique_ptr<ProviderAdapter> make_provider_adapter(const ProviderDialect dialect) {
  switch (dialect) {
  case ProviderDialect::anthropic:
    return std::make_unique<AnthropicAdapter>();
  case ProviderDialect::openai_compatible:
    return std::make_unique<OpenAiAdapter>();
  case ProviderDialect::llamad:
    // llamad is gRPC, not HTTP: it has its own ModelBackend and no adapter.
    return nullptr;
  }
  std::unreachable();
}

} // namespace scry::detail
