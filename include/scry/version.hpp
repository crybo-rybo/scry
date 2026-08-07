#pragma once

#include <string_view>

/// @brief Scry release major version.
///
/// These macros are the single source of the release number. CMake refuses to configure
/// when they disagree with the project version.
#define SCRY_VERSION_MAJOR 0
/// @brief Scry release minor version.
#define SCRY_VERSION_MINOR 1
/// @brief Scry release patch version.
#define SCRY_VERSION_PATCH 0

/// @brief Integer release version for consumer preprocessor gates.
///
/// Version 1.2.3 encodes as 10203.
#define SCRY_VERSION                                                                   \
  (SCRY_VERSION_MAJOR * 10000 + SCRY_VERSION_MINOR * 100 + SCRY_VERSION_PATCH)

#define SCRY_DETAIL_VERSION_STRINGIZE(value) #value
#define SCRY_DETAIL_VERSION_EXPAND(value) SCRY_DETAIL_VERSION_STRINGIZE(value)

namespace scry {

/// Scry release major version.
inline constexpr int version_major = SCRY_VERSION_MAJOR;
/// Scry release minor version.
inline constexpr int version_minor = SCRY_VERSION_MINOR;
/// Scry release patch version.
inline constexpr int version_patch = SCRY_VERSION_PATCH;
/// Scry semantic version string.
inline constexpr std::string_view version =
    SCRY_DETAIL_VERSION_EXPAND(SCRY_VERSION_MAJOR) "." SCRY_DETAIL_VERSION_EXPAND(
        SCRY_VERSION_MINOR) "." SCRY_DETAIL_VERSION_EXPAND(SCRY_VERSION_PATCH);

} // namespace scry

#undef SCRY_DETAIL_VERSION_EXPAND
#undef SCRY_DETAIL_VERSION_STRINGIZE
