#pragma once

#include "core/json_codec.hpp"
#include "core/model.hpp"

#include <optional>
#include <string_view>

namespace scry::detail {

[[nodiscard]] Result<ContentBlock> decode_anthropic_content(const JsonValue& value,
                                                            bool streaming_start);

[[nodiscard]] Result<FinishReason>
decode_anthropic_finish(std::optional<std::string_view> reason);

[[nodiscard]] Status apply_anthropic_usage(const JsonValue& owner, Usage& usage);

} // namespace scry::detail
