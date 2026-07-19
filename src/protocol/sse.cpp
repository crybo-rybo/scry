#include "protocol/sse.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace scry::detail {
namespace {

[[nodiscard]] Error size_error() {
  return Error{
      .category = ErrorCategory::resource_limit,
      .message = "SSE event exceeds the configured byte limit",
  };
}

[[nodiscard]] bool exceeds(const std::size_t current, const std::size_t added,
                           const std::size_t limit) noexcept {
  return added > limit || current > limit - added;
}

[[nodiscard]] std::size_t line_ending_at(const std::string& input,
                                         const std::size_t offset) noexcept {
  const auto newline = input.find('\n', offset);
  const auto carriage_return = input.find('\r', offset);
  return std::min(newline, carriage_return);
}

[[nodiscard]] std::size_t terminator_size(const std::string& input,
                                          const std::size_t position) noexcept {
  // push() processes a trailing carriage return before appending more input.
  // Therefore the only byte that can coexist after one here is its LF pair.
  if (input[position] == '\r' && position + 1 < input.size()) {
    return 2;
  }
  return 1;
}

} // namespace

SseParser::SseParser(const std::size_t max_event_bytes)
    : max_event_bytes_(max_event_bytes) {}

Result<std::vector<SseEvent>> SseParser::push(const std::string_view bytes) {
  auto remaining = bytes;
  std::vector<SseEvent> events{};
  while (!remaining.empty()) {
    if (!input_buffer_.empty() && input_buffer_.back() == '\r') {
      if (remaining.front() == '\n') {
        input_buffer_.push_back('\n');
        remaining.remove_prefix(1);
      }
      auto parsed = process_complete_lines(true);
      events.insert(events.end(), std::make_move_iterator(parsed.begin()),
                    std::make_move_iterator(parsed.end()));
      continue;
    }

    const auto ending = remaining.find_first_of("\r\n");
    const auto count = ending == std::string_view::npos ? remaining.size() : ending + 1;
    if (exceeds(buffered_bytes(), count, max_event_bytes_)) {
      return std::unexpected(size_error());
    }
    input_buffer_.append(remaining.substr(0, count));
    remaining.remove_prefix(count);

    auto parsed = process_complete_lines();
    events.insert(events.end(), std::make_move_iterator(parsed.begin()),
                  std::make_move_iterator(parsed.end()));
  }
  return events;
}

Result<std::vector<SseEvent>> SseParser::finish() {
  auto events = process_complete_lines(true);

  if (!input_buffer_.empty()) {
    auto status = account_for_line(input_buffer_.size());
    if (!status) {
      return std::unexpected(std::move(status.error()));
    }
    process_line(input_buffer_, events);
    input_buffer_.clear();
  }
  dispatch(events);
  return events;
}

std::size_t SseParser::buffered_bytes() const noexcept {
  return event_bytes_ + input_buffer_.size();
}

Status SseParser::account_for_line(const std::size_t line_bytes) {
  constexpr auto terminator_bytes = std::size_t{1};
  // line_bytes comes from std::string::size(), whose max_size() leaves room
  // for the implicit SSE line terminator accounted here.
  if (exceeds(event_bytes_, line_bytes + terminator_bytes, max_event_bytes_)) {
    return std::unexpected(size_error());
  }
  event_bytes_ += line_bytes + terminator_bytes;
  return {};
}

void SseParser::process_line(const std::string_view line,
                             std::vector<SseEvent>& events) {
  if (line.empty()) {
    dispatch(events);
    return;
  }
  if (line.front() == ':') {
    return;
  }

  const auto separator = line.find(':');
  const auto field = line.substr(0, separator);
  auto value = separator == std::string_view::npos ? std::string_view{}
                                                   : line.substr(separator + 1);
  if (!value.empty() && value.front() == ' ') {
    value.remove_prefix(1);
  }

  if (field == "event") {
    event_name_.assign(value);
  } else if (field == "data") {
    if (has_data_) {
      data_.push_back('\n');
    }
    data_.append(value);
    has_data_ = true;
  }
}

void SseParser::dispatch(std::vector<SseEvent>& events) {
  if (has_data_) {
    events.push_back(SseEvent{
        .name = event_name_.empty() ? "message" : std::move(event_name_),
        .data = std::move(data_),
    });
  }
  reset_event();
}

std::vector<SseEvent>
SseParser::process_complete_lines(const bool accept_trailing_carriage_return) {
  std::vector<SseEvent> events{};
  std::size_t consumed{};
  while (true) {
    const auto ending = line_ending_at(input_buffer_, consumed);
    if (ending == std::string::npos) {
      break;
    }
    if (input_buffer_[ending] == '\r' && ending + 1 == input_buffer_.size() &&
        !accept_trailing_carriage_return) {
      break;
    }

    // push() bounds event_bytes_ + input_buffer_.size() before appending.
    // A complete line's implicit terminator is already present in that buffer,
    // so accounting it cannot exceed the configured event limit.
    event_bytes_ += ending - consumed + 1;
    process_line(std::string_view{input_buffer_}.substr(consumed, ending - consumed),
                 events);
    consumed = ending + terminator_size(input_buffer_, ending);
  }
  input_buffer_.erase(0, consumed);
  return events;
}

void SseParser::reset_event() noexcept {
  event_bytes_ = 0;
  event_name_.clear();
  data_.clear();
  has_data_ = false;
}

} // namespace scry::detail
