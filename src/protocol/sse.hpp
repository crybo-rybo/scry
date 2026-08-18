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

  [[nodiscard]] Result<std::vector<SseEvent>> push(std::string_view bytes);
  [[nodiscard]] Result<std::vector<SseEvent>> finish();

  [[nodiscard]] std::size_t buffered_bytes() const noexcept;

private:
  [[nodiscard]] Status account_for_line(std::size_t line_bytes);
  [[nodiscard]] Status accept_split_crlf(std::string_view& remaining,
                                         std::vector<SseEvent>& events);
  [[nodiscard]] Status append_limited(std::string_view& remaining);
  void process_line(std::string_view line, std::vector<SseEvent>& events);
  void dispatch(std::vector<SseEvent>& events);
  void process_complete_lines(std::vector<SseEvent>& events,
                              bool accept_trailing_carriage_return = false);
  void compact_input();
  void reset_event() noexcept;
  [[nodiscard]] std::size_t unconsumed_size() const noexcept;
  [[nodiscard]] bool unconsumed_ends_with_cr() const noexcept;

  std::size_t max_event_bytes_{};
  std::size_t event_bytes_{};
  std::size_t input_head_{};
  std::size_t scan_pos_{};
  std::string input_buffer_{};
  std::string event_name_{};
  std::string data_{};
  bool has_data_{false};
};

} // namespace scry::detail
