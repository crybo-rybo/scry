#include "runtime/tool_dispatch.hpp"
#include "runtime/tool_registry_impl.hpp"
#include "support/harness_test_support.hpp"

#include <array>
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

enum class TemperatureUnit {
  celsius,
  fahrenheit,
};

struct PresenceArguments {
  [[= scrydantic::description{"C++ default"}]] std::string defaulted{"fallback"};
  std::optional<std::int16_t> nullable;
  std::optional<std::int16_t> optional_nullable = std::nullopt;
  [[= scrydantic::description{"Annotation text"}]] std::string required;
};

struct NestedResult {
  std::string label{};
};

struct CopyOnlyResult {
  static inline int destructions = 0;
  std::vector<int> values;

  // Suppress the implicit move constructor while retaining aggregate support.
  ~CopyOnlyResult() { ++destructions; }
};

struct AllTypesArguments {
  std::array<std::int32_t, 2> fixed{};
  bool flag{};
  NestedResult nested{};
  double ratio{};
  TemperatureUnit unit{TemperatureUnit::celsius};
  std::vector<std::int32_t> values{};
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

struct ContextHandler {
  NestedResult operator()(const scry::ToolCallContext&, PresenceArguments) const {
    return {};
  }
};

// The context is a leading parameter, never a trailing one: the wrong order has
// to stay a compile error rather than silently binding.
struct ReversedContextHandler {
  NestedResult operator()(PresenceArguments, const scry::ToolCallContext&) const {
    return {};
  }
};

struct VoidContextHandler {
  void operator()(const scry::ToolCallContext&, PresenceArguments) const {}
};

// Refuses a call whose required text is "reject" and acknowledges any other.
struct StatusHandler {
  scrydantic::Status operator()(PresenceArguments arguments) const {
    if (arguments.required == "reject") {
      return std::unexpected(scrydantic::tool_error("rejected by the handler"));
    }
    return {};
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

// Scry's registry binds handlers with its call context.
static_assert(scrydantic::ToolHandlerFor<ContextHandler, PresenceArguments,
                                         scry::ToolCallContext>);
static_assert(!scrydantic::ToolHandlerFor<ReversedContextHandler, PresenceArguments,
                                          scry::ToolCallContext>);
static_assert(scrydantic::ToolHandlerFor<VoidContextHandler, PresenceArguments,
                                         scry::ToolCallContext>);

TEST_CASE("a reflected tool answers arguments that are not JSON with a fixed text") {
  auto handler = make_tool_handler<PresenceArguments>([](PresenceArguments) {});
  auto malformed = handler({}, scrydantic::Json{.text = "{"});
  REQUIRE_FALSE(malformed);
  CHECK(malformed.error().category == scry::ErrorCategory::tool);
  CHECK(malformed.error().message == "tool arguments are not valid JSON");
  CHECK(malformed.error().model_message == "tool arguments are not valid JSON");
}

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
    return AllTypesArguments{
        .fixed = {1, 2},
        .flag = true,
        .ratio = std::numeric_limits<double>::infinity(),
        .unit = TemperatureUnit::celsius,
    };
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

TEST_CASE("a reflected result that is not valid UTF-8 reaches the model as the fixed "
          "text") {
  auto handler = make_tool_handler<PresenceArguments>(
      [](PresenceArguments) { return std::string(1, '\xff'); });
  const auto tools = snapshot_of("read_bytes", std::move(handler));

  const auto result = scry::detail::dispatch_tool(
      tools,
      scry::detail::ToolCallBlock{
          .id = "call-1",
          .name = "read_bytes",
          .arguments = {.text = R"({"nullable":null,"required":"value"})"},
      },
      {}, 1024);

  REQUIRE(result);
  CHECK(result->is_error);
  CHECK(result->result.text == R"({"error":"tool handler returned an error"})");
}

TEST_CASE("public encoding matches reflected tool dispatch output") {
  const auto value = AllTypesArguments{
      .fixed = {1, 2},
      .flag = true,
      .nested = {.label = "same"},
      .ratio = 1e20,
      .unit = TemperatureUnit::fahrenheit,
      .values = {3, 4},
  };
  auto handler = make_tool_handler<PresenceArguments>(
      [value](PresenceArguments) { return value; });
  const auto tools = snapshot_of("snapshot", std::move(handler));

  const auto direct = scrydantic::encode(value);
  const auto through_dispatch = scry::detail::dispatch_tool(
      tools,
      scry::detail::ToolCallBlock{
          .id = "call-1",
          .name = "snapshot",
          .arguments =
              {
                  .text = R"({"nullable":null,"required":"value"})",
              },
      },
      {}, 1024);

  REQUIRE(direct);
  REQUIRE(through_dispatch);
  CHECK(
      direct->text ==
      R"({"fixed":[1,2],"flag":true,"nested":{"label":"same"},"ratio":1E20,"unit":"fahrenheit","values":[3,4]})");
  CHECK(direct->text == through_dispatch->result.text);
}

TEST_CASE("reflected erased handlers encode copy-only results without extra copies") {
  CopyOnlyResult::destructions = 0;
  const auto run_handler = [](auto body) {
    auto handler = make_tool_handler<PresenceArguments>(std::move(body));
    return handler({},
                   scrydantic::Json{.text = R"({"nullable":null,"required":"ok"})"});
  };

  scry::Result<scrydantic::Json> result{};
  SECTION("bare result") {
    result = run_handler(
        [](PresenceArguments) { return CopyOnlyResult{.values = {1, 2, 3}}; });
  }
  SECTION("expected result") {
    result = run_handler([](PresenceArguments) -> scrydantic::Result<CopyOnlyResult> {
      return scrydantic::Result<CopyOnlyResult>{std::in_place,
                                                std::vector<int>{1, 2, 3}};
    });
  }

  REQUIRE(result);
  CHECK(result->text == R"({"values":[1,2,3]})");
  CHECK(CopyOnlyResult::destructions == 1);
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

TEST_CASE("reflected handlers returning void or Status acknowledge with an object") {
  const auto arguments =
      scrydantic::Json{.text = R"({"nullable":null,"required":"ok"})"};
  int calls = 0;
  auto acknowledging =
      make_tool_handler<PresenceArguments>([&calls](PresenceArguments) { ++calls; });
  auto result = acknowledging({}, arguments);
  REQUIRE(result);
  CHECK(result->text == "{}");
  CHECK(calls == 1);

  auto status = make_tool_handler<PresenceArguments>(StatusHandler{});
  result = status({}, arguments);
  REQUIRE(result);
  CHECK(result->text == "{}");

  result =
      status({}, scrydantic::Json{.text = R"({"nullable":null,"required":"reject"})"});
  REQUIRE_FALSE(result);
  CHECK(result.error().model_message == "rejected by the handler");
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

TEST_CASE("reflected registration lowers into the additive registry") {
  auto created = scry::Harness::create(scry::test_support::test_config());
  REQUIRE(created);
  auto harness = std::move(*created);

  auto status = harness.tools().add<PresenceArguments>(
      {
          .name = "presence",
          .description = "Exercise reflected arguments",
      },
      DirectHandler{});
  REQUIRE(status);
  CHECK(harness.tools().size() == 1);

  status = harness.tools().add<PresenceArguments>(
      {
          .name = "presence",
          .description = "Duplicate",
      },
      DirectHandler{});
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(harness.tools().size() == 1);

  status = harness.tools().add<PresenceArguments>(
      {
          .name = "second_presence",
          .description = "Exercise a second reflected registration",
      },
      DirectHandler{});
  REQUIRE(status);
  CHECK(harness.tools().size() == 2);
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
  CHECK(malformed.error().message == "JSON text is not valid JSON");
  CHECK(malformed.error().model_message == "JSON text is not valid JSON");
}
