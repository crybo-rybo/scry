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
#include <scry/reflection.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

enum class TemperatureUnit {
  celsius,
  fahrenheit,
};

enum class AliasedUnit {
  first = 1,
  alias = 1,
};

enum UnscopedUnit {
  unscoped,
};

struct PresenceArguments {
  [[= scry::reflection::description{"C++ default"}]] std::string defaulted{"fallback"};
  std::optional<std::int16_t> nullable;
  std::optional<std::int16_t> optional_nullable = std::nullopt;
  [[= scry::reflection::description{"Annotation text"}]] std::string required;
};

struct EscapedDescriptionArguments {
  [[= scry::reflection::description{
      "quote\" slash\\\b\f\n\r\t\x01"}]] std::string value;
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

struct NumericArguments {
  float floating;
  std::int16_t signed_value;
  std::uint16_t unsigned_value;
};

struct CharacterArguments {
  char value{};
};

struct NestedOptionalArguments {
  std::optional<std::optional<int>> value{};
};

struct PackedBooleanArguments {
  std::vector<bool> value{};
};

struct AliasedEnumArguments {
  AliasedUnit value{};
};

struct UnscopedEnumArguments {
  UnscopedUnit value{};
};

struct DirectHandler {
  NestedResult operator()(PresenceArguments) const { return {}; }
};

struct ExpectedHandler {
  scry::Result<NestedResult> operator()(PresenceArguments) const { return {}; }
};

// A const-qualified value is supported, so a Result of one is too, just as a
// handler returning a const value directly is.
struct ConstExpectedHandler {
  scry::Result<const std::int32_t> operator()(PresenceArguments) const { return 7; }
};

struct ReferenceHandler {
  NestedResult& operator()(PresenceArguments) const;
};

struct RawJsonHandler {
  scry::Json operator()(PresenceArguments) const { return {}; }
};

struct VoidHandler {
  void operator()(PresenceArguments) const {}
};

// A text catalog: the shape description_of exists for. The views need static storage
// because each one is bound as a reference template parameter.
namespace catalog {
inline constexpr std::string_view direction = "Direction of travel";
} // namespace catalog

struct CatalogArguments {
  [[= scry::reflection::description_of<catalog::direction>()]] std::string heading;
};

struct LiteralArguments {
  [[= scry::reflection::description{"Direction of travel"}]] std::string heading;
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

template <scry::reflection::SupportedValue Type>
[[nodiscard]] scry::Result<Type> decode_value(const std::string_view text) {
  auto parsed = scry::JsonView::parse(scry::Json{.text = std::string{text}});
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  return scry::reflection::detail::decode<Type>(*parsed);
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

static_assert(scry::reflection::SupportedValue<bool>);
static_assert(scry::reflection::SupportedValue<std::vector<std::int32_t>>);
static_assert(scry::reflection::SupportedValue<AllTypesArguments>);
static_assert(scry::reflection::ToolArguments<NumericArguments>);
static_assert(scry::reflection::ToolArguments<PresenceArguments>);
static_assert(scry::reflection::ToolHandlerFor<DirectHandler, PresenceArguments>);
static_assert(scry::reflection::ToolHandlerFor<ExpectedHandler, PresenceArguments>);
static_assert(
    scry::reflection::ToolHandlerFor<ConstExpectedHandler, PresenceArguments>);
static_assert(!scry::reflection::ToolHandlerFor<ReferenceHandler, PresenceArguments>);
static_assert(!scry::reflection::ToolHandlerFor<RawJsonHandler, PresenceArguments>);
static_assert(!scry::reflection::ToolHandlerFor<VoidHandler, PresenceArguments>);
static_assert(scry::reflection::ToolHandlerFor<ContextHandler, PresenceArguments>);
static_assert(
    !scry::reflection::ToolHandlerFor<ReversedContextHandler, PresenceArguments>);
static_assert(!scry::reflection::ToolHandlerFor<VoidContextHandler, PresenceArguments>);
static_assert(!scry::reflection::ToolArguments<CharacterArguments>);
static_assert(!scry::reflection::ToolArguments<NestedOptionalArguments>);
static_assert(!scry::reflection::ToolArguments<PackedBooleanArguments>);
static_assert(!scry::reflection::ToolArguments<AliasedEnumArguments>);
static_assert(!scry::reflection::ToolArguments<UnscopedEnumArguments>);

static_assert(
    scry::reflection::input_schema_v<PresenceArguments> ==
    R"({"additionalProperties":false,"properties":{"defaulted":{"description":"C++ default","type":"string"},"nullable":{"anyOf":[{"maximum":32767,"minimum":-32768,"type":"integer"},{"type":"null"}]},"optional_nullable":{"anyOf":[{"maximum":32767,"minimum":-32768,"type":"integer"},{"type":"null"}]},"required":{"description":"Annotation text","type":"string"}},"required":["nullable","required"],"type":"object"})");
static_assert(
    scry::reflection::input_schema_v<EscapedDescriptionArguments> ==
    R"({"additionalProperties":false,"properties":{"value":{"description":"quote\" slash\\\b\f\n\r\t\u0001","type":"string"}},"required":["value"],"type":"object"})");
static_assert(
    scry::reflection::input_schema_v<AllTypesArguments> ==
    R"({"additionalProperties":false,"properties":{"fixed":{"items":{"maximum":2147483647,"minimum":-2147483648,"type":"integer"},"maxItems":2,"minItems":2,"type":"array"},"flag":{"type":"boolean"},"nested":{"additionalProperties":false,"properties":{"label":{"type":"string"}},"required":[],"type":"object"},"ratio":{"type":"number"},"unit":{"enum":["celsius","fahrenheit"],"type":"string"},"values":{"items":{"maximum":2147483647,"minimum":-2147483648,"type":"integer"},"type":"array"}},"required":[],"type":"object"})");
static_assert(
    scry::reflection::input_schema_v<NumericArguments> ==
    R"({"additionalProperties":false,"properties":{"floating":{"type":"number"},"signed_value":{"maximum":32767,"minimum":-32768,"type":"integer"},"unsigned_value":{"maximum":65535,"minimum":0,"type":"integer"}},"required":["floating","signed_value","unsigned_value"],"type":"object"})");

// A catalog-sourced description must be indistinguishable from the literal form.
static_assert(scry::reflection::input_schema_v<CatalogArguments> ==
              scry::reflection::input_schema_v<LiteralArguments>);
static_assert(
    scry::reflection::input_schema_v<CatalogArguments> ==
    R"({"additionalProperties":false,"properties":{"heading":{"description":"Direction of travel","type":"string"}},"required":["heading"],"type":"object"})");
static_assert(scry::reflection::description_of<catalog::direction>().view() ==
              catalog::direction);

// Result types and bare enums are schema roots too, even though Scry never sends
// either to a model.
static_assert(std::same_as<decltype(scry::reflection::schema_v<NestedResult>),
                           const std::string_view>);
static_assert(scry::reflection::schema_v<NestedResult> ==
              scry::reflection::input_schema_v<NestedResult>);
static_assert(
    scry::reflection::schema_v<NestedResult> ==
    R"({"additionalProperties":false,"properties":{"label":{"type":"string"}},"required":[],"type":"object"})");
static_assert(scry::reflection::schema_v<TemperatureUnit> ==
              R"({"enum":["celsius","fahrenheit"],"type":"string"})");
static_assert(
    scry::reflection::schema_v<std::vector<TemperatureUnit>> ==
    R"({"items":{"enum":["celsius","fahrenheit"],"type":"string"},"type":"array"})");

TEST_CASE("reflected decoding preserves defaults and required nullability") {
  auto decoded = scry::reflection::detail::decode_arguments<PresenceArguments>(
      scry::Json{.text = R"({"nullable":null,"required":"Detroit"})"});

  REQUIRE(decoded);
  CHECK(decoded->defaulted == "fallback");
  CHECK_FALSE(decoded->nullable.has_value());
  CHECK_FALSE(decoded->optional_nullable.has_value());
  CHECK(decoded->required == "Detroit");
}

TEST_CASE("reflected decode failures carry a schema-derived model message") {
  const auto model_message = [](const std::string_view text) {
    auto decoded = scry::reflection::detail::decode_arguments<AllTypesArguments>(
        scry::Json{.text = std::string{text}});
    REQUIRE_FALSE(decoded);
    // The model copy is the host copy without the library-facing prefix.
    CHECK(decoded.error().message ==
          "reflected JSON at " + decoded.error().model_message);
    return decoded.error().model_message;
  };

  CHECK(model_message("[]") == "$ must be an object");
  CHECK(model_message(R"({"surprise":1})") ==
        R"($ contains unknown member "surprise")");
  CHECK(model_message(R"({"fixed":[1,2],"flag":1,"nested":{},"ratio":1,)"
                      R"("unit":"celsius","values":[]})") ==
        "$.flag must be a boolean");
  CHECK(model_message(R"({"fixed":[1],"flag":true,"nested":{},"ratio":1,)"
                      R"("unit":"celsius","values":[]})") ==
        "$.fixed must be an array of the declared fixed size");
  CHECK(model_message(R"({"fixed":[1,2],"flag":true,"nested":{},"ratio":1,)"
                      R"("unit":"celsius","values":[4294967296]})") ==
        "$.values[0] is outside the integer range");
  CHECK(model_message(R"({"fixed":[1,2],"flag":true,"nested":{},"ratio":1,)"
                      R"("unit":"celsius","values":[1.0]})") ==
        "$.values[0] must be an integer");
  CHECK(model_message(R"({"fixed":[1,2],"flag":true,"nested":{"label":1},"ratio":1,)"
                      R"("unit":"celsius","values":[]})") ==
        "$.nested.label must be a string");
  CHECK(model_message(R"({"fixed":[1,2],"flag":true,"nested":{},"ratio":1,)"
                      R"("unit":"kelvin","values":[]})") ==
        "$.unit is not a declared enumerator; must be one of: celsius, fahrenheit");

  auto required = scry::reflection::detail::decode_arguments<PresenceArguments>(
      scry::Json{.text = R"({"required":"Detroit"})"});
  REQUIRE_FALSE(required);
  CHECK(required.error().message ==
        "reflected JSON at $.nullable is a required member");
  CHECK(required.error().model_message == "$.nullable is a required member");

  auto malformed = scry::reflection::detail::decode_arguments<PresenceArguments>(
      scry::Json{.text = "{"});
  REQUIRE_FALSE(malformed);
  CHECK(malformed.error().category == scry::ErrorCategory::tool);
  CHECK(malformed.error().message == "reflected tool arguments are not valid JSON");
  CHECK(malformed.error().model_message == "tool arguments are not valid JSON");
}

TEST_CASE("reflected codec round trips every supported composite family") {
  auto decoded = scry::reflection::detail::decode_arguments<
      AllTypesArguments>(scry::Json{
      .text =
          R"({"fixed":[1,2],"flag":true,"nested":{"label":"indoors"},"ratio":0.1,"unit":"fahrenheit","values":[3,4]})"});

  REQUIRE(decoded);
  CHECK(decoded->fixed == std::array<std::int32_t, 2>{1, 2});
  CHECK(decoded->flag);
  CHECK(decoded->nested.label == "indoors");
  CHECK(decoded->ratio == 0.1);
  CHECK(decoded->unit == TemperatureUnit::fahrenheit);
  CHECK(decoded->values == std::vector<std::int32_t>{3, 4});

  const auto encoded = scry::reflection::encode(*decoded);
  REQUIRE(encoded);
  CHECK(
      encoded->text ==
      R"({"fixed":[1,2],"flag":true,"nested":{"label":"indoors"},"ratio":0.1,"unit":"fahrenheit","values":[3,4]})");
}

TEST_CASE("reflected numeric decoding accepts exact boundaries and signs") {
  auto decoded =
      scry::reflection::detail::decode_arguments<NumericArguments>(scry::Json{
          .text = R"({"floating":7,"signed_value":-32768,"unsigned_value":65535})"});
  REQUIRE(decoded);
  CHECK(decoded->floating == 7.0F);
  CHECK(decoded->signed_value == std::int16_t{-32768});
  CHECK(decoded->unsigned_value == std::uint16_t{65535});

  decoded = scry::reflection::detail::decode_arguments<NumericArguments>(
      scry::Json{.text = R"({"floating":1e39,"signed_value":0,"unsigned_value":1})"});
  REQUIRE_FALSE(decoded);
  CHECK(decoded.error().message ==
        "reflected JSON at $.floating must be a finite in-range number");
}

TEST_CASE("reflected signed integer decoding covers every strict boundary") {
  CHECK(decode_value<std::int16_t>("-32768") == std::int16_t{-32768});
  CHECK(decode_value<std::int16_t>("32767") == std::int16_t{32767});
  CHECK_FALSE(decode_value<std::int16_t>("-32769"));
  CHECK_FALSE(decode_value<std::int16_t>("32768"));
  CHECK_FALSE(decode_value<std::int16_t>("1.0"));

  CHECK(decode_value<std::int32_t>("-2147483648") ==
        std::numeric_limits<std::int32_t>::lowest());
  CHECK(decode_value<std::int32_t>("2147483647") ==
        std::numeric_limits<std::int32_t>::max());
  CHECK_FALSE(decode_value<std::int32_t>("-2147483649"));
  CHECK_FALSE(decode_value<std::int32_t>("2147483648"));
  CHECK_FALSE(decode_value<std::int32_t>(R"("integer")"));
}

TEST_CASE("reflected unsigned integer decoding covers signs and boundaries") {
  CHECK(decode_value<std::uint16_t>("0") == std::uint16_t{0});
  CHECK(decode_value<std::uint16_t>("-0") == std::uint16_t{0});
  CHECK(decode_value<std::uint16_t>("65535") == std::uint16_t{65535});
  CHECK_FALSE(decode_value<std::uint16_t>("-1"));
  CHECK_FALSE(decode_value<std::uint16_t>("65536"));
  CHECK_FALSE(decode_value<std::uint16_t>("false"));
}

TEST_CASE("reflected floating decoding covers every JSON numeric kind") {
  CHECK(decode_value<float>("-1") == -1.0F);
  CHECK(decode_value<float>("1") == 1.0F);
  CHECK(decode_value<float>("1.25") == 1.25F);
  CHECK_FALSE(decode_value<float>(R"("number")"));
  CHECK_FALSE(decode_value<float>("1e39"));
  CHECK_FALSE(decode_value<float>("-1e39"));
  CHECK_FALSE(decode_value<float>("1e-50"));

  CHECK(decode_value<double>("-2") == -2.0);
  CHECK(decode_value<double>("2") == 2.0);
  CHECK(decode_value<double>("2.5") == 2.5);
  CHECK_FALSE(decode_value<double>("null"));
}

TEST_CASE("reflected scalar and enum decoding is strict") {
  CHECK(decode_value<bool>("true") == true);
  CHECK_FALSE(decode_value<bool>("1"));
  CHECK(decode_value<std::string>(R"("text")") == "text");
  CHECK_FALSE(decode_value<std::string>("false"));

  CHECK(decode_value<TemperatureUnit>(R"("celsius")") == TemperatureUnit::celsius);
  CHECK(decode_value<TemperatureUnit>(R"("fahrenheit")") ==
        TemperatureUnit::fahrenheit);
  CHECK_FALSE(decode_value<TemperatureUnit>(R"("kelvin")"));
  CHECK_FALSE(decode_value<TemperatureUnit>("1"));
}

TEST_CASE("reflected optional and sequence decoding propagates element errors") {
  CHECK_FALSE(decode_value<std::optional<std::int16_t>>("null").value());
  CHECK(decode_value<std::optional<std::int16_t>>("7").value() ==
        std::optional<std::int16_t>{7});
  CHECK_FALSE(decode_value<std::optional<std::int16_t>>(R"("bad")"));

  CHECK(decode_value<std::vector<std::int32_t>>("[]").value().empty());
  CHECK(decode_value<std::vector<std::int32_t>>("[1,-2]").value() ==
        std::vector<std::int32_t>{1, -2});
  CHECK_FALSE(decode_value<std::vector<std::int32_t>>("{}"));
  CHECK_FALSE(decode_value<std::vector<std::int32_t>>(R"([1,"bad"])"));
}

TEST_CASE("reflected fixed-array decoding distinguishes kind size and element errors") {
  using Fixed = std::array<std::int32_t, 2>;
  CHECK(decode_value<Fixed>("[1,2]").value() == Fixed{1, 2});
  CHECK_FALSE(decode_value<Fixed>("{}"));
  CHECK_FALSE(decode_value<Fixed>("[1]"));
  CHECK_FALSE(decode_value<Fixed>(R"([1,"bad"])"));
}

TEST_CASE("reflected encoding covers nullable enum and sequence results") {
  CHECK(scry::reflection::encode(std::optional<std::int16_t>{})->text == "null");
  CHECK(scry::reflection::encode(std::optional<std::int16_t>{std::int16_t{7}})->text ==
        "7");
  CHECK(scry::reflection::encode(TemperatureUnit::celsius)->text == R"("celsius")");
  CHECK(scry::reflection::encode(TemperatureUnit::fahrenheit)->text ==
        R"("fahrenheit")");
  CHECK(scry::reflection::encode(std::vector<std::int32_t>{})->text == "[]");
  CHECK(scry::reflection::encode(std::vector<std::int32_t>{1, 2})->text == "[1,2]");
}

TEST_CASE("reflected encoding uses Scry canonical number spelling") {
  const auto fraction = scry::reflection::encode(0.1);
  const auto exponent = scry::reflection::encode(1e20);
  const auto negative_zero = scry::reflection::encode(-0.0);

  REQUIRE(fraction);
  REQUIRE(exponent);
  REQUIRE(negative_zero);
  CHECK(fraction->text == "0.1");
  CHECK(exponent->text == "1E20");
  CHECK(negative_zero->text == "0");
}

TEST_CASE("reflected sequence encoding propagates fallible element errors") {
  const auto encoded = scry::reflection::encode(
      std::vector<double>{1.0, std::numeric_limits<double>::infinity()});
  REQUIRE_FALSE(encoded);
  CHECK(encoded.error().category == scry::ErrorCategory::tool);
  CHECK(encoded.error().message == "reflected JSON at $[1] must be finite");
}

TEST_CASE("reflected encoding rejects non-finite and unnamed values") {
  auto value = AllTypesArguments{
      .fixed = {1, 2},
      .flag = true,
      .nested = {.label = "x"},
      .ratio = std::numeric_limits<double>::infinity(),
      .unit = TemperatureUnit::celsius,
  };
  auto encoded = scry::reflection::encode(value);
  REQUIRE_FALSE(encoded);
  CHECK(encoded.error().category == scry::ErrorCategory::tool);
  CHECK(encoded.error().message == "reflected JSON at $.ratio must be finite");
  // A result type's schema never reaches the model, so an encoding failure
  // carries nothing for it to act on.
  CHECK(encoded.error().model_message.empty());

  value.ratio = 1.0;
  value.unit = static_cast<TemperatureUnit>(99);
  encoded = scry::reflection::encode(value);
  REQUIRE_FALSE(encoded);
  CHECK(encoded.error().category == scry::ErrorCategory::tool);
  CHECK(encoded.error().message ==
        "reflected JSON at $.unit is not a declared enumerator value");
  CHECK(encoded.error().model_message.empty());
}

TEST_CASE("reflected result-encoding failures reach the model as the fixed text") {
  auto handler = scry::reflection::detail::make_tool_handler<PresenceArguments>(
      [](PresenceArguments) {
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

TEST_CASE("public encoding matches reflected tool dispatch output") {
  const auto value = AllTypesArguments{
      .fixed = {1, 2},
      .flag = true,
      .nested = {.label = "same"},
      .ratio = 1e20,
      .unit = TemperatureUnit::fahrenheit,
      .values = {3, 4},
  };
  auto handler = scry::reflection::detail::make_tool_handler<PresenceArguments>(
      [value](PresenceArguments) { return value; });
  const auto tools = snapshot_of("snapshot", std::move(handler));

  const auto direct = scry::reflection::encode(value);
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
    auto handler =
        scry::reflection::detail::make_tool_handler<PresenceArguments>(std::move(body));
    return handler({}, scry::Json{.text = R"({"nullable":null,"required":"ok"})"});
  };

  scry::Result<scry::Json> result{};
  SECTION("bare result") {
    result = run_handler(
        [](PresenceArguments) { return CopyOnlyResult{.values = {1, 2, 3}}; });
  }
  SECTION("expected result") {
    result = run_handler([](PresenceArguments) -> scry::Result<CopyOnlyResult> {
      return scry::Result<CopyOnlyResult>{std::in_place, std::vector<int>{1, 2, 3}};
    });
  }

  REQUIRE(result);
  CHECK(result->text == R"({"values":[1,2,3]})");
  CHECK(CopyOnlyResult::destructions == 1);
}

TEST_CASE("reflected erased handlers encode a Result of a const-qualified value") {
  // Instantiating the handler is the regression: the const value type used to
  // reach encode_value unstripped and fail to compile inside the library.
  auto handler = scry::reflection::detail::make_tool_handler<PresenceArguments>(
      ConstExpectedHandler{});

  const auto result =
      handler({}, scry::Json{.text = R"({"nullable":null,"required":"ok"})"});

  REQUIRE(result);
  CHECK(result->text == "7");
}

TEST_CASE("encoding named reflected results does not copy their values") {
  CopyOnlyResult::destructions = 0;
  SECTION("bare result") {
    const CopyOnlyResult value{.values = {1, 2, 3}};
    const auto encoded = scry::reflection::detail::encode_handler_result(value);
    REQUIRE(encoded);
    CHECK(encoded->text == R"({"values":[1,2,3]})");
    CHECK(CopyOnlyResult::destructions == 0);
  }
  SECTION("expected result passed as an rvalue") {
    scry::Result<CopyOnlyResult> value{std::in_place, std::vector<int>{1, 2, 3}};
    const auto encoded =
        scry::reflection::detail::encode_handler_result(std::move(value));
    REQUIRE(encoded);
    CHECK(encoded->text == R"({"values":[1,2,3]})");
    CHECK(CopyOnlyResult::destructions == 0);
  }
  CHECK(CopyOnlyResult::destructions == 1);
}

TEST_CASE("encoding a named reflected error preserves the caller's error") {
  scry::Result<CopyOnlyResult> value = std::unexpected(scry::Error{
      .category = scry::ErrorCategory::tool,
      .message = "application rejected arguments",
  });
  const auto encoded = scry::reflection::detail::encode_handler_result(value);
  REQUIRE_FALSE(encoded);
  CHECK(encoded.error().category == scry::ErrorCategory::tool);
  CHECK(encoded.error().message == "application rejected arguments");
  CHECK(value.error().message == "application rejected arguments");
}

TEST_CASE("reflected erased handlers retain move-only captures and typed errors") {
  auto handler = scry::reflection::detail::make_tool_handler<PresenceArguments>(
      [owned = std::make_unique<std::string>("handled")](
          PresenceArguments arguments) -> scry::Result<NestedResult> {
        if (arguments.required == "reject") {
          return std::unexpected(scry::Error{
              .category = scry::ErrorCategory::tool,
              .message = "application rejected arguments",
          });
        }
        return NestedResult{.label = *owned + ":" + arguments.required};
      });

  auto result = handler({}, scry::Json{.text = R"({"nullable":null,"required":"ok"})"});
  REQUIRE(result);
  CHECK(result->text == R"({"label":"handled:ok"})");

  result = handler({}, scry::Json{.text = R"({"nullable":null,"required":"reject"})"});
  REQUIRE_FALSE(result);
  CHECK(result.error().message == "application rejected arguments");

  result = handler({}, scry::Json{.text = R"({"required":"missing nullable"})"});
  REQUIRE_FALSE(result);
  CHECK(result.error().category == scry::ErrorCategory::tool);
}

TEST_CASE("reflected registration lowers into the additive registry") {
  auto created = scry::Harness::create(scry::test_support::test_config());
  REQUIRE(created);
  auto harness = std::move(*created);

  auto status = scry::reflection::add<PresenceArguments>(
      harness.tools(),
      {
          .name = "presence",
          .description = "Exercise reflected arguments",
      },
      DirectHandler{});
  REQUIRE(status);
  CHECK(harness.tools().size() == 1);

  status = scry::reflection::add<PresenceArguments>(harness.tools(),
                                                    {
                                                        .name = "presence",
                                                        .description = "Duplicate",
                                                    },
                                                    DirectHandler{});
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(harness.tools().size() == 1);

  status = scry::reflection::add<PresenceArguments>(
      harness.tools(),
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
  REQUIRE(scry::reflection::add<PresenceArguments>(
      harness->tools(), {.name = "presence", .description = "Reflected arguments"},
      DirectHandler{}));
  REQUIRE(harness->tools().add(
      {.name = "explicit",
       .description = "Explicit arguments",
       .input_schema = {.text = R"({"type":"object"})"}},
      [](scry::Json input) -> scry::Result<scry::Json> { return input; }));

  const auto manifest = harness->tools().to_json();
  REQUIRE(manifest);
  CHECK(
      manifest->text ==
      std::string{R"({"tools":[{"description":"Reflected arguments","input_schema":)"} +
          std::string{scry::reflection::input_schema_v<PresenceArguments>} +
          R"(,"name":"presence"},{"description":"Explicit arguments","input_schema":{"type":"object"},"name":"explicit"}],"version":1})");
}

TEST_CASE("reflected handlers may take the call context as a leading parameter") {
  auto created = scry::Harness::create(scry::test_support::test_config());
  REQUIRE(created);
  auto harness = std::move(*created);

  std::string observed_call_id;
  std::string observed_tool_name;
  REQUIRE(scry::reflection::add<PresenceArguments>(
      harness.tools(),
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
  REQUIRE(scry::reflection::add<PresenceArguments>(harness.tools(),
                                                   {
                                                       .name = "plain",
                                                       .description = "Ignore the call",
                                                   },
                                                   DirectHandler{}));
  CHECK(harness.tools().size() == 2);

  const auto snapshot =
      scry::detail::ToolRegistryAccess::snapshot(harness.tools()).entries;
  REQUIRE(snapshot);
  const auto arguments = scry::Json{.text = R"({"nullable":null,"required":"value"})"};

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
