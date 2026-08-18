#include "protocol/sse.hpp"

#include <algorithm>
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

[[nodiscard]] std::size_t terminator_size(const std::string& input,
                                          const std::size_t position) noexcept {
  // A whole chunk may contain standalone CR line endings, not only CRLF.
  if (input[position] == '\r' && position + 1 < input.size() &&
      input[position + 1] == '\n') {
    return 2;
  }
  return 1;
}

} // namespace

SseParser::SseParser(const std::size_t max_event_bytes)
    : max_event_bytes_(max_event_bytes) {}

std::size_t SseParser::unconsumed_size() const noexcept {
  return input_buffer_.size() - input_head_;
}

bool SseParser::unconsumed_ends_with_cr() const noexcept {
  return unconsumed_size() != 0 && input_buffer_.back() == '\r';
}

std::size_t SseParser::buffered_bytes() const noexcept {
  return event_bytes_ + unconsumed_size();
}

void SseParser::compact_input() {
  if (input_head_ == 0) {
    return;
  }
  if (input_head_ == input_buffer_.size()) {
    input_buffer_.clear();
    input_head_ = 0;
    scan_pos_ = 0;
    return;
  }
  input_buffer_.erase(0, input_head_);
  scan_pos_ = scan_pos_ < input_head_ ? 0 : scan_pos_ - input_head_;
  input_head_ = 0;
}

Status SseParser::append_limited(std::string_view& remaining) {
  if (buffered_bytes() >= max_event_bytes_) {
    return std::unexpected(size_error());
  }
  const auto room = max_event_bytes_ - buffered_bytes();
  const auto count = std::min(remaining.size(), room);
  input_buffer_.append(remaining.substr(0, count));
  remaining.remove_prefix(count);
  return {};
}

Status SseParser::accept_split_crlf(std::string_view& remaining,
                                    std::vector<SseEvent>& events) {
  if (!unconsumed_ends_with_cr()) {
    return {};
  }
  if (!remaining.empty() && remaining.front() == '\n') {
    if (exceeds(buffered_bytes(), 1, max_event_bytes_)) {
      return std::unexpected(size_error());
    }
    input_buffer_.push_back('\n');
    remaining.remove_prefix(1);
  }
  process_complete_lines(events, true);
  return {};
}

Result<std::vector<SseEvent>> SseParser::push(const std::string_view bytes) {
  auto remaining = bytes;
  std::vector<SseEvent> events{};
  while (!remaining.empty()) {
    if (unconsumed_ends_with_cr()) {
      auto status = accept_split_crlf(remaining, events);
      if (!status) {
        return std::unexpected(std::move(status.error()));
      }
      continue;
    }
    auto appended = append_limited(remaining);
    if (!appended) {
      return std::unexpected(std::move(appended.error()));
    }
    process_complete_lines(events);
    if (!remaining.empty() && buffered_bytes() >= max_event_bytes_) {
      return std::unexpected(size_error());
    }
  }
  return events;
}

Result<std::vector<SseEvent>> SseParser::finish() {
  std::vector<SseEvent> events{};
  process_complete_lines(events, true);

  if (unconsumed_size() != 0) {
    auto status = account_for_line(unconsumed_size());
    if (!status) {
      return std::unexpected(std::move(status.error()));
    }
    process_line(std::string_view{input_buffer_}.substr(input_head_), events);
    input_buffer_.clear();
    input_head_ = 0;
    scan_pos_ = 0;
  }
  dispatch(events);
  return events;
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

void SseParser::process_complete_lines(std::vector<SseEvent>& events,
                                       const bool accept_trailing_carriage_return) {
  auto consumed = input_head_;
  auto search_from = std::max(consumed, scan_pos_);
  while (true) {
    const auto ending = input_buffer_.find_first_of("\r\n", search_from);
    if (ending == std::string::npos) {
      scan_pos_ = input_buffer_.size();
      break;
    }
    if (input_buffer_[ending] == '\r' && ending + 1 == input_buffer_.size() &&
        !accept_trailing_carriage_return) {
      scan_pos_ = ending;
      break;
    }

    // push() bounds event_bytes_ + unconsumed input before appending.
    // A complete line's implicit terminator is already present in that buffer,
    // so accounting it cannot exceed the configured event limit.
    event_bytes_ += ending - consumed + 1;
    process_line(std::string_view{input_buffer_}.substr(consumed, ending - consumed),
                 events);
    consumed = ending + terminator_size(input_buffer_, ending);
    search_from = consumed;
  }
  input_head_ = consumed;
  compact_input();
}

void SseParser::reset_event() noexcept {
  event_bytes_ = 0;
  event_name_.clear();
  data_.clear();
  has_data_ = false;
}

} // namespace scry::detail
