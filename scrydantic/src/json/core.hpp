#pragma once

// The JSON core is the part of Scrydantic that parses untrusted bytes: the
// strict parser, the validator, the canonical writer, and the number spelling.
// Its sources are compiled as C++23 without reflection in every build, so
// clang-tidy can analyze them, and so a C++23 library such as Scry's kernel can
// link them.
//
// Every JSON core source file includes this header first. Its headers do not:
// the reflected codec includes the public ones from C++26 translation units.
// Compiling the core's own sources as C++23 is what keeps those headers, and
// everything they include, free of reflection syntax: an annotation or a
// reflection operator fails to compile there (CMakeLists.txt makes GCC's
// C++26-extension warning for an annotation an error). The check below catches
// a build that enables reflection for the core sources anyway.
//
// JSON core code may include only the standard library, other headers under
// src/json/, and <scrydantic/error.hpp>, <scrydantic/json.hpp>, and
// <scrydantic/detail/json_text.hpp>. scripts/lint.sh checks that list.

#if defined(__cpp_impl_reflection)
#error                                                                                 \
    "scrydantic/src/json/ is C++23 without reflection; compile it without -freflection"
#endif
