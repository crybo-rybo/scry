#include "runtime/tool_dispatch.hpp"
#include "runtime/tool_registry_impl.hpp"
#include "support/harness_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <scry/config.hpp>
#include <scry/harness.hpp>
#include <scry/response_format.hpp>
#include <scrydantic/codec.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What Scry does with a reflected tool or a typed answer: how a Scrydantic
// handler lowers into the registry, how its results and failures reach the model
// and the host, and which category Scry gives its errors. The codec itself, its
// schemas, and its decode rules are tested in Scrydantic's repository.
namespace {

struct PresenceArguments {
  [[= scrydantic::description{"C++ default"}]] std::string defaulted{"fallback"};
  std::optional<std::int16_t> nullable;
  std::optional<std::int16_t> optional_nullable = std::nullopt;
  [[= scrydantic::description{"Annotation text"}]] std::string required;
};

struct NestedResult {
  std::string label{};
};

struct RatioResult {
  double ratio{};
};

struct DirectHandler {
  NestedResult operator()(PresenceArguments) const { return {}; }
};

// A const-qualified value is supported, so a Result of one is too, just as a
// handler returning a const value directly is.
struct ConstExpectedHandler {
  scrydantic::Result<const std::int32_t> operator()(PresenceArguments) const {
    return 7;
  }
};

// A reflected handler in the registry's shape, as ToolRegistry::add<Args>() builds
// it.
template <typename Args, typename Handler>
[[nodiscard]] scry::ContextualToolHandler make_tool_handler(Handler&& handler) {
  return scry::detail::reflected_handler(
      scrydantic::bind_handler<Args, scry::ToolCallContext>(
          std::forward<Handler>(handler)));
}

// A dispatchable snapshot holding one reflected handler under the given name.
[[nodiscard]] scry::detail::ToolSnapshot
snapshot_of(std::string name, scry::ContextualToolHandler handler) {
  return {
      std::make_shared<const scry::detail::RegisteredTool>(scry::detail::RegisteredTool{
          .definition =
              {
                  .name = std::move(name),
                  .description = "Reflected test tool",
                  .input_schema = {.text = "{}"},
              },
          .handler = std::move(handler),
      })};
}

} // namespace

TEST_CASE("a reflected tool's decode failure keeps Scrydantic's texts and issues") {
  auto handler = make_tool_handler<PresenceArguments>([](PresenceArguments) {});
  auto missing = handler({}, scrydantic::Json{.text = R"({"required":"Detroit"})"});
  REQUIRE_FALSE(missing);
  // Scrydantic words the failure; Scry files it under its tool category and
  // carries the issues to the host.
  CHECK(missing.error().category == scry::ErrorCategory::tool);
  CHECK(missing.error().model_message == "$.nullable is a required member");
  REQUIRE(missing.error().issues.size() == 1);
  CHECK(missing.error().issues[0].kind == scrydantic::IssueKind::missing_member);
  CHECK(scrydantic::json_path(missing.error().issues[0].path) == "$.nullable");
}

TEST_CASE("reflected result-encoding failures reach the model as the fixed text") {
  auto handler = make_tool_handler<PresenceArguments>([](PresenceArguments) {
    return RatioResult{.ratio = std::numeric_limits<double>::infinity()};
  });
  const auto tools = snapshot_of("private_calculation", std::move(handler));

  const auto result = scry::detail::dispatch_tool(
      tools,
      scry::detail::ToolCallBlock{
          .id = "call-1",
          .name = "private_calculation",
          .arguments = {.text = R"({"nullable":null,"required":"value"})"},
      },
      {}, 1024);

  REQUIRE(result);
  CHECK(result->is_error);
  CHECK(result->result.text == R"({"error":"tool handler returned an error"})");
  CHECK(result->result.text.find("ratio") == std::string::npos);
}

TEST_CASE("reflected erased handlers encode a Result of a const-qualified value") {
  // Instantiating the handler is the check: the const value type has to be
  // stripped before it reaches encode_value, or this fails to compile inside the
  // library.
  auto handler = make_tool_handler<PresenceArguments>(ConstExpectedHandler{});

  const auto result =
      handler({}, scrydantic::Json{.text = R"({"nullable":null,"required":"ok"})"});

  REQUIRE(result);
  CHECK(result->text == "7");
}

TEST_CASE("reflected erased handlers retain move-only captures and typed errors") {
  auto handler = make_tool_handler<PresenceArguments>(
      [owned = std::make_unique<std::string>("handled")](
          PresenceArguments arguments) -> scrydantic::Result<NestedResult> {
        if (arguments.required == "reject") {
          return std::unexpected(scrydantic::Error{
              .message = "application rejected arguments",
          });
        }
        return NestedResult{.label = *owned + ":" + arguments.required};
      });

  auto result =
      handler({}, scrydantic::Json{.text = R"({"nullable":null,"required":"ok"})"});
  REQUIRE(result);
  CHECK(result->text == R"({"label":"handled:ok"})");

  result =
      handler({}, scrydantic::Json{.text = R"({"nullable":null,"required":"reject"})"});
  REQUIRE_FALSE(result);
  CHECK(result.error().message == "application rejected arguments");

  result = handler({}, scrydantic::Json{.text = R"({"required":"missing nullable"})"});
  REQUIRE_FALSE(result);
  CHECK(result.error().category == scry::ErrorCategory::tool);
}

TEST_CASE("tool manifests include reflected and explicit contracts together") {
  auto harness = scry::Harness::create(scry::test_support::test_config());
  REQUIRE(harness);
  REQUIRE(harness->tools().add<PresenceArguments>(
      {.name = "presence", .description = "Reflected arguments"}, DirectHandler{}));
  REQUIRE(harness->tools().add_dynamic(
      {.name = "explicit",
       .description = "Explicit arguments",
       .input_schema = {.text = R"({"type":"object"})"}},
      [](scrydantic::Json input) -> scry::Result<scrydantic::Json> { return input; }));

  const auto manifest = harness->tools().to_json();
  REQUIRE(manifest);
  CHECK(
      manifest->text ==
      std::string{R"({"tools":[{"description":"Reflected arguments","input_schema":)"} +
          std::string{scrydantic::input_schema_v<PresenceArguments>} +
          R"(,"name":"presence"},{"description":"Explicit arguments","input_schema":{"type":"object"},"name":"explicit"}],"version":1})");
}

TEST_CASE("reflected handlers may take the call context as a leading parameter") {
  auto created = scry::Harness::create(scry::test_support::test_config());
  REQUIRE(created);
  auto harness = std::move(*created);

  std::string observed_call_id;
  std::string observed_tool_name;
  REQUIRE(harness.tools().add<PresenceArguments>(
      {
          .name = "contextual",
          .description = "Record the call it was invoked for",
      },
      [&](const scry::ToolCallContext& context, PresenceArguments arguments) {
        // The views borrow from the live call block, so the test owns copies.
        observed_call_id = std::string{context.call_id};
        observed_tool_name = std::string{context.tool_name};
        return NestedResult{.label = std::move(arguments.required)};
      }));
  REQUIRE(harness.tools().add<PresenceArguments>(
      {
          .name = "plain",
          .description = "Ignore the call",
      },
      DirectHandler{}));
  CHECK(harness.tools().size() == 2);

  const auto snapshot =
      scry::detail::ToolRegistryAccess::snapshot(harness.tools()).entries;
  REQUIRE(snapshot);
  const auto arguments =
      scrydantic::Json{.text = R"({"nullable":null,"required":"value"})"};

  const auto contextual = scry::detail::dispatch_tool(
      *snapshot,
      scry::detail::ToolCallBlock{
          .id = "call-9", .name = "contextual", .arguments = arguments},
      scry::ToolCallContext{
          .turn_id = scry::TurnId{.value = 3},
          .call_id = "call-9",
          .tool_name = "contextual",
          .round = 1,
          .index = 0,
      },
      1024);
  REQUIRE(contextual);
  CHECK(contextual->result.text == R"({"label":"value"})");
  CHECK(observed_call_id == "call-9");
  CHECK(observed_tool_name == "contextual");

  const auto plain = scry::detail::dispatch_tool(
      *snapshot,
      scry::detail::ToolCallBlock{
          .id = "call-10", .name = "plain", .arguments = arguments},
      {}, 1024);
  REQUIRE(plain);
  CHECK(plain->result.text == R"({"label":""})");
}

TEST_CASE("a dynamic tool forwards a Scrydantic decode failure with tool_error") {
  // A dynamic handler returns scry::Result, and Scrydantic's errors have no
  // category. scry::tool_error() files one as a tool error and keeps its texts
  // and issues, so the model and the host get what a reflected tool gives them.
  const auto tools = snapshot_of(
      "presence",
      [](const scry::ToolCallContext&,
         const scrydantic::Json& arguments) -> scry::Result<scrydantic::Json> {
        auto decoded = scrydantic::decode<PresenceArguments>(arguments);
        if (!decoded) {
          return std::unexpected(scry::tool_error(std::move(decoded.error())));
        }
        return scrydantic::Json{.text = "{}"};
      });

  std::optional<scry::Error> handler_error;
  const auto result =
      scry::detail::dispatch_tool(tools,
                                  scry::detail::ToolCallBlock{
                                      .id = "call-1",
                                      .name = "presence",
                                      .arguments = {.text = R"({"required":7})"},
                                  },
                                  {}, 1024, &handler_error);

  REQUIRE(result);
  CHECK(result->is_error);
  CHECK(result->result.text ==
        R"({"error":"$.nullable is a required member\n$.required must be a string"})");
  REQUIRE(handler_error);
  CHECK(handler_error->category == scry::ErrorCategory::tool);
  CHECK(handler_error->issues.size() == 2);
}

TEST_CASE("response_format validators report a rejected answer as invalid_argument") {
  // Scrydantic's errors carry no category; Scry files a rejected typed answer
  // under invalid_argument, with the decoder's host and model texts.
  auto format = scry::response_format<PresenceArguments>();
  REQUIRE(format.validate);
  CHECK(format.schema.text == scrydantic::input_schema_v<PresenceArguments>);
  CHECK(format.validate(
      scrydantic::Json{.text = R"({"nullable":null,"required":"ok"})"}));

  const auto missing =
      format.validate(scrydantic::Json{.text = R"({"required":"ok"})"});
  REQUIRE_FALSE(missing);
  CHECK(missing.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(missing.error().message == "$.nullable is a required member");
  CHECK(missing.error().model_message == "$.nullable is a required member");

  const auto malformed = format.validate(scrydantic::Json{.text = "{"});
  REQUIRE_FALSE(malformed);
  CHECK(malformed.error().category == scry::ErrorCategory::invalid_argument);
  constexpr std::string_view text =
      "JSON text is not valid JSON: unexpected end of input at line 1, column 2";
  CHECK(malformed.error().message == text);
  CHECK(malformed.error().model_message == text);
}
