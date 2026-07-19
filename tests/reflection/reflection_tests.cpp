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

struct NestedResult {
  std::string label{};
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

struct ReferenceHandler {
  NestedResult& operator()(PresenceArguments) const;
};

struct RawJsonHandler {
  scry::Json operator()(PresenceArguments) const { return {}; }
};

struct VoidHandler {
  void operator()(PresenceArguments) const {}
};

[[nodiscard]] scry::Config test_config() {
  return {
      .base_url = "http://127.0.0.1:1",
      .api_key = "sanitized-test-key",
      .model = "test-model",
  };
}

template <scry::reflection::SupportedValue Type>
[[nodiscard]] scry::Result<Type> decode_value(const std::string_view text) {
  auto parsed =
      scry::reflection::detail::parse_json(scry::Json{.text = std::string{text}});
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  return scry::reflection::detail::decode<Type>(*parsed);
}

} // namespace

template <> struct scry::reflection::tool_traits<PresenceArguments> {
  static constexpr std::array descriptions{
      scry::reflection::parameter_description{"required", "Trait override"},
  };
};

static_assert(scry::reflection::SupportedValue<bool>);
static_assert(scry::reflection::SupportedValue<std::vector<std::int32_t>>);
static_assert(scry::reflection::SupportedValue<AllTypesArguments>);
static_assert(scry::reflection::ToolArguments<NumericArguments>);
static_assert(scry::reflection::ToolArguments<PresenceArguments>);
static_assert(scry::reflection::ToolHandlerFor<DirectHandler, PresenceArguments>);
static_assert(scry::reflection::ToolHandlerFor<ExpectedHandler, PresenceArguments>);
static_assert(!scry::reflection::ToolHandlerFor<ReferenceHandler, PresenceArguments>);
static_assert(!scry::reflection::ToolHandlerFor<RawJsonHandler, PresenceArguments>);
static_assert(!scry::reflection::ToolHandlerFor<VoidHandler, PresenceArguments>);
static_assert(!scry::reflection::ToolArguments<CharacterArguments>);
static_assert(!scry::reflection::ToolArguments<NestedOptionalArguments>);
static_assert(!scry::reflection::ToolArguments<PackedBooleanArguments>);
static_assert(!scry::reflection::ToolArguments<AliasedEnumArguments>);
static_assert(!scry::reflection::ToolArguments<UnscopedEnumArguments>);

static_assert(
    scry::reflection::input_schema_v<PresenceArguments> ==
    R"({"additionalProperties":false,"properties":{"defaulted":{"description":"C++ default","type":"string"},"nullable":{"anyOf":[{"maximum":32767,"minimum":-32768,"type":"integer"},{"type":"null"}]},"optional_nullable":{"anyOf":[{"maximum":32767,"minimum":-32768,"type":"integer"},{"type":"null"}]},"required":{"description":"Trait override","type":"string"}},"required":["nullable","required"],"type":"object"})");
static_assert(
    scry::reflection::input_schema_v<AllTypesArguments> ==
    R"({"additionalProperties":false,"properties":{"fixed":{"items":{"maximum":2147483647,"minimum":-2147483648,"type":"integer"},"maxItems":2,"minItems":2,"type":"array"},"flag":{"type":"boolean"},"nested":{"additionalProperties":false,"properties":{"label":{"type":"string"}},"required":[],"type":"object"},"ratio":{"type":"number"},"unit":{"enum":["celsius","fahrenheit"],"type":"string"},"values":{"items":{"maximum":2147483647,"minimum":-2147483648,"type":"integer"},"type":"array"}},"required":[],"type":"object"})");
static_assert(
    scry::reflection::input_schema_v<NumericArguments> ==
    R"({"additionalProperties":false,"properties":{"floating":{"type":"number"},"signed_value":{"maximum":32767,"minimum":-32768,"type":"integer"},"unsigned_value":{"maximum":65535,"minimum":0,"type":"integer"}},"required":["floating","signed_value","unsigned_value"],"type":"object"})");

TEST_CASE("reflected decoding preserves defaults and required nullability") {
  auto decoded = scry::reflection::detail::decode_arguments<PresenceArguments>(
      scry::Json{.text = R"({"nullable":null,"required":"Detroit"})"});

  REQUIRE(decoded);
  CHECK(decoded->defaulted == "fallback");
  CHECK_FALSE(decoded->nullable.has_value());
  CHECK_FALSE(decoded->optional_nullable.has_value());
  CHECK(decoded->required == "Detroit");
}

TEST_CASE("reflected decoding rejects unknown missing and mistyped members") {
  auto decoded = scry::reflection::detail::decode_arguments<PresenceArguments>(
      scry::Json{.text = R"({"nullable":null,"required":"Detroit","surprise":1})"});
  REQUIRE_FALSE(decoded);
  CHECK(decoded.error().message ==
        R"(reflected JSON at $ contains unknown member "surprise")");

  decoded = scry::reflection::detail::decode_arguments<PresenceArguments>(
      scry::Json{.text = R"({"required":"Detroit"})"});
  REQUIRE_FALSE(decoded);
  CHECK(decoded.error().message == "reflected JSON at $.nullable is a required member");

  decoded = scry::reflection::detail::decode_arguments<PresenceArguments>(
      scry::Json{.text = R"({"nullable":null,"required":4})"});
  REQUIRE_FALSE(decoded);
  CHECK(decoded.error().message == "reflected JSON at $.required must be a string");

  decoded = scry::reflection::detail::decode_arguments<PresenceArguments>(
      scry::Json{.text = "[]"});
  REQUIRE_FALSE(decoded);
  CHECK(decoded.error().message == "reflected JSON at $ must be an object");
}

TEST_CASE("reflected codec round trips every supported composite family") {
  auto decoded = scry::reflection::detail::decode_arguments<
      AllTypesArguments>(scry::Json{
      .text =
          R"({"fixed":[1,2],"flag":true,"nested":{"label":"indoors"},"ratio":1.25,"unit":"fahrenheit","values":[3,4]})"});

  REQUIRE(decoded);
  CHECK(decoded->fixed == std::array<std::int32_t, 2>{1, 2});
  CHECK(decoded->flag);
  CHECK(decoded->nested.label == "indoors");
  CHECK(decoded->ratio == 1.25);
  CHECK(decoded->unit == TemperatureUnit::fahrenheit);
  CHECK(decoded->values == std::vector<std::int32_t>{3, 4});

  const auto encoded = scry::reflection::detail::encode(*decoded);
  REQUIRE(encoded);
  CHECK(
      encoded->text ==
      R"({"fixed":[1,2],"flag":true,"nested":{"label":"indoors"},"ratio":1.25,"unit":"fahrenheit","values":[3,4]})");
}

TEST_CASE("reflected codec enforces numeric enum and fixed-array bounds") {
  auto decoded = scry::reflection::detail::decode_arguments<
      AllTypesArguments>(scry::Json{
      .text =
          R"({"fixed":[1],"flag":true,"nested":{"label":"x"},"ratio":1,"unit":"celsius","values":[]})"});
  REQUIRE_FALSE(decoded);
  CHECK(decoded.error().message ==
        "reflected JSON at $.fixed must be an array of the declared fixed size");

  decoded = scry::reflection::detail::decode_arguments<AllTypesArguments>(scry::Json{
      .text =
          R"({"fixed":[1,2],"flag":true,"nested":{"label":"x"},"ratio":1,"unit":"kelvin","values":[]})"});
  REQUIRE_FALSE(decoded);
  CHECK(decoded.error().message ==
        "reflected JSON at $.unit is not a declared enumerator");

  auto narrow = scry::reflection::detail::decode_arguments<PresenceArguments>(
      scry::Json{.text = R"({"nullable":32768,"required":"x"})"});
  REQUIRE_FALSE(narrow);
  CHECK(narrow.error().message ==
        "reflected JSON at $.nullable is outside the integer range");

  narrow = scry::reflection::detail::decode_arguments<PresenceArguments>(
      scry::Json{.text = R"({"nullable":1.0,"required":"x"})"});
  REQUIRE_FALSE(narrow);
  CHECK(narrow.error().message == "reflected JSON at $.nullable must be an integer");
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
      scry::Json{.text = R"({"floating":1,"signed_value":32768,"unsigned_value":1})"});
  REQUIRE_FALSE(decoded);
  CHECK(decoded.error().message ==
        "reflected JSON at $.signed_value is outside the integer range");

  decoded = scry::reflection::detail::decode_arguments<NumericArguments>(
      scry::Json{.text = R"({"floating":1,"signed_value":0,"unsigned_value":-1})"});
  REQUIRE_FALSE(decoded);
  CHECK(decoded.error().message ==
        "reflected JSON at $.unsigned_value is outside the integer range");

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

TEST_CASE("reflected argument parsing rejects malformed JSON") {
  const auto decoded = scry::reflection::detail::decode_arguments<PresenceArguments>(
      scry::Json{.text = "{"});
  REQUIRE_FALSE(decoded);
  CHECK(decoded.error().message == "reflected tool arguments are not valid JSON");
}

TEST_CASE("reflected encoding covers nullable enum and sequence results") {
  CHECK(scry::reflection::detail::encode(std::optional<std::int16_t>{})->text ==
        "null");
  CHECK(scry::reflection::detail::encode(std::optional<std::int16_t>{std::int16_t{7}})
            ->text == "7");
  CHECK(scry::reflection::detail::encode(TemperatureUnit::celsius)->text ==
        R"("celsius")");
  CHECK(scry::reflection::detail::encode(TemperatureUnit::fahrenheit)->text ==
        R"("fahrenheit")");
  CHECK(scry::reflection::detail::encode(std::vector<std::int32_t>{})->text == "[]");
  CHECK(scry::reflection::detail::encode(std::vector<std::int32_t>{1, 2})->text ==
        "[1,2]");
}

TEST_CASE("reflected sequence encoding propagates fallible element errors") {
  const auto encoded = scry::reflection::detail::encode(
      std::vector<double>{1.0, std::numeric_limits<double>::infinity()});
  REQUIRE_FALSE(encoded);
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
  auto encoded = scry::reflection::detail::encode(value);
  REQUIRE_FALSE(encoded);
  CHECK(encoded.error().message == "reflected JSON at $.ratio must be finite");

  value.ratio = 1.0;
  value.unit = static_cast<TemperatureUnit>(99);
  encoded = scry::reflection::detail::encode(value);
  REQUIRE_FALSE(encoded);
  CHECK(encoded.error().message ==
        "reflected JSON at $.unit is not a declared enumerator value");
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

  auto result = handler(scry::Json{.text = R"({"nullable":null,"required":"ok"})"});
  REQUIRE(result);
  CHECK(result->text == R"({"label":"handled:ok"})");

  result = handler(scry::Json{.text = R"({"nullable":null,"required":"reject"})"});
  REQUIRE_FALSE(result);
  CHECK(result.error().message == "application rejected arguments");

  result = handler(scry::Json{.text = R"({"required":"missing nullable"})"});
  REQUIRE_FALSE(result);
  CHECK(result.error().category == scry::ErrorCategory::tool);
}

TEST_CASE("reflected registration lowers into the additive registry") {
  auto created = scry::Harness::create(test_config());
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
  CHECK(status.error().category == scry::ErrorCategory::invalid_state);
  CHECK(harness.tools().size() == 1);

  status = scry::reflection::add<PresenceArguments>(
      harness.tools(),
      {
          .name = "worker_presence",
          .description = "Exercise reflected worker registration",
      },
      DirectHandler{}, {.execution = scry::ToolExecution::worker_thread});
  REQUIRE(status);
  CHECK(harness.tools().size() == 2);
}
