#pragma once

#if !defined(SCRY_ENABLE_REFLECTION)
#error "Include <scry/reflection.hpp> through the scry::reflection CMake target"
#endif

#if !defined(__cpp_impl_reflection)
#error "Scry reflection requires a compiler implementing P2996"
#endif

#include <scry/detail/reflection_registration.hpp>

/// Optional experimental C++26 typed-tool API built on P2996 reflection.
namespace scry::reflection {

/// Whether this installed component was built with C++26 reflection support.
inline constexpr bool enabled = true;

/// Encodes a supported reflected value as Scry's canonical JSON.
///
/// This is the same value encoder used for reflected tool-handler results. The
/// returned text is canonical within the current Scry version but is not a
/// versioned archival format.
/// @tparam Type Value from the closed SupportedValue family.
/// @param value Value to encode without modifying it.
/// @return Canonical JSON, or a reflected-value codec error.
template <SupportedValue Type> [[nodiscard]] Result<Json> encode(const Type& value) {
  return detail::encode_value<Type>(value);
}

} // namespace scry::reflection
