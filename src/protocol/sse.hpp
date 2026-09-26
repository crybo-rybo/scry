#pragma once

#include <cstddef>
#include <scry/error.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace scry::detail {

struct SseEvent {
  std::string name{};
  std::string data{};

  bool operator==(const SseEvent&) const = default;
};

class SseParser {
public:
  explicit SseParser(std::size_t max_event_bytes);

  // The caller owns the destination, so a streaming consumer reuses one vector
  // across every chunk instead of allocating a fresh one per push. A failed
  // call leaves whatever it already appended in place; the caller discards the
  // chunk with the error.
  [[nodiscard]] Status push(std::string_view bytes, std::vector<SseEvent>& events);
  [[nodiscard]] Status finish(std::vector<SseEvent>& events);

  [[nodiscard]] std::size_t buffered_bytes() const noexcept;

private:
  void process_line(std::string_view line, std::vector<SseEvent>& events);
  void dispatch(std::vector<SseEvent>& events);

  std::size_t max_event_bytes_{};
  std::size_t event_bytes_{};
  // The current line's bytes so far; it never holds a line terminator.
  std::string input_buffer_{};
  std::string event_name_{};
  std::string data_{};
  bool has_data_{false};
  // The previous chunk ended on a carriage return, so a line feed opening the
  // next chunk completes that CRLF rather than ending an empty line.
  bool after_carriage_return_{false};
};

} // namespace scry::detail
