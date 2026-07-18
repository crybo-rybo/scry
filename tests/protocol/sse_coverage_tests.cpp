#include "protocol/sse.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <scry/error.hpp>
#include <string_view>
#include <vector>

namespace {

using scry::detail::SseEvent;
using scry::detail::SseParser;

void check_resource_limit(const scry::Result<std::vector<SseEvent>>& result) {
  REQUIRE_FALSE(result);
  CHECK(result.error().category == scry::ErrorCategory::resource_limit);
  CHECK(result.error().message == "SSE event exceeds the configured byte limit");
}

} // namespace

TEST_CASE("SSE parser handles CRLF and standalone carriage returns across chunks") {
  SECTION("split CRLF") {
    SseParser parser{128};
    REQUIRE(parser.push("event: split\r")->empty());
    REQUIRE(parser.push("\ndata: value\r")->empty());
    REQUIRE(parser.push("\n\r")->empty());

    const auto events = parser.push("\n");

    REQUIRE(events);
    REQUIRE(events->size() == 1);
    CHECK(events->front() == SseEvent{.name = "split", .data = "value"});
    CHECK(parser.buffered_bytes() == 0);
  }

  SECTION("standalone carriage return") {
    SseParser parser{128};
    const auto pending = parser.push("event: lone\rdata: first\rdata: second\r\r");
    REQUIRE(pending);
    CHECK(pending->empty());

    const auto events = parser.finish();
    REQUIRE(events);
    REQUIRE(events->size() == 1);
    CHECK(events->front() == SseEvent{.name = "lone", .data = "first\nsecond"});
    CHECK(parser.buffered_bytes() == 0);
  }
}

TEST_CASE("SSE parser applies field syntax without carrying empty events forward") {
  SseParser parser{256};
  const auto events = parser.push("event: stale\n"
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
                                  "\n");

  REQUIRE(events);
  REQUIRE(events->size() == 2);
  CHECK((*events)[0] == SseEvent{.name = "named", .data = "\n second"});
  CHECK((*events)[1] == SseEvent{.name = "message", .data = "value"});
  CHECK(parser.buffered_bytes() == 0);
}

TEST_CASE("SSE parser dispatches multiple events and safely finishes empty state") {
  SseParser parser{128};
  REQUIRE(parser.push({})->empty());

  const auto events = parser.push("data: one\n\nevent: two\ndata: second\n\n");

  REQUIRE(events);
  REQUIRE(events->size() == 2);
  CHECK((*events)[0] == SseEvent{.name = "message", .data = "one"});
  CHECK((*events)[1] == SseEvent{.name = "two", .data = "second"});
  const auto finished = parser.finish();
  REQUIRE(finished);
  CHECK(finished->empty());
  CHECK(parser.buffered_bytes() == 0);
}

TEST_CASE("SSE parser accounts for the implicit terminator at end of input") {
  SseParser exact{4};
  REQUIRE(exact.push("data")->empty());
  CHECK(exact.buffered_bytes() == 4);

  const auto rejected_finish = exact.finish();

  check_resource_limit(rejected_finish);
}

TEST_CASE("SSE parser rejects zero and cumulative event limits") {
  SseParser zero{0};
  check_resource_limit(zero.push("x"));

  SseParser cumulative{12};
  REQUIRE(cumulative.push("data: a\n")->empty());
  CHECK(cumulative.buffered_bytes() == 8);
  check_resource_limit(cumulative.push("data: b"));

  SseParser trailing_carriage_return{1};
  REQUIRE(trailing_carriage_return.push("\r")->empty());
  const auto finished = trailing_carriage_return.finish();
  REQUIRE(finished);
  CHECK(finished->empty());
  CHECK(trailing_carriage_return.buffered_bytes() == 0);
}
