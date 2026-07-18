#pragma once

#if !defined(SCRY_ENABLE_REFLECTION)
#error "Include <scry/reflection.hpp> through the scry::reflection CMake target"
#endif

#if !defined(__cpp_impl_reflection)
#error "Scry reflection requires a compiler implementing P2996"
#endif

#include <scry/detail/reflection_registration.hpp>

namespace scry::reflection {

inline constexpr bool enabled = true;

} // namespace scry::reflection
