#include "protocol/sse.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scry::detail::SseEvent;
using scry::detail::SseParser;

constexpr auto stream = std::string_view{": keepalive\r\n"
                                         "event: alpha\r\n"
                                         "data: first\r\n"
                                         "data: second\r\n"
                                         "\r\n"
                                         "event: beta\n"
                                         "data: {\"ok\":true}\n"
                                         "\n"};

[[nodiscard]] std::vector<SseEvent>
parse_chunks(const std::vector<std::string_view>& chunks) {
  SseParser parser{1024};
  std::vector<SseEvent> result{};
  for (const auto chunk : chunks) {
    auto events = parser.push(chunk);
    REQUIRE(events.has_value());
    result.insert(result.end(), std::make_move_iterator(events->begin()),
                  std::make_move_iterator(events->end()));
  }
  auto tail = parser.finish();
  REQUIRE(tail.has_value());
  result.insert(result.end(), std::make_move_iterator(tail->begin()),
                std::make_move_iterator(tail->end()));
  return result;
}

[[nodiscard]] std::vector<SseEvent> expected_events() {
  return {
      SseEvent{.name = "alpha", .data = "first\nsecond"},
      SseEvent{.name = "beta", .data = R"({"ok":true})"},
  };
}

} // namespace

TEST_CASE("SSE parser accepts CRLF, comments, and multiple data lines") {
  CHECK(parse_chunks({stream}) == expected_events());
}

TEST_CASE("SSE parser is invariant at every single split point") {
  const auto expected = expected_events();
  for (std::size_t split = 0; split <= stream.size(); ++split) {
    INFO("split at byte " << split);
    CHECK(parse_chunks({stream.substr(0, split), stream.substr(split)}) == expected);
  }
}

TEST_CASE("SSE parser is invariant when delivered one byte at a time") {
  std::vector<std::string_view> chunks{};
  chunks.reserve(stream.size());
  for (std::size_t offset = 0; offset < stream.size(); ++offset) {
    chunks.push_back(stream.substr(offset, 1));
  }
  CHECK(parse_chunks(chunks) == expected_events());
}

TEST_CASE("SSE parser is invariant across fixed-seed random partitions") {
  std::mt19937 generator{0x5C12'024U};
  std::uniform_int_distribution<std::size_t> width{1, 13};
  const auto expected = expected_events();

  for (int run = 0; run < 100; ++run) {
    std::vector<std::string_view> chunks{};
    for (std::size_t offset = 0; offset < stream.size();) {
      const auto count = std::min(width(generator), stream.size() - offset);
      chunks.push_back(stream.substr(offset, count));
      offset += count;
    }
    INFO("partition " << run);
    CHECK(parse_chunks(chunks) == expected);
  }
}

TEST_CASE("SSE parser flushes an unterminated final event") {
  SseParser parser{64};
  REQUIRE(parser.push("event: done\rdata: yes\r").has_value());
  const auto events = parser.finish();
  REQUIRE(events.has_value());
  REQUIRE(events->size() == 1);
  CHECK(events->front() == SseEvent{.name = "done", .data = "yes"});
  CHECK(parser.buffered_bytes() == 0);
}

TEST_CASE("SSE parser enforces the per-event byte limit") {
  constexpr auto complete = std::string_view{"event: ping\ndata: {}\n\n"};
  SseParser exact{complete.size()};
  const auto accepted = exact.push(complete);
  REQUIRE(accepted.has_value());
  REQUIRE(accepted->size() == 1);

  SseParser too_small{complete.size() - 1};
  const auto rejected = too_small.push(complete);
  REQUIRE_FALSE(rejected.has_value());
  CHECK(rejected.error().category == scry::ErrorCategory::resource_limit);
}

TEST_CASE("SSE parser bounds an event even before a line ending arrives") {
  SseParser parser{8};
  const auto result = parser.push("data: payload-without-newline");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == scry::ErrorCategory::resource_limit);
}

TEST_CASE("SSE parser ignores comment-only and empty dispatches") {
  CHECK(parse_chunks({": comment\n\n\n"}) == std::vector<SseEvent>{});
}

TEST_CASE("SSE parser preserves empty data lines") {
  const auto events = parse_chunks({"data:\ndata: second\n\n"});
  REQUIRE(events.size() == 1);
  CHECK(events.front().name == "message");
  CHECK(events.front().data == "\nsecond");
}
