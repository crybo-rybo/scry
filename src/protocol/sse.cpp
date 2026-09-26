#include "protocol/sse.hpp"

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

// Two single-byte searches rather than one find_first_of: libstdc++ lowers
// find(char) to memchr, while a two-byte set search walks the buffer a
// character at a time and measured several times slower on long lines. The
// carriage-return search is bounded by the line feed already found, so a chunk
// holding many LF-terminated lines is not rescanned to its end for every line.
[[nodiscard]] std::size_t line_ending_at(const std::string_view input) noexcept {
  const auto newline = input.find('\n');
  const auto line =
      newline == std::string_view::npos ? input : input.substr(0, newline);
  const auto carriage_return = line.find('\r');
  return carriage_return == std::string_view::npos ? newline : carriage_return;
}

} // namespace

SseParser::SseParser(const std::size_t max_event_bytes)
    : max_event_bytes_(max_event_bytes) {}

Status SseParser::push(std::string_view bytes, std::vector<SseEvent>& events) {
  if (after_carriage_return_ && !bytes.empty()) {
    after_carriage_return_ = false;
    if (bytes.front() == '\n') {
      bytes.remove_prefix(1);
    }
  }
  while (!bytes.empty()) {
    // Only the new bytes are scanned: the buffered prefix holds no terminator,
    // so a long line arriving in many chunks is scanned once, not per chunk.
    const auto ending = line_ending_at(bytes);
    // A terminator is charged one byte, whether it is CR, LF, or CRLF.
    const auto count = ending == std::string_view::npos ? bytes.size() : ending + 1;
    if (exceeds(buffered_bytes(), count, max_event_bytes_)) {
      return std::unexpected(size_error());
    }
    if (ending == std::string_view::npos) {
      input_buffer_.append(bytes);
      return {};
    }

    input_buffer_.append(bytes.substr(0, ending));
    event_bytes_ += input_buffer_.size() + 1;
    process_line(input_buffer_, events);
    input_buffer_.clear();

    auto terminator = std::size_t{1};
    if (bytes[ending] == '\r') {
      if (ending + 1 == bytes.size()) {
        after_carriage_return_ = true;
      } else if (bytes[ending + 1] == '\n') {
        terminator = 2;
      }
    }
    bytes.remove_prefix(ending + terminator);
  }
  return {};
}

Status SseParser::finish(std::vector<SseEvent>& events) {
  after_carriage_return_ = false;
  if (!input_buffer_.empty()) {
    // The size comes from std::string::size(), whose max_size() leaves room for
    // the implicit line terminator charged here.
    if (exceeds(event_bytes_, input_buffer_.size() + 1, max_event_bytes_)) {
      return std::unexpected(size_error());
    }
    process_line(input_buffer_, events);
    input_buffer_.clear();
  }
  dispatch(events);
  return {};
}

std::size_t SseParser::buffered_bytes() const noexcept {
  return event_bytes_ + input_buffer_.size();
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
  event_bytes_ = 0;
  event_name_.clear();
  data_.clear();
  has_data_ = false;
}

} // namespace scry::detail
