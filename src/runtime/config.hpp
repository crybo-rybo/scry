#pragma once

#include <scry/config.hpp>
#include <scry/error.hpp>

namespace scry::detail {

[[nodiscard]] Status validate_config(const Config& config);

} // namespace scry::detail
