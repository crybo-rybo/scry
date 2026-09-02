/// @file
/// @brief Implements config-driven selection of built-in provider strategies.
///
/// The closed factory is deliberately internal: adding a public plugin system
/// is deferred until a concrete third-party provider requires its lifecycle and
/// compatibility contract.

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
  }
  std::unreachable();
}

} // namespace scry::detail
