/// @file
/// @brief Provider-neutral JSON parsing, canonicalization, and field access.
///
/// This is the single internal JSON codec used by provider adapters, tool
/// dispatch, and persistence. Its sorted object representation gives every
/// re-serialized value one lexicographic key order. Glaze remains confined to
/// implementation headers and never appears in Scry's installed public API.

#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>
#include <optional>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>

namespace scry::detail {

/// @brief Mutable parsed representation used by every internal JSON boundary.
///
/// The sorted-map object storage makes re-serialization canonical with respect
/// to object-key order: semantically equal parsed objects produce identical
/// ordering regardless of the layer that parsed them.
using JsonValue = glz::generic_sorted_u64;

/// @brief Parses a complete JSON document into the canonical internal tree.
/// @param input UTF-8 JSON bytes; the view is borrowed for this call only.
/// @param category Category to assign if parsing fails.
/// @param failure_message Sanitized diagnostic to use if parsing fails.
/// @return The owning parsed value, or the caller-selected error.
[[nodiscard]] Result<JsonValue> parse_json(std::string_view input,
                                           ErrorCategory category,
                                           std::string_view failure_message);

/// @brief Serializes an internal JSON tree to canonical minified text.
/// @param value Parsed value to serialize.
/// @param category Category to assign if serialization fails.
/// @param failure_message Sanitized diagnostic to use on failure.
/// @return Canonical JSON bytes, or the caller-selected error.
[[nodiscard]] Result<std::string> write_json_text(const JsonValue& value,
                                                  ErrorCategory category,
                                                  std::string_view failure_message);

/// @brief Serializes an internal JSON tree into Scry's public JSON value.
/// @param value Parsed value to serialize.
/// @param category Category to assign if serialization fails.
/// @param failure_message Sanitized diagnostic to use on failure.
/// @return An owning `Json` containing canonical bytes, or an error.
[[nodiscard]] Result<Json> write_json(const JsonValue& value, ErrorCategory category,
                                      std::string_view failure_message);

/// @brief Validates and rewrites an arbitrary JSON value canonically.
/// @param json Scry-owned JSON text to parse.
/// @param category Category used for either parse or write failure.
/// @param failure_message Sanitized diagnostic used for either failure.
/// @return Canonical JSON with lexicographically ordered object keys.
[[nodiscard]] Result<Json> canonicalize_json(const Json& json, ErrorCategory category,
                                             std::string_view failure_message);

/// @brief Validates and canonicalizes a JSON value that must be an object.
/// @param json Scry-owned JSON text to parse.
/// @param category Category used for parse, type, or write failure.
/// @param failure_message Sanitized diagnostic used for any failure.
/// @return Canonical object JSON, or an error if the root is not an object.
[[nodiscard]] Result<Json> canonicalize_json_object(const Json& json,
                                                    ErrorCategory category,
                                                    std::string_view failure_message);

/// @brief Builds the bounded model-visible envelope for a tool error.
///
/// Serialization failure falls back to a fixed valid object rather than
/// allowing diagnostics to change tool-loop control flow.
///
/// @param message Already-bounded message to place in the `error` field.
/// @return Valid canonical object JSON.
[[nodiscard]] Json make_json_error_object(std::string_view message);

/// @brief Finds a named member of a JSON object without changing it.
/// @param value Candidate object.
/// @param name Member name to find.
/// @return A borrowed pointer to the member, or `nullptr` when the root is not
///         an object or the member is absent.
/// @note The pointer remains valid only while `value` is alive and unmodified.
[[nodiscard]] const JsonValue* json_field(const JsonValue& value,
                                          std::string_view name) noexcept;

/// @brief Reads a required string member.
/// @param value Object that owns the member.
/// @param name Required member name.
/// @return A borrowed view into `value`, or a protocol error for absence/type
///         mismatch.
[[nodiscard]] Result<std::string_view> required_json_string(const JsonValue& value,
                                                            std::string_view name);

/// @brief Reads a required array member.
/// @param value Object that owns the member.
/// @param name Required member name.
/// @return A borrowed array pointer, or a protocol error for absence/type
///         mismatch.
/// @note The pointer remains valid only while `value` is alive and unmodified.
[[nodiscard]] Result<const JsonValue::array_t*>
required_json_array(const JsonValue& value, std::string_view name);

/// @brief Reads a required object member.
/// @param value Object that owns the member.
/// @param name Required member name.
/// @return A borrowed member pointer, or a protocol error for absence/type
///         mismatch.
/// @note The pointer remains valid only while `value` is alive and unmodified.
[[nodiscard]] Result<const JsonValue*> required_json_object(const JsonValue& value,
                                                            std::string_view name);

/// @brief Reads a nullable or absent string member.
/// @param value Object that owns the member.
/// @param name Optional member name.
/// @return A borrowed string view when present, `nullopt` for absence/JSON
///         `null`, or a protocol error for another JSON kind.
[[nodiscard]] Result<std::optional<std::string_view>>
optional_json_string(const JsonValue& value, std::string_view name);

/// @brief Reads a nullable or absent unsigned-integer member.
/// @param value Object that owns the member.
/// @param name Optional member name.
/// @return The integer when present, `nullopt` for absence/JSON `null`, or a
///         protocol error for another JSON kind.
[[nodiscard]] Result<std::optional<std::uint64_t>>
optional_json_uint(const JsonValue& value, std::string_view name);

} // namespace scry::detail
