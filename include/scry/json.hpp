#pragma once

/**
 * @file json.hpp
 * @brief Dependency-free JSON text value used at Scry's public boundaries.
 *
 * Json intentionally stores text rather than exposing the internal JSON library. Each
 * operation validates and, where required, canonicalizes the value at its own semantic
 * boundary.
 */

#include <string>

namespace scry {

/// Scry-owned serialized JSON boundary value.
///
/// Json is an owning UTF-8 text container, not a parsed DOM and not an assertion that
/// its contents are valid. Callers provide syntactically valid JSON; operations that
/// require a particular shape parse and validate it, returning the error category
/// appropriate to that boundary. Tool schemas and arguments commonly require an
/// object, while reflected result encoding may produce any supported JSON value.
///
/// Canonical values emitted by Scry use lexicographically ordered object keys, but
/// arbitrary caller-created Json values are not canonical until accepted by an API
/// that promises canonicalization. No third-party type or header crosses this boundary.
struct Json {
  /// Owned UTF-8 JSON text, possibly empty or invalid until a consumer validates it.
  std::string text{};
};

} // namespace scry
