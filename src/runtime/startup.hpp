/// @file
/// @brief Failure translation for starting the Harness worker thread.
///
/// Thread creation is the only fallible runtime-start operation translated here. The
/// helper preserves Scry's failure-as-value contract without pretending to recover from
/// allocation failure.

#pragma once

#include <functional>
#include <scry/error.hpp>
#include <system_error>
#include <utility>

namespace scry::detail {

/// Invokes a worker-start operation and translates thread-start system failures.
///
/// `std::jthread` construction reports host resource exhaustion with
/// `std::system_error`. This adapter maps that failure to
/// ErrorCategory::resource_limit; other exceptions, including allocation failure,
/// retain normal C++ semantics.
///
/// @tparam Value Successful value produced by the operation.
/// @tparam Operation Nullary callable that constructs Value.
/// @param operation Construction operation invoked exactly once.
/// @return Constructed value, or a resource-limit error if thread startup fails.
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
