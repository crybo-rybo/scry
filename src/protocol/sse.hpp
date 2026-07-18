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
  void process_line(std::string_view line, std::vector<SseEvent>& events);
  void dispatch(std::vector<SseEvent>& events);
  [[nodiscard]] Result<std::vector<SseEvent>>
  process_complete_lines(bool accept_trailing_carriage_return = false);
  void reset_event() noexcept;

  std::size_t max_event_bytes_{};
  std::size_t event_bytes_{};
  std::string input_buffer_{};
  std::string event_name_{};
  std::string data_{};
  bool has_data_{false};
};

} // namespace scry::detail
