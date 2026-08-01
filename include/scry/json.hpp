#pragma once

#include <string>

namespace scry {

/// Scry-owned serialized JSON boundary value.
///
/// Callers provide syntactically valid JSON. Operations that require a specific shape
/// validate it and return ErrorCategory::invalid_config or ErrorCategory::tool as
/// appropriate. No third-party JSON type crosses Scry's public boundary.
struct Json {
  /// UTF-8 JSON text.
  std::string text{};
};

} // namespace scry
