/// @file
/// @brief Shared Anthropic content, finish-reason, and usage decoders.
///
/// Request-independent field mappings live here so initial and incremental Anthropic
/// stream events use the same neutral interpretation.

#pragma once

#include "core/json_codec.hpp"
#include "core/model.hpp"

#include <optional>
#include <string_view>

namespace scry::detail {

/// @brief Decodes one required Anthropic content block.
/// @param value Parsed content-block object.
/// @param streaming_start When true, a `tool_use` block records ID/name but
///        leaves arguments empty for later `input_json_delta` fragments.
/// @return Neutral text/tool-call content, or a protocol error for malformed or
///         unsupported required content.
[[nodiscard]] Result<ContentBlock> decode_anthropic_content(const JsonValue& value,
                                                            bool streaming_start);

/// @brief Maps an Anthropic stop reason to the provider-neutral enum.
/// @param reason Nullable/absent stop reason view.
/// @return Mapped reason; absent and forward-compatible unknown values become
///         `FinishReason::unknown`.
[[nodiscard]] Result<FinishReason>
decode_anthropic_finish(std::optional<std::string_view> reason);

/// @brief Applies an incremental Anthropic usage object to running counts.
///
/// Anthropic reports usage in pieces, so an omitted counter preserves the
/// value already stored in `usage` rather than clearing it.
///
/// @param owner Event object that may contain `usage`.
/// @param usage Running neutral counters to update.
/// @return Success when absent/well formed, or a protocol error.
[[nodiscard]] Status apply_anthropic_usage(const JsonValue& owner, Usage& usage);

} // namespace scry::detail
