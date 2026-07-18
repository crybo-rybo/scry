#pragma once

#include <functional>
#include <scry/error.hpp>
#include <system_error>
#include <utility>

namespace scry::detail {

template <typename Value, typename Operation>
[[nodiscard]] Result<Value> translate_worker_start_failure(Operation&& operation) {
  try {
    return Result<Value>{
        std::in_place,
        std::invoke(std::forward<Operation>(operation)),
    };
  } catch (const std::system_error&) {
    return std::unexpected(Error{
        .category = ErrorCategory::resource_limit,
        .message = "Harness worker thread could not be started",
    });
  }
}

} // namespace scry::detail
