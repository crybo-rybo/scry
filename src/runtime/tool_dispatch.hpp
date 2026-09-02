/// @file
/// @brief App-thread dispatch boundary for snapshotted tool registrations.
///
/// Dispatch resolves a provider-neutral call against the accepted turn's immutable
/// snapshot, invokes application code, and converts its result into a bounded neutral
/// tool-result block for the worker.

#pragma once

#include "core/model.hpp"
#include "runtime/state.hpp"

#include <cstddef>
#include <scry/error.hpp>

namespace scry::detail {

/// Dispatches one model-requested tool on the thread running Harness::update().
///
/// Unknown tools, empty handlers, handler exceptions, handler-reported failures, and
/// malformed JSON become bounded model-visible error results whenever possible. A fatal
/// framework resource limit is returned as an unexpected Error so the pump can suppress
/// the remainder of the provider batch before further side effects.
///
/// @param snapshot Immutable registrations captured when the turn was accepted.
/// @param call Provider-neutral call identifier, name, and canonical arguments.
/// @param max_result_bytes Maximum serialized bytes allowed for this result.
/// @return Canonical result, or a fatal error that terminates the tool batch.
[[nodiscard]] Result<ToolResultBlock> dispatch_tool(const ToolSnapshot& snapshot,
                                                    const ToolCallBlock& call,
                                                    std::size_t max_result_bytes);

} // namespace scry::detail
