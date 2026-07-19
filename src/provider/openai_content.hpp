#pragma once

#include "core/model.hpp"
#include "provider/wire_json.hpp"

#include <optional>
#include <scry/error.hpp>
#include <string>
#include <string_view>

namespace scry::detail {

[[nodiscard]] Result<std::string>
canonical_openai_arguments(std::string_view arguments);

[[nodiscard]] Result<ToolCallBlock> decode_openai_tool_call(const WireValue& value);

[[nodiscard]] Result<FinishReason>
decode_openai_finish(std::optional<std::string_view> reason);

[[nodiscard]] Status apply_openai_usage(const WireValue& owner, Usage& usage);

[[nodiscard]] bool is_openai_error(const WireValue& root) noexcept;

[[nodiscard]] Error decode_openai_error(const WireValue& root, std::string_view message,
                                        std::string request_id = {});

} // namespace scry::detail
