#include "tool_dispatch_test_support.hpp"

#include <stdexcept>
#include <string_view>

using namespace scry::test_support;

TEST_CASE("tool dispatch canonicalizes successful handler results") {
  const scry::detail::ToolSnapshot tools{
      registered_tool("forecast", [](scry::Json) -> scry::Result<scry::Json> {
        return scry::Json{.text = R"( { "z": [2, 1], "a": {"y":true,"x":null} } )"};
      })};

  const auto result = scry::detail::dispatch_tool(tools, tool_call(), 1024);

  REQUIRE(result);
  CHECK(result->tool_call_id == "call-1");
  CHECK_FALSE(result->is_error);
  CHECK(result->result.text == R"({"a":{"x":null,"y":true},"z":[2,1]})");
}

TEST_CASE("tool dispatch turns an unknown tool into a model-visible error") {
  const auto result = scry::detail::dispatch_tool({}, tool_call("missing"), 1024);

  REQUIRE(result);
  CHECK(result->tool_call_id == "call-1");
  CHECK(result->is_error);
  CHECK(result->result.text == R"({"error":"model requested an unknown tool"})");
}

TEST_CASE("tool dispatch treats unavailable handlers as model-visible errors") {
  const auto check_unavailable = [](scry::detail::ToolRegistrationPtr tool) {
    const auto result =
        scry::detail::dispatch_tool({std::move(tool)}, tool_call(), 1024);
    REQUIRE(result);
    CHECK(result->tool_call_id == "call-1");
    CHECK(result->is_error);
    CHECK(result->result.text == R"({"error":"tool handler is unavailable"})");
  };

  SECTION("missing handler storage") {
    check_unavailable(std::make_shared<const scry::detail::RegisteredTool>(
        scry::detail::RegisteredTool{
            .definition = tool_definition("forecast"),
            .handler = nullptr,
        }));
  }
  SECTION("empty type-erased handler") {
    check_unavailable(std::make_shared<const scry::detail::RegisteredTool>(
        scry::detail::RegisteredTool{
            .definition = tool_definition("forecast"),
            .handler = std::make_shared<scry::ToolHandler>(),
        }));
  }
}

TEST_CASE("tool dispatch does not disclose handler-returned Error details") {
  const scry::detail::ToolSnapshot tools{
      registered_tool("forecast", [](scry::Json) -> scry::Result<scry::Json> {
        return std::unexpected(scry::Error{
            .category = scry::ErrorCategory::tool,
            .message = "secret application message",
            .provider_detail = "secret provider detail",
        });
      })};

  const auto result = scry::detail::dispatch_tool(tools, tool_call(), 1024);

  REQUIRE(result);
  CHECK(result->is_error);
  CHECK(result->result.text == R"({"error":"tool handler returned an error"})");
  CHECK(result->result.text.find("secret") == std::string::npos);
}

TEST_CASE("tool dispatch contains standard and non-standard handler exceptions") {
  const auto check_exception = [](scry::ToolHandler handler) {
    const scry::detail::ToolSnapshot tools{
        registered_tool("forecast", std::move(handler))};
    const auto result = scry::detail::dispatch_tool(tools, tool_call(), 1024);
    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(result->result.text == R"({"error":"tool handler returned an error"})");
  };

  SECTION("standard exception") {
    check_exception([](scry::Json) -> scry::Result<scry::Json> {
      throw std::runtime_error{"secret exception message"};
    });
  }
  SECTION("non-standard exception") {
    check_exception([](scry::Json) -> scry::Result<scry::Json> { throw 7; });
  }
}

TEST_CASE("tool dispatch turns invalid handler JSON into a bounded tool error") {
  const scry::detail::ToolSnapshot tools{
      registered_tool("forecast", [](scry::Json) -> scry::Result<scry::Json> {
        return scry::Json{.text = "{"};
      })};

  const auto result = scry::detail::dispatch_tool(tools, tool_call(), 1024);

  REQUIRE(result);
  CHECK(result->is_error);
  CHECK(result->result.text == R"({"error":"tool handler returned invalid JSON"})");
}

TEST_CASE("tool dispatch enforces the canonical result byte limit exactly") {
  constexpr std::string_view canonical = R"({"a":1})";
  const scry::detail::ToolSnapshot tools{
      registered_tool("forecast", [](scry::Json) -> scry::Result<scry::Json> {
        return scry::Json{.text = R"({"a":1})"};
      })};

  const auto exact = scry::detail::dispatch_tool(tools, tool_call(), canonical.size());
  REQUIRE(exact);
  CHECK(exact->result.text == canonical);

  const auto over =
      scry::detail::dispatch_tool(tools, tool_call(), canonical.size() - 1);
  REQUIRE_FALSE(over);
  CHECK(over.error().category == scry::ErrorCategory::resource_limit);
}

TEST_CASE("tool dispatch fails when even its generic error exceeds the limit") {
  constexpr std::string_view generic_error = R"({"error":"tool execution failed"})";

  const auto result =
      scry::detail::dispatch_tool({}, tool_call("missing"), generic_error.size() - 1);

  REQUIRE_FALSE(result);
  CHECK(result.error().category == scry::ErrorCategory::resource_limit);
}
