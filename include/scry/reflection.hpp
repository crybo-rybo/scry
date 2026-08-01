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

} // namespace scry::reflection
