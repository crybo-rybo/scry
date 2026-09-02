#pragma once

/**
 * @file version.hpp
 * @brief Compile-time Scry release version macros and typed constants.
 *
 * CMake verifies these values against the project version at configure time. Macros
 * support preprocessor feature gates; the namespace constants are preferred in normal
 * C++ expressions.
 */

#include <string_view>

/// Scry release major version component.
///
/// These macros are the single source of the release number. CMake refuses to configure
/// when they disagree with the project version.
#define SCRY_VERSION_MAJOR 0
/// Scry release minor version component.
#define SCRY_VERSION_MINOR 2
/// Scry release patch version component.
#define SCRY_VERSION_PATCH 0

/// Integer release version for consumer preprocessor gates.
///
/// Version 1.2.3 encodes as 10203, allowing comparisons such as
/// `#if SCRY_VERSION >= 10203` without string parsing.
#define SCRY_VERSION                                                                   \
  (SCRY_VERSION_MAJOR * 10000 + SCRY_VERSION_MINOR * 100 + SCRY_VERSION_PATCH)

/// Internal first-stage preprocessor stringification helper.
/// @param value Token sequence to stringify without an extra expansion pass.
#define SCRY_DETAIL_VERSION_STRINGIZE(value) #value
/// Internal expansion helper used to stringify a version component's macro value.
/// @param value Macro or token sequence expanded before stringification.
#define SCRY_DETAIL_VERSION_EXPAND(value) SCRY_DETAIL_VERSION_STRINGIZE(value)

namespace scry {

/// Typed Scry release major version component.
inline constexpr int version_major = SCRY_VERSION_MAJOR;
/// Typed Scry release minor version component.
inline constexpr int version_minor = SCRY_VERSION_MINOR;
/// Typed Scry release patch version component.
inline constexpr int version_patch = SCRY_VERSION_PATCH;
/// Dotted `major.minor.patch` release string with static storage duration.
inline constexpr std::string_view version =
    SCRY_DETAIL_VERSION_EXPAND(SCRY_VERSION_MAJOR) "." SCRY_DETAIL_VERSION_EXPAND(
        SCRY_VERSION_MINOR) "." SCRY_DETAIL_VERSION_EXPAND(SCRY_VERSION_PATCH);

} // namespace scry

#undef SCRY_DETAIL_VERSION_EXPAND
#undef SCRY_DETAIL_VERSION_STRINGIZE
