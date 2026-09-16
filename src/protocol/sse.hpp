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

  auto operator<=>(const SseEvent&) const = default;
};

class SseParser {
public:
  explicit SseParser(std::size_t max_event_bytes);

  // Appending overloads: the caller owns the destination, so a streaming
  // consumer reuses one vector across every chunk instead of allocating a
  // fresh one per push. A failed call leaves whatever it already appended in
  // place; the caller discards the chunk with the error.
  [[nodiscard]] Status push(std::string_view bytes, std::vector<SseEvent>& events);
  [[nodiscard]] Status finish(std::vector<SseEvent>& events);

  [[nodiscard]] Result<std::vector<SseEvent>> push(std::string_view bytes);
  [[nodiscard]] Result<std::vector<SseEvent>> finish();

  [[nodiscard]] std::size_t buffered_bytes() const noexcept;

private:
  [[nodiscard]] Status account_for_line(std::size_t line_bytes);
  void process_line(std::string_view line, std::vector<SseEvent>& events);
  void dispatch(std::vector<SseEvent>& events);
  void process_complete_lines(std::vector<SseEvent>& events,
                              bool accept_trailing_carriage_return = false);
  void reset_event() noexcept;

  std::size_t max_event_bytes_{};
  std::size_t event_bytes_{};
  std::string input_buffer_{};
  std::string event_name_{};
  std::string data_{};
  bool has_data_{false};
};

} // namespace scry::detail
