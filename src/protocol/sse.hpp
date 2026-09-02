/// @file
/// @brief Pure incremental parser for Server-Sent Events streams.
///
/// The parser accepts arbitrary byte chunking, including delimiters split
/// across calls, and retains only the current bounded event. It performs no I/O
/// and is shared by every streaming provider dialect.

#pragma once

#include <cstddef>
#include <scry/error.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace scry::detail {

/// @brief One complete SSE event after field folding and defaulting.
struct SseEvent {
  std::string name{}; ///< `event` value, or `"message"` when omitted.
  std::string data{}; ///< Joined `data` lines separated by newline bytes.

  /// @brief Provides value comparison for deterministic parser tests.
  /// @return Lexicographic ordering/equality over `name`, then `data`.
  auto operator<=>(const SseEvent&) const = default;
};

/// @brief Stateful, I/O-free decoder for one bounded SSE byte stream.
///
/// A parser instance belongs to one transfer. `push()` may emit any number of
/// events while retaining an incomplete line/event for the next chunk;
/// `finish()` consumes the final unterminated line and dispatches remaining
/// data. Comment lines and unsupported fields are ignored according to SSE
/// rules. Only events containing at least one `data` field are emitted.
class SseParser {
public:
  /// @brief Creates an empty parser with a per-event byte ceiling.
  /// @param max_event_bytes Maximum retained bytes for one event, including
  ///        parsed fields, line framing, and an incomplete input line.
  explicit SseParser(std::size_t max_event_bytes);

  /// @brief Consumes the next arbitrarily split stream chunk.
  /// @param bytes Borrowed transfer bytes valid only for this call.
  /// @return Complete events in wire order, or `resource_limit` before retained
  ///         state can grow beyond the configured ceiling.
  [[nodiscard]] Result<std::vector<SseEvent>> push(std::string_view bytes);
  /// @brief Signals end of input and dispatches any final partial event.
  /// @return Remaining complete events, or `resource_limit` if final-line
  ///         accounting exceeds the ceiling.
  /// @note The transport calls this when its stream reaches EOF.
  [[nodiscard]] Result<std::vector<SseEvent>> finish();

  /// @brief Reports bytes retained for the current event and partial line.
  /// @return Accounted event bytes plus the incomplete input buffer size.
  [[nodiscard]] std::size_t buffered_bytes() const noexcept;

private:
  /// @brief Accounts a complete line plus its logical terminator.
  /// @param line_bytes Bytes in the delimiter-free line.
  /// @return Success or `resource_limit` without incrementing on failure.
  [[nodiscard]] Status account_for_line(std::size_t line_bytes);
  /// @brief Applies one delimiter-free SSE line to the current event.
  /// @param line Borrowed line content.
  /// @param events Destination receiving an event when `line` is blank.
  void process_line(std::string_view line, std::vector<SseEvent>& events);
  /// @brief Emits the current data-bearing event and resets accumulation.
  /// @param events Destination that receives the event, if one has data.
  void dispatch(std::vector<SseEvent>& events);
  /// @brief Consumes every complete line currently in `input_buffer_`.
  /// @param accept_trailing_carriage_return Whether a final CR may terminate a
  ///        line without waiting to see if a following chunk begins with LF.
  /// @return Events dispatched by blank lines encountered during the scan.
  [[nodiscard]] std::vector<SseEvent>
  process_complete_lines(bool accept_trailing_carriage_return = false);
  /// @brief Clears all fields belonging to the current SSE event.
  void reset_event() noexcept;

  std::size_t max_event_bytes_{}; ///< Immutable retained-byte ceiling.
  std::size_t event_bytes_{};     ///< Accounted bytes in already-processed lines.
  std::string input_buffer_{};    ///< Incomplete and not-yet-processed wire bytes.
  std::string event_name_{};      ///< Most recent `event` field for this event.
  std::string data_{};            ///< Newline-joined data fields for this event.
  bool has_data_{false}; ///< Distinguishes no data field from an empty data field.
};

} // namespace scry::detail
