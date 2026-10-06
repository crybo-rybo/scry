#pragma once

// The kernel is the part of Scry that parses untrusted bytes or does retry and
// transport arithmetic: the JSON codec, the SSE parser, retry delays, and the
// transport seam with its libcurl implementation. Its sources are compiled as
// C++23 without reflection in every build, so clang-tidy can analyze them
// while the rest of src/ is free to use C++26 reflection.
//
// Every kernel source file includes this header first. Kernel headers do not:
// the rest of src/ includes them from C++26 translation units. Compiling the
// kernel's own sources as C++23 is what keeps those headers, and everything they
// include, free of reflection syntax: an annotation or a reflection operator
// fails to compile there (CMakeLists.txt makes GCC's C++26-extension warning for
// an annotation an error). The check below catches a build that enables reflection
// for the kernel sources anyway.
//
// Kernel code may include only the standard library, libcurl, other kernel
// headers, and the public headers <scry/config.hpp>, <scry/error.hpp>,
// <scry/json.hpp>, <scry/turn_id.hpp>, and <scry/unique_function.hpp>.
// scripts/lint.sh checks that list.

#if defined(__cpp_impl_reflection)
#error "src/kernel/ is C++23 without reflection; compile it without -freflection"
#endif
