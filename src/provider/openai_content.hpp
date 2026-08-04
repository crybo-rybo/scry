#pragma once

#include "core/json_codec.hpp"
#include "core/model.hpp"

#include <optional>
#include <scry/error.hpp>
#include <string>
#include <string_view>

namespace scry::detail {

[[nodiscard]] Status validate_openai_arguments(std::string_view arguments);

[[nodiscard]] Result<FinishReason>
decode_openai_finish(std::optional<std::string_view> reason);

[[nodiscard]] Status apply_openai_usage(const JsonValue& owner, Usage& usage);

[[nodiscard]] bool is_openai_error(const JsonValue& root) noexcept;

[[nodiscard]] Error decode_openai_error(const JsonValue& root, std::string_view message,
                                        std::string request_id = {});

} // namespace scry::detail
