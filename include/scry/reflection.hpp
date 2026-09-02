#pragma once

/**
 * @file reflection.hpp
 * @brief Entry point for Scry's optional experimental C++26 reflection component.
 *
 * This header requires the `scry::reflection` CMake target and a compiler implementing
 * Scry's required P2996/P3394 surface. It exposes typed schema generation, strict
 * reflected registration, description annotations, and direct value encoding while
 * lowering all runtime work to the stable C++23 registry and Scry-owned Json boundary.
 */

#if !defined(SCRY_ENABLE_REFLECTION)
#error "Include <scry/reflection.hpp> through the scry::reflection CMake target"
#endif

#if !defined(__cpp_impl_reflection)
#error "Scry reflection requires a compiler implementing P2996"
#endif

#include <scry/detail/reflection_registration.hpp>
#include <utility>

/// Optional experimental C++26 typed-tool API built on P2996 reflection.
///
/// The namespace is available only when the optional package component is enabled. Its
/// concepts define a deliberately closed supported-value matrix; support in Scry's
/// private JSON dependency does not implicitly expand this contract.
namespace scry::reflection {

/// Encodes a supported reflected value as Scry's canonical JSON.
///
/// This is the same recursive value encoder and error mapping used for reflected
/// tool-handler results. Aggregate keys are sorted lexicographically, enum values use
/// their exact declared names, and optionals/sequences follow the schema mapping
/// described by SupportedValue. Invalid runtime enum values and non-finite floating
/// values are reported as ErrorCategory::tool.
///
/// The returned text is canonical within the current Scry version but is not a
/// versioned archival format and carries no cross-version byte guarantee. The operation
/// has no registry, Harness, I/O, or global-state dependency.
/// @tparam Type Value from the closed SupportedValue family.
/// @param value Value to encode without modifying it.
/// @return Canonical JSON, or a reflected-value codec error.
/// @see input_schema_v
/// @see add
template <SupportedValue Type> [[nodiscard]] Result<Json> encode(const Type& value) {
  auto encoded = detail::encode_value<Type>(value);
  if (!encoded) {
    return encoded;
  }
  return detail::canonicalize_encoded_json(std::move(*encoded));
}

} // namespace scry::reflection
