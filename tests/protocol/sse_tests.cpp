#include "protocol/sse.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <scry/error.hpp>
#include <string>
#include <string_view>
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

// The worker hands the parser one vector for a whole response, so every chunk
// appends to the same sink.
[[nodiscard]] std::vector<SseEvent>
parse_chunks(const std::vector<std::string_view>& chunks) {
  SseParser parser{1024};
  std::vector<SseEvent> result{};
  for (const auto chunk : chunks) {
    REQUIRE(parser.push(chunk, result).has_value());
  }
  REQUIRE(parser.finish(result).has_value());
  return result;
}

[[nodiscard]] std::vector<SseEvent> expected_events() {
  return {
      SseEvent{.name = "alpha", .data = "first\nsecond"},
      SseEvent{.name = "beta", .data = R"({"ok":true})"},
  };
}

void check_resource_limit(const scry::Status& status) {
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::resource_limit);
  CHECK(status.error().message == "SSE event exceeds the configured byte limit");
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

TEST_CASE("SSE parser flushes an unterminated final event") {
  SseParser parser{64};
  std::vector<SseEvent> events{};
  REQUIRE(parser.push("event: done\rdata: yes\r", events));
  REQUIRE(parser.finish(events));
  REQUIRE(events.size() == 1);
  CHECK(events.front() == SseEvent{.name = "done", .data = "yes"});
  CHECK(parser.buffered_bytes() == 0);
}

TEST_CASE("SSE parser enforces the per-event byte limit") {
  constexpr auto complete = std::string_view{"event: ping\ndata: {}\n\n"};
  std::vector<SseEvent> events{};
  SseParser exact{complete.size()};
  REQUIRE(exact.push(complete, events));
  REQUIRE(events.size() == 1);

  SseParser too_small{complete.size() - 1};
  check_resource_limit(too_small.push(complete, events));
}

TEST_CASE("SSE parser bounds an event even before a line ending arrives") {
  SseParser parser{8};
  std::vector<SseEvent> events{};
  check_resource_limit(parser.push("data: payload-without-newline", events));
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

TEST_CASE("SSE parser handles CRLF and standalone carriage returns across chunks") {
  std::vector<SseEvent> events{};

  SECTION("split CRLF") {
    SseParser parser{128};
    REQUIRE(parser.push("event: split\r", events));
    REQUIRE(parser.push("\ndata: value\r", events));
    REQUIRE(events.empty());

    // The line feed completes the data line's CRLF; the carriage return ends
    // the blank line that dispatches, without waiting for its line feed.
    REQUIRE(parser.push("\n\r", events));
    REQUIRE(events.size() == 1);
    CHECK(events.front() == SseEvent{.name = "split", .data = "value"});

    REQUIRE(parser.push("\n", events));
    CHECK(events.size() == 1);
    CHECK(parser.buffered_bytes() == 0);
  }

  SECTION("standalone carriage return") {
    SseParser parser{128};
    REQUIRE(parser.push("event: lone\rdata: first\rdata: second\r\r", events));
    REQUIRE(events.size() == 1);
    CHECK(events.front() == SseEvent{.name = "lone", .data = "first\nsecond"});

    REQUIRE(parser.finish(events));
    CHECK(events.size() == 1);
    CHECK(parser.buffered_bytes() == 0);
  }
}

TEST_CASE("SSE parser applies field syntax without carrying empty events forward") {
  SseParser parser{256};
  std::vector<SseEvent> events{};
  REQUIRE(parser.push("event: stale\n"
                      "id: ignored\n"
                      "retry: 20\n"
                      "\n"
                      "event\n"
                      "event: named\n"
                      ": comment\n"
                      "data\n"
                      "data:  second\n"
                      "unknown-field\n"
                      "\n"
                      "data:value\n"
                      "\n",
                      events));

  REQUIRE(events.size() == 2);
  CHECK(events[0] == SseEvent{.name = "named", .data = "\n second"});
  CHECK(events[1] == SseEvent{.name = "message", .data = "value"});
  CHECK(parser.buffered_bytes() == 0);
}

TEST_CASE("SSE parser dispatches multiple events and safely finishes empty state") {
  SseParser parser{128};
  std::vector<SseEvent> events{};
  REQUIRE(parser.push({}, events));
  REQUIRE(events.empty());

  REQUIRE(parser.push("data: one\n\nevent: two\ndata: second\n\n", events));

  REQUIRE(events.size() == 2);
  CHECK(events[0] == SseEvent{.name = "message", .data = "one"});
  CHECK(events[1] == SseEvent{.name = "two", .data = "second"});
  REQUIRE(parser.finish(events));
  CHECK(events.size() == 2);
  CHECK(parser.buffered_bytes() == 0);
}

TEST_CASE("SSE parser accounts for the implicit terminator at end of input") {
  std::vector<SseEvent> events{};
  SseParser enough{5};
  REQUIRE(enough.push("data", events));
  REQUIRE(events.empty());
  REQUIRE(enough.finish(events));
  REQUIRE(events.size() == 1);
  CHECK(events.front() == SseEvent{.name = "message", .data = ""});
  CHECK(enough.buffered_bytes() == 0);

  SseParser exact{4};
  REQUIRE(exact.push("data", events));

  check_resource_limit(exact.finish(events));
}

TEST_CASE("SSE parser rejects zero and cumulative event limits") {
  std::vector<SseEvent> events{};
  SseParser zero{0};
  check_resource_limit(zero.push("x", events));

  SseParser cumulative{12};
  REQUIRE(cumulative.push("data: a\n", events));
  CHECK(cumulative.buffered_bytes() == 8);
  check_resource_limit(cumulative.push("data: b", events));

  SseParser trailing_carriage_return{1};
  REQUIRE(trailing_carriage_return.push("\r", events));
  REQUIRE(trailing_carriage_return.finish(events));
  CHECK(events.empty());
  CHECK(trailing_carriage_return.buffered_bytes() == 0);
}

// A data line far longer than a transport chunk arrives in many pieces. Each
// push may scan only the bytes it added rather than the whole unfinished line,
// and that shortcut must not change what is parsed, including when the chunk
// boundary falls between a carriage return and its line feed.
TEST_CASE("SSE parser reassembles a long line delivered across many chunks") {
  constexpr std::size_t data_bytes = 256U * 1024U;
  constexpr std::size_t chunk_bytes = 16U * 1024U;
  const std::string payload(data_bytes, 'x');
  const std::string text =
      "event: big\r\ndata: " + payload + "\r\n\r\n" + "data: tail\n\n";
  const std::vector<SseEvent> expected{
      SseEvent{.name = "big", .data = payload},
      SseEvent{.name = "message", .data = "tail"},
  };

  // The payload's CRLF starts at byte 262162. With 16 KiB chunks, phase 19 puts
  // a chunk edge between that carriage return and its line feed, and phase 20
  // puts the edge immediately after the pair.
  for (const std::size_t phase : {std::size_t{0}, std::size_t{19}, std::size_t{20}}) {
    SseParser parser{2U * data_bytes};
    std::vector<SseEvent> result{};
    std::size_t offset = 0;
    if (phase != 0) {
      REQUIRE(parser.push(std::string_view{text}.substr(0, phase), result));
      offset = phase;
    }
    while (offset < text.size()) {
      REQUIRE(parser.push(std::string_view{text}.substr(offset, chunk_bytes), result));
      offset += chunk_bytes;
    }
    REQUIRE(parser.finish(result));
    CHECK(result == expected);
  }
}
