// The documented decisions of Scry's JSON layer (kernel/json/document.hpp), one
// case each. The golden fixtures cover the same ground by the thousand; these
// name the rules.

#include "kernel/json/document.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace json = scry::detail::json;
using scry::JsonKind;

[[nodiscard]] json::Value parse_ok(const std::string_view text) {
  auto parsed = json::parse(text);
  REQUIRE(parsed);
  REQUIRE(json::validate(text));
  return std::move(*parsed);
}

[[nodiscard]] bool rejected(const std::string_view text) {
  const bool parsed = json::parse(text).has_value();
  CHECK(json::validate(text) == parsed);
  return !parsed;
}

[[nodiscard]] std::string canonical(const std::string_view text) {
  return json::write(parse_ok(text));
}

} // namespace

TEST_CASE("json values report their kind and nothing else") {
  const auto document =
      parse_ok(R"({"a":null,"b":true,"c":-3,"d":3,"e":1.5,"f":"x","g":[1],"h":{}})");
  REQUIRE(document.kind() == JsonKind::object);
  CHECK(document.find("a")->is_null());
  CHECK(document.find("b")->boolean() == true);
  CHECK(document.find("c")->signed_integer() == -3);
  CHECK(document.find("c")->unsigned_integer() == std::nullopt);
  CHECK(document.find("d")->unsigned_integer() == 3U);
  CHECK(document.find("d")->signed_integer() == std::nullopt);
  CHECK(document.find("d")->number() == 3.0);
  CHECK(document.find("e")->kind() == JsonKind::number);
  CHECK(document.find("e")->number() == 1.5);
  CHECK(document.find("f")->string() == "x");
  CHECK(document.find("f")->number() == std::nullopt);
  CHECK(document.find("g")->array()->size() == 1);
  CHECK(document.find("g")->size() == 1);
  CHECK(document.find("h")->object()->empty());
  CHECK(document.find("missing") == nullptr);
  CHECK(document.find("g")->find("a") == nullptr);
  CHECK(json::Value{}.is_null());
  CHECK(json::Value{}.size() == 0);
}

TEST_CASE("json objects sort keys by byte and keep the last duplicate") {
  const auto document = parse_ok(
      "{\"b\":1,\"a\":2,\"\xc3\xa9\":3,\"B\":4,\"a\":5,\"o\":{\"k\":1,\"k\":2}}");
  const auto& members = *document.object();
  REQUIRE(members.size() == 5);
  CHECK(members[0].first == "B");
  CHECK(members[1].first == "a");
  CHECK(members[2].first == "b");
  CHECK(members[3].first == "o");
  CHECK(members[4].first == "\xc3\xa9");
  CHECK(document.find("a")->unsigned_integer() == 5U);
  CHECK(document.find("o")->find("k")->unsigned_integer() == 2U);
  CHECK(json::write(document) ==
        "{\"B\":4,\"a\":5,\"b\":1,\"o\":{\"k\":2},\"\xc3\xa9\":3}");
  // An escaped key is the same key as its plain spelling.
  CHECK(canonical("{\"A\":1,\"\\u0041\":2}") == R"({"A":2})");
}

TEST_CASE("json integers are plain digit strings that fit their kind") {
  CHECK(parse_ok("18446744073709551615").unsigned_integer() ==
        std::numeric_limits<std::uint64_t>::max());
  CHECK(parse_ok("-9223372036854775808").signed_integer() ==
        std::numeric_limits<std::int64_t>::min());
  CHECK(parse_ok("18446744073709551616").kind() == JsonKind::number);
  CHECK(canonical("18446744073709551616") == "1.8446744073709552E19");
  CHECK(parse_ok("-9223372036854775809").kind() == JsonKind::number);
  CHECK(parse_ok("0").unsigned_integer() == 0U);
}

TEST_CASE("json numbers with a fraction or exponent are doubles even when whole") {
  for (const auto text : {"1.0", "1e2", "1E+2", "0e5", "1.0e19"}) {
    INFO(text);
    CHECK(parse_ok(text).kind() == JsonKind::number);
  }
  // A whole double is spelled without a fraction.
  CHECK(canonical("1.0") == "1");
  CHECK(canonical("1E+2") == "100");
}

TEST_CASE("json negative zero is the double -0.0 and keeps its sign") {
  for (const auto text : {"-0", "-0.0", "-0e5", "-0.0E-3"}) {
    INFO(text);
    const auto zero = parse_ok(text);
    CHECK(zero.kind() == JsonKind::number);
    CHECK(std::signbit(*zero.number()));
    CHECK(json::write(zero) == "-0");
  }
  CHECK(canonical("0.0") == "0");
}

TEST_CASE("json doubles take the canonical spelling") {
  CHECK(canonical("0.1") == "0.1");
  CHECK(canonical("0.30000000000000004") == "0.30000000000000004");
  CHECK(canonical("0.0001") == "0.0001");
  CHECK(canonical("0.00001") == "1E-5");
  CHECK(canonical("1.5e15") == "1500000000000000");
  CHECK(canonical("1.5e16") == "1.5E16");
  CHECK(canonical("-2.5E-3") == "-0.0025");
  CHECK(canonical("123e-10") == "1.23E-8");
  CHECK(canonical("1.7976931348623157e308") == "1.7976931348623157E308");
  CHECK(canonical("5e-324") == "5E-324");
  CHECK(canonical("1.00000000000000011102230246251565404236316680908203125") == "1");
  CHECK(canonical("1.00000000000000011102230246251565404236316680908203126") ==
        "1.0000000000000002");
}

TEST_CASE("json numbers a double cannot hold are rejected") {
  CHECK(rejected("1e400"));
  CHECK(rejected("-1e400"));
  CHECK(rejected("1.7976931348623159e308"));
  CHECK(rejected("1e-400"));
  CHECK(rejected("2.4703282292062327e-324"));
  CHECK(canonical("2.4703282292062328e-324") == "5E-324");
  CHECK(canonical("0e99999999999999999999") == "0");
  CHECK(rejected(std::string(400, '9')));
}

TEST_CASE("json number grammar is RFC 8259's") {
  for (const auto text :
       {"01", "-01", "+1", "-", "1.", ".5", "1e", "1e+", "0x10", "NaN", "Infinity",
        "-Infinity", "--1", "1.5.", "1e5e5", "123abc"}) {
    INFO(text);
    CHECK(rejected(text));
  }
}

TEST_CASE("json strings decode escapes and surrogate pairs") {
  CHECK(parse_ok(R"("\"\\\/\b\f\n\r\t")").string() == "\"\\/\b\f\n\r\t");
  CHECK(parse_ok("\"\\u0041\\u00e9\\u20AC\"").string() == "A\xc3\xa9\xe2\x82\xac");
  CHECK(parse_ok("\"\\ud83d\\ude00\"").string() == "\xf0\x9f\x98\x80");
  CHECK(parse_ok("\"\\u0000\"").string() == std::string_view{"\0", 1});
  CHECK(rejected("\"\\ud83d\""));
  CHECK(rejected("\"\\ude00\""));
  CHECK(rejected("\"\\udc00\\ud800\""));
  CHECK(rejected("\"\\ud83d\\u0041\""));
  CHECK(rejected("\"\\ud83dx\""));
  CHECK(rejected("\"\\u00G1\""));
  CHECK(rejected(R"("\a")"));
  CHECK(rejected(R"("\x41")"));
}

TEST_CASE("json strings hold strictly valid UTF-8 and no raw control characters") {
  CHECK(parse_ok("\"\xf4\x8f\xbf\xbf\xed\x9f\xbf\x7f\"").string() ==
        "\xf4\x8f\xbf\xbf\xed\x9f\xbf\x7f");
  for (const auto text :
       {"\"\xc0\x80\"", "\"\xc1\xbf\"", "\"\xe0\x80\x80\"", "\"\xf0\x8f\xbf\xbf\"",
        "\"\xed\xa0\x80\"", "\"\xf4\x90\x80\x80\"", "\"\xf5\x80\x80\x80\"", "\"\xff\"",
        "\"\x80\"", "\"\xc3\"", "\"\xe2\x82\"", "{\"\xff\":1}", "\"\x01\"", "\"\t\""}) {
    INFO(text);
    CHECK(rejected(text));
  }
  CHECK(rejected(std::string_view{"\"a\0b\"", 5}));
}

TEST_CASE("json documents are exactly one value") {
  for (const auto text :
       {"", " ", " \t\r\n", "{} {}", "1 2", "[1] x", "{\"a\":1,}", "[1,]", "[,1]",
        "{a:1}", "\xef\xbb\xbf{}", "\x0c{}", "{} // c", "[1}"}) {
    INFO(text);
    CHECK(rejected(text));
  }
  CHECK(rejected(std::string_view{"{}\0", 3}));
  CHECK(canonical(" \t\r\n{ \"a\" : [ 1 , 2 ] }\n") == R"({"a":[1,2]})");
}

TEST_CASE("json nesting stops at max_depth") {
  const auto nest = [](const std::size_t depth) {
    return std::string(depth, '[') + std::string(depth, ']');
  };
  CHECK(json::max_depth == 256);
  CHECK(json::parse(nest(256)));
  CHECK(rejected(nest(257)));
  CHECK(rejected(std::string(256, '[') + "{}" + std::string(256, ']')));
  CHECK(rejected(std::string(100000, '[')));
}

TEST_CASE("json quoting escapes only what JSON requires") {
  CHECK(json::quote(std::string_view{"q\" b\\ /\b\f\n\r\t\x01\x1f\x7f\0", 16}) ==
        std::string{"\"q\\\" b\\\\ /\\b\\f\\n\\r\\t\\u0001\\u001F\x7f\\u0000\""});
  CHECK(json::quote("caf\xc3\xa9") == "\"caf\xc3\xa9\"");
  CHECK(json::quote("") == R"("")");
  std::string appended{"x"};
  json::append_quoted(appended, "y");
  CHECK(appended == R"(x"y")");
  CHECK(canonical("\"\\u001f\\u007f\\/\"") == "\"\\u001F\x7f/\"");
}
