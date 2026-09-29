#pragma once

#include "core/model.hpp"
#include "kernel/json/codec.hpp"

#include <optional>
#include <string_view>

namespace scry::detail {

[[nodiscard]] Result<ContentBlock> decode_anthropic_content(const JsonValue& value,
                                                            bool streaming_start);

[[nodiscard]] FinishReason
decode_anthropic_finish(std::optional<std::string_view> reason);

[[nodiscard]] Status apply_anthropic_usage(const JsonValue& owner, Usage& usage);

} // namespace scry::detail
