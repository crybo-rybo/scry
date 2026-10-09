#pragma once

#include <cstddef>
#include <scry/config.hpp>
#include <scry/error.hpp>

namespace scry::detail {

// The worker keeps this many bytes of every turn's queued-event budget for the
// terminal event, so a turn's outcome always fits in the queue.
inline constexpr std::size_t terminal_event_reserve = 512;

// The smallest max_queued_event_bytes_per_turn that validation accepts. It
// exceeds the reserve, so the worker's streamed-event limit never underflows.
inline constexpr std::size_t minimum_queued_event_bytes_per_turn = 1024;

static_assert(minimum_queued_event_bytes_per_turn > terminal_event_reserve,
              "the per-turn queued-event minimum must exceed the terminal reserve");

[[nodiscard]] Status validate_config(const Config& config);

} // namespace scry::detail
