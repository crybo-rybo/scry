#pragma once

#include <string_view>

// The single source of the release number. CMake refuses to configure when
// these values disagree with the project version, and the public-api
// contract test pins the resulting string.
#define SCRY_VERSION_MAJOR 0
#define SCRY_VERSION_MINOR 0
#define SCRY_VERSION_PATCH 1

// Encoded for consumer preprocessor gates: version 1.2.3 encodes as 10203.
#define SCRY_VERSION                                                                   \
  (SCRY_VERSION_MAJOR * 10000 + SCRY_VERSION_MINOR * 100 + SCRY_VERSION_PATCH)

#define SCRY_DETAIL_VERSION_STRINGIZE(value) #value
#define SCRY_DETAIL_VERSION_EXPAND(value) SCRY_DETAIL_VERSION_STRINGIZE(value)

namespace scry {

inline constexpr int version_major = SCRY_VERSION_MAJOR;
inline constexpr int version_minor = SCRY_VERSION_MINOR;
inline constexpr int version_patch = SCRY_VERSION_PATCH;
inline constexpr std::string_view version =
    SCRY_DETAIL_VERSION_EXPAND(SCRY_VERSION_MAJOR) "." SCRY_DETAIL_VERSION_EXPAND(
        SCRY_VERSION_MINOR) "." SCRY_DETAIL_VERSION_EXPAND(SCRY_VERSION_PATCH);

} // namespace scry

#undef SCRY_DETAIL_VERSION_EXPAND
#undef SCRY_DETAIL_VERSION_STRINGIZE
