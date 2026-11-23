#include "runtime_test_support.hpp"

#include <stdexcept>
#include <string_view>

using namespace scry::test_support;

TEST_CASE("tool dispatch canonicalizes successful handler results") {
  const scry::detail::ToolSnapshot tools{registered_tool(
      "forecast", [](scrydantic::Json) -> scry::Result<scrydantic::Json> {
        return scrydantic::Json{.text =
                                    R"( { "z": [2, 1], "a": {"y":true,"x":null} } )"};
      })};

  const auto result = scry::detail::dispatch_tool(tools, tool_call(), {}, 1024);

  REQUIRE(result);
  CHECK(result->tool_call_id == "call-1");
  CHECK_FALSE(result->is_error);
  CHECK(result->result.text == R"({"a":{"x":null,"y":true},"z":[2,1]})");
}

TEST_CASE("tool dispatch turns an unknown tool into a model-visible error") {
  const auto result = scry::detail::dispatch_tool({}, tool_call("missing"), {}, 1024);

  REQUIRE(result);
  CHECK(result->tool_call_id == "call-1");
  CHECK(result->is_error);
  CHECK(result->result.text ==
        R"({"error":"unknown tool \"missing\"; there are no tools"})");
}

TEST_CASE(
    "tool dispatch names the registered tools in registration-independent order") {
  const auto stub = [](scrydantic::Json) -> scry::Result<scrydantic::Json> {
    return scrydantic::Json{.text = "{}"};
  };
  const scry::detail::ToolSnapshot tools{
      registered_tool("move", stub),
      registered_tool("eat", stub),
      registered_tool("look", stub),
  };

  const auto result = scry::detail::dispatch_tool(tools, tool_call("jump"), {}, 1024);

  REQUIRE(result);
  CHECK(result->is_error);
  CHECK(result->result.text ==
        R"({"error":"unknown tool \"jump\"; known tools: eat, look, move"})");
}

TEST_CASE("tool dispatch lists a registered name that is not one word as one name") {
  const scry::detail::ToolSnapshot tools{registered_tool(
      "list, delete", [](scrydantic::Json) -> scry::Result<scrydantic::Json> {
        return scrydantic::Json{.text = "{}"};
      })};

  const auto result = scry::detail::dispatch_tool(tools, tool_call("jump"), {}, 1024);

  REQUIRE(result);
  CHECK(result->result.text ==
        R"({"error":"unknown tool \"jump\"; known tools: \"list, delete\""})");
}

TEST_CASE("tool dispatch does not disclose handler-returned Error details") {
  const scry::detail::ToolSnapshot tools{registered_tool(
      "forecast", [](scrydantic::Json) -> scry::Result<scrydantic::Json> {
        return std::unexpected(scry::Error{
            .category = scry::ErrorCategory::tool,
            .message = "secret application message",
            .provider_detail = "secret provider detail",
        });
      })};

  const auto result = scry::detail::dispatch_tool(tools, tool_call(), {}, 1024);

  REQUIRE(result);
  CHECK(result->is_error);
  CHECK(result->result.text == R"({"error":"tool handler returned an error"})");
  CHECK(result->result.text.find("secret") == std::string::npos);
}

TEST_CASE("tool dispatch forwards a handler's model_message unchanged") {
  const scry::detail::ToolSnapshot tools{registered_tool(
      "forecast", [](scrydantic::Json) -> scry::Result<scrydantic::Json> {
        return std::unexpected(scry::tool_error("north, south, east, or west only",
                                                "secret application message"));
      })};

  const auto result = scry::detail::dispatch_tool(tools, tool_call(), {}, 1024);

  REQUIRE(result);
  CHECK(result->is_error);
  CHECK(result->result.text == R"({"error":"north, south, east, or west only"})");
  CHECK(result->result.text.find("secret") == std::string::npos);
}

TEST_CASE("tool dispatch drops an oversized model_message for the fixed diagnostic") {
  const scry::detail::ToolSnapshot tools{registered_tool(
      "forecast", [](scrydantic::Json) -> scry::Result<scrydantic::Json> {
        return std::unexpected(scry::tool_error(std::string(4096, 'x')));
      })};

  const auto result = scry::detail::dispatch_tool(tools, tool_call(), {}, 1024);

  REQUIRE(result);
  CHECK(result->is_error);
  CHECK(result->result.text == R"({"error":"tool execution failed"})");
}

TEST_CASE("tool dispatch contains standard and non-standard handler exceptions") {
  const auto check_exception = [](scry::ToolHandler handler) {
    const scry::detail::ToolSnapshot tools{
        registered_tool("forecast", std::move(handler))};
    const auto result = scry::detail::dispatch_tool(tools, tool_call(), {}, 1024);
    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(result->result.text == R"({"error":"tool handler returned an error"})");
  };

  SECTION("standard exception") {
    check_exception([](scrydantic::Json) -> scry::Result<scrydantic::Json> {
      throw std::runtime_error{"secret exception message"};
    });
  }
  SECTION("non-standard exception") {
    check_exception(
        [](scrydantic::Json) -> scry::Result<scrydantic::Json> { throw 7; });
  }
}

TEST_CASE("tool dispatch turns invalid handler JSON into a bounded tool error") {
  const scry::detail::ToolSnapshot tools{registered_tool(
      "forecast", [](scrydantic::Json) -> scry::Result<scrydantic::Json> {
        return scrydantic::Json{.text = "{"};
      })};

  const auto result = scry::detail::dispatch_tool(tools, tool_call(), {}, 1024);

  REQUIRE(result);
  CHECK(result->is_error);
  CHECK(result->result.text == R"({"error":"tool handler returned invalid JSON"})");
}

TEST_CASE("tool dispatch enforces the canonical result byte limit exactly") {
  constexpr std::string_view canonical = R"({"a":1})";
  const scry::detail::ToolSnapshot tools{registered_tool(
      "forecast", [](scrydantic::Json) -> scry::Result<scrydantic::Json> {
        return scrydantic::Json{.text = R"({"a":1})"};
      })};

  const auto exact =
      scry::detail::dispatch_tool(tools, tool_call(), {}, canonical.size());
  REQUIRE(exact);
  CHECK(exact->result.text == canonical);

  const auto over =
      scry::detail::dispatch_tool(tools, tool_call(), {}, canonical.size() - 1);
  REQUIRE_FALSE(over);
  CHECK(over.error().category == scry::ErrorCategory::resource_limit);
}

TEST_CASE("tool dispatch fails when even its generic error exceeds the limit") {
  constexpr std::string_view generic_error = R"({"error":"tool execution failed"})";

  const auto result = scry::detail::dispatch_tool({}, tool_call("missing"), {},
                                                  generic_error.size() - 1);

  REQUIRE_FALSE(result);
  CHECK(result.error().category == scry::ErrorCategory::resource_limit);
}

TEST_CASE("a plain handler is adapted without disturbing its arguments") {
  scrydantic::Json seen{};
  const scry::detail::ToolSnapshot tools{
      registered_tool("forecast", [&seen](scrydantic::Json arguments) {
        seen = arguments;
        return scry::Result<scrydantic::Json>{std::move(arguments)};
      })};

  const auto result = scry::detail::dispatch_tool(tools, tool_call(), {}, 1024);

  REQUIRE(result);
  CHECK(seen.text == R"({"z":2,"a":1})");
  CHECK(result->result.text == R"({"a":1,"z":2})");
}
