/// @file
/// @brief Shared OpenAI-compatible content, usage, and error decoding helpers.
///
/// These functions centralize mappings needed by strict streaming completion
/// parsing while keeping provider-specific JSON out of the neutral model.

#pragma once

#include "core/json_codec.hpp"
#include "core/model.hpp"

#include <optional>
#include <scry/error.hpp>
#include <string>
#include <string_view>

namespace scry::detail {

/// @brief Validates accumulated OpenAI tool arguments as a JSON object.
/// @param arguments Complete concatenated argument bytes.
/// @return Success, or a protocol error for invalid JSON/non-object roots.
[[nodiscard]] Status validate_openai_arguments(std::string_view arguments);

/// @brief Maps an OpenAI finish reason to the provider-neutral enum.
/// @param reason Nullable/absent finish reason view.
/// @return Mapped reason, `unknown` for absent/forward-compatible values, or a
///         protocol error for the unsupported legacy `function_call` shape.
[[nodiscard]] Result<FinishReason>
decode_openai_finish(std::optional<std::string_view> reason);

/// @brief Replaces neutral usage counters from an optional OpenAI usage object.
///
/// Unlike Anthropic's incremental accounting, a present OpenAI usage object is
/// authoritative: an omitted counter becomes zero.
///
/// @param owner Chunk object that may contain `usage` or JSON null.
/// @param usage Neutral counters to replace when usage is present.
/// @return Success when absent/null/well formed, or a protocol error.
[[nodiscard]] Status apply_openai_usage(const JsonValue& owner, Usage& usage);

/// @brief Detects the top-level OpenAI error envelope.
/// @param root Parsed JSON root.
/// @return Whether an `error` member is present, regardless of its shape.
[[nodiscard]] bool is_openai_error(const JsonValue& root) noexcept;

/// @brief Converts an OpenAI error envelope to a sanitized Scry error.
///
/// Only bounded alphanumeric error type/code tokens enter `provider_detail`;
/// provider prose and prompt content are never copied. Recognized auth,
/// rate-limit, and server tokens select their corresponding categories.
///
/// @param root Parsed error envelope.
/// @param message Scry-owned high-level diagnostic.
/// @param request_id Optional transport correlation identifier.
/// @return Categorized error with provider detail prefixed by `openai:`.
[[nodiscard]] Error decode_openai_error(const JsonValue& root, std::string_view message,
                                        std::string request_id = {});

} // namespace scry::detail
