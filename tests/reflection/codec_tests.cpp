#include "runtime/tool_dispatch.hpp"
#include "runtime/tool_registry_impl.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <scry/annotations.hpp>
#include <scry/reflection.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace reflection = scry::reflection;

// ---- Tagged variants ---------------------------------------------------------

struct[[= reflection::tag{"circle"}]] Circle {
  double radius{};
};

// "height" sorts before the "type" tag key and "width" after it, so the tag lands
// between them.
struct[[= reflection::tag{"rectangle"}]] Rectangle {
  double height{};
  double width{};
};

using Shape = std::variant<Circle, Rectangle>;

struct Drawing {
  [[= reflection::description{"Shapes to draw"}]] std::vector<Shape> shapes;
  std::optional<Shape> highlight = std::nullopt;
};

// Alternatives with no members, and one whose only key sorts after the tag.
struct[[= reflection::tag{"empty"}]] EmptyAlternative {};

struct[[= reflection::tag{"late"}]] LateAlternative {
  int zulu{};
};

using Edge = std::variant<EmptyAlternative, LateAlternative>;

// ---- Keys and null policy ----------------------------------------------------

struct Renamed {
  [[= reflection::name{"max_tokens"}]] std::uint32_t limit{};
  // One bracket may hold several annotations, each introduced by its own `=`.
  [[
    = reflection::name{"a_first"},
    = reflection::description{"Sorted by its key"}
  ]] std::string zed;
};

struct[[= reflection::skip_null]] Sparse {
  std::optional<int> absent;
  [[= reflection::emit_null]] std::optional<int> explicit_null;
  std::optional<int> initialized = 3;
  int plain{};
};

struct MemberSkip {
  [[= reflection::skip_null]] std::optional<std::string> note;
  std::optional<std::string> other;
};

// ---- Lenient decoding --------------------------------------------------------

struct[[= reflection::ignore_unknown]] LenientInner {
  int kept{};
};

struct[[= reflection::ignore_unknown]] LenientEvent {
  LenientInner inner;
  std::string kind;
};

struct StrictHolder {
  LenientInner inner;
};

struct[[= reflection::tag{"delta"}]][[= reflection::ignore_unknown]] LenientDelta {
  std::string text;
};

struct[[= reflection::tag{"stop"}]] StrictStop {};

using StreamEvent = std::variant<LenientDelta, StrictStop>;

// ---- Wire-like and document-like shapes --------------------------------------

struct WireTool {
  std::string_view description{};
  scry::Json input_schema{};
  std::string_view name{};
};

struct[[= reflection::skip_null]] WireBody {
  std::optional<std::uint32_t> max_tokens{};
  std::string_view model{};
  bool stream{true};
  std::optional<std::string_view> system{};
  double temperature{};
  std::optional<std::vector<WireTool>> tools{};
  std::optional<double> top_p{};
};

enum class Role {
  user,
  assistant,
};

struct[[= reflection::tag{"text"}]] DocText {
  std::string text;
};

struct[[= reflection::tag{"tool_call"}]] DocToolCall {
  scry::Json arguments;
  std::string id;
  std::string name;
};

struct[[= reflection::tag{"tool_result"}]] DocToolResult {
  bool is_error{};
  scry::Json result;
  std::string tool_call_id;
};

using DocBlock = std::variant<DocText, DocToolCall, DocToolResult>;

struct DocMessage {
  std::vector<DocBlock> content;
  Role role{};
};

struct Document {
  std::vector<DocMessage> messages;
  std::string system_prompt;
  std::uint64_t version{};
};

// ---- Families ----------------------------------------------------------------

struct ConstView {
  const int value;
};

struct Payload {
  scry::Json body;
};

// Rejected shapes, checked through the diagnostics the entry points print.
struct Unannotated {
  int value{};
};

struct[[= reflection::tag{"left"}]] Left {
  int value{};
};

struct[[= reflection::tag{"left"}]] AlsoLeft {
  int other{};
};

struct[[= reflection::tag{"typed"}]] TypedAlternative {
  [[= reflection::name{"type"}]] std::string kind;
};

struct Colliding {
  [[= reflection::name{"key"}]] int first{};
  [[= reflection::name{"key"}]] int second{};
};

struct MisplacedSkip {
  [[= reflection::skip_null]] int value{};
};

struct HoldsView {
  std::string_view text;
};

struct Nested {
  std::vector<std::optional<wchar_t>> letters;
};

struct Outer {
  Nested nested;
};

struct[[= reflection::tag{"bad"}]] BadAlternative {
  char letter{};
};

struct HoldsBad {
  std::vector<std::variant<BadAlternative>> items;
};

template <typename Type>
[[nodiscard]] scry::Result<Type> decode_text(const std::string_view text) {
  return reflection::decode<Type>(scry::Json{.text = std::string{text}});
}

// A dispatchable snapshot holding one reflected handler under the given name.
[[nodiscard]] scry::detail::ToolSnapshot
snapshot_of(std::string tool_name, scry::ContextualToolHandler handler) {
  return {
      std::make_shared<const scry::detail::RegisteredTool>(scry::detail::RegisteredTool{
          .definition =
              {
                  .name = std::move(tool_name),
                  .description = "Reflected test tool",
                  .input_schema = {.text = "{}"},
              },
          .handler = std::move(handler),
      })};
}

} // namespace

// ---- Family membership -------------------------------------------------------

static_assert(reflection::SupportedValue<Shape>);
static_assert(reflection::SupportedValue<Drawing>);
static_assert(reflection::ToolArguments<Drawing>);
static_assert(reflection::SupportedValue<Edge>);
static_assert(reflection::SupportedValue<Renamed>);
static_assert(reflection::SupportedValue<Sparse>);
static_assert(reflection::SupportedValue<StreamEvent>);
static_assert(!reflection::ToolArguments<Shape>);

static_assert(reflection::Encodable<WireBody>);
static_assert(!reflection::Decodable<WireBody>);
static_assert(!reflection::SupportedValue<WireBody>);

static_assert(reflection::Encodable<Document>);
static_assert(reflection::Decodable<Document>);
static_assert(!reflection::SupportedValue<Document>);
static_assert(!reflection::ToolArguments<Document>);

static_assert(reflection::Encodable<ConstView>);
static_assert(!reflection::Decodable<ConstView>);
static_assert(reflection::Decodable<Payload>);
static_assert(reflection::Encodable<scry::Json>);
static_assert(reflection::Decodable<std::optional<scry::Json>>);
static_assert(!reflection::SupportedValue<scry::Json>);
static_assert(reflection::Encodable<std::string_view>);
static_assert(!reflection::Decodable<std::string_view>);

// decode<T>() produces a value, so its target is a cv-unqualified object type, and
// the concept agrees with the entry point's check.
static_assert(!reflection::Decodable<const Payload>);
static_assert(!reflection::Decodable<volatile Payload>);
static_assert(!reflection::Decodable<Payload&>);
static_assert(!reflection::Decodable<const Payload&>);
static_assert(!reflection::Decodable<Payload&&>);

namespace constrained {

// A wrapper constrained on the concept, as the concepts' documentation suggests,
// never reaches decode<T>() with a target decode<T>() rejects.
template <typename Type>
  requires reflection::Decodable<Type>
scry::Result<Type> decode_checked(const scry::Json& json) {
  return reflection::decode<Type>(json);
}

template <typename Type>
concept checked_decodable =
    requires(const scry::Json& json) { decode_checked<Type>(json); };

static_assert(checked_decodable<Payload>);
static_assert(!checked_decodable<const Payload>);
static_assert(!checked_decodable<Payload&>);

} // namespace constrained

static_assert(!reflection::SupportedValue<std::variant<Unannotated>>);
static_assert(!reflection::SupportedValue<std::variant<Left, int>>);
static_assert(!reflection::SupportedValue<std::variant<Left, AlsoLeft>>);
static_assert(!reflection::SupportedValue<std::variant<TypedAlternative>>);
static_assert(!reflection::SupportedValue<Colliding>);
static_assert(!reflection::SupportedValue<MisplacedSkip>);
// A tagged class outside a variant, and a type key outside one, are ordinary.
static_assert(reflection::SupportedValue<Left>);
static_assert(reflection::SupportedValue<TypedAlternative>);

// ---- Diagnostics -------------------------------------------------------------

namespace {

// Index-based rather than string_view::find: under AddressSanitizer GCC 16 cannot
// constant-fold find's null check on an interior pointer of a static string.
[[nodiscard]] consteval bool mentions(const std::string_view text,
                                      const std::string_view fragment) {
  for (std::size_t start = 0; start + fragment.size() <= text.size(); ++start) {
    bool matched = true;
    for (std::size_t index = 0; index < fragment.size() && matched; ++index) {
      matched = text[start + index] == fragment[index];
    }
    if (matched) {
      return true;
    }
  }
  return false;
}

} // namespace

static_assert(mentions(reflection::detail::supported_diagnostic<Outer>(),
                       "does not satisfy scry::reflection::SupportedValue: "));
static_assert(mentions(reflection::detail::supported_diagnostic<Outer>(),
                       "Outer::nested.letters[]: wchar_t is a character type"));
static_assert(mentions(reflection::detail::supported_diagnostic<HoldsBad>(),
                       "HoldsBad::items[]<"));
static_assert(mentions(reflection::detail::supported_diagnostic<HoldsBad>(),
                       "BadAlternative>.letter: char is a character type"));
static_assert(
    mentions(reflection::detail::supported_diagnostic<std::variant<Unannotated>>(),
             "without a scry::reflection::tag annotation"));
static_assert(
    mentions(reflection::detail::supported_diagnostic<std::variant<Left, int>>(),
             "has the alternative int, which is not a plain aggregate"));
static_assert(
    mentions(reflection::detail::supported_diagnostic<std::variant<Left, AlsoLeft>>(),
             "that share the tag \"left\""));
static_assert(
    mentions(reflection::detail::supported_diagnostic<std::variant<TypedAlternative>>(),
             "TypedAlternative::kind: has the JSON key \"type\""));
static_assert(mentions(reflection::detail::supported_diagnostic<Colliding>(),
                       "has members first and second that both map to the JSON key "
                       "\"key\""));
static_assert(mentions(reflection::detail::supported_diagnostic<MisplacedSkip>(),
                       "MisplacedSkip::value: is not a std::optional"));
static_assert(mentions(reflection::detail::supported_diagnostic<HoldsView>(),
                       "HoldsView::text: "));
static_assert(mentions(reflection::detail::supported_diagnostic<HoldsView>(),
                       " is encode-only borrowed text; decoding and schemas take "
                       "std::string"));
static_assert(mentions(reflection::detail::decodable_diagnostic<ConstView>(),
                       "ConstView::value: is a const member"));
static_assert(mentions(reflection::detail::supported_diagnostic<Document>(),
                       "scry::Json is raw JSON, which has no schema"));
static_assert(mentions(reflection::detail::tool_arguments_diagnostic<Shape>(),
                       "is not a plain aggregate; tool arguments decode from one "
                       "JSON object"));
static_assert(reflection::detail::supported_diagnostic<Drawing>().empty());

// ---- Schemas -----------------------------------------------------------------

static_assert(
    reflection::schema_v<Shape> ==
    R"({"anyOf":[{"additionalProperties":false,"properties":{"radius":{"type":"number"},"type":{"enum":["circle"],"type":"string"}},"required":["type"],"type":"object"},{"additionalProperties":false,"properties":{"height":{"type":"number"},"type":{"enum":["rectangle"],"type":"string"},"width":{"type":"number"}},"required":["type"],"type":"object"}]})");

// An optional variant joins null to the alternatives; a described variant keeps
// its description beside anyOf.
static_assert(
    reflection::input_schema_v<Drawing> ==
    R"({"additionalProperties":false,"properties":{"highlight":{"anyOf":[{"additionalProperties":false,"properties":{"radius":{"type":"number"},"type":{"enum":["circle"],"type":"string"}},"required":["type"],"type":"object"},{"additionalProperties":false,"properties":{"height":{"type":"number"},"type":{"enum":["rectangle"],"type":"string"},"width":{"type":"number"}},"required":["type"],"type":"object"},{"type":"null"}]},"shapes":{"description":"Shapes to draw","items":{"anyOf":[{"additionalProperties":false,"properties":{"radius":{"type":"number"},"type":{"enum":["circle"],"type":"string"}},"required":["type"],"type":"object"},{"additionalProperties":false,"properties":{"height":{"type":"number"},"type":{"enum":["rectangle"],"type":"string"},"width":{"type":"number"}},"required":["type"],"type":"object"}]},"type":"array"}},"required":["shapes"],"type":"object"})");

static_assert(
    reflection::schema_v<Edge> ==
    R"({"anyOf":[{"additionalProperties":false,"properties":{"type":{"enum":["empty"],"type":"string"}},"required":["type"],"type":"object"},{"additionalProperties":false,"properties":{"type":{"enum":["late"],"type":"string"},"zulu":{"maximum":2147483647,"minimum":-2147483648,"type":"integer"}},"required":["type"],"type":"object"}]})");

static_assert(
    reflection::schema_v<Renamed> ==
    R"({"additionalProperties":false,"properties":{"a_first":{"description":"Sorted by its key","type":"string"},"max_tokens":{"maximum":4294967295,"minimum":0,"type":"integer"}},"required":["a_first"],"type":"object"})");

// A skip_null member may be absent, so only the emit_null one stays required.
static_assert(
    reflection::schema_v<Sparse> ==
    R"({"additionalProperties":false,"properties":{"absent":{"anyOf":[{"maximum":2147483647,"minimum":-2147483648,"type":"integer"},{"type":"null"}]},"explicit_null":{"anyOf":[{"maximum":2147483647,"minimum":-2147483648,"type":"integer"},{"type":"null"}]},"initialized":{"anyOf":[{"maximum":2147483647,"minimum":-2147483648,"type":"integer"},{"type":"null"}]},"plain":{"maximum":2147483647,"minimum":-2147483648,"type":"integer"}},"required":["explicit_null"],"type":"object"})");

// Lenient decoding does not open the schema: it describes what a producer sends.
static_assert(
    reflection::schema_v<LenientInner> ==
    R"({"additionalProperties":false,"properties":{"kept":{"maximum":2147483647,"minimum":-2147483648,"type":"integer"}},"required":[],"type":"object"})");

static_assert(reflection::schema_v<std::uint64_t> ==
              R"({"maximum":18446744073709551615,"minimum":0,"type":"integer"})");
static_assert(
    reflection::schema_v<std::int64_t> ==
    R"({"maximum":9223372036854775807,"minimum":-9223372036854775808,"type":"integer"})");
static_assert(reflection::schema_v<std::int16_t> ==
              R"({"maximum":32767,"minimum":-32768,"type":"integer"})");

// ---- Runtime -----------------------------------------------------------------

TEST_CASE("tagged variants encode the tag at its canonical position") {
  const Drawing drawing{
      .shapes = {Circle{.radius = 1.5}, Rectangle{.height = 2, .width = 3}},
      .highlight = Shape{Rectangle{.height = 4, .width = 5}},
  };
  const auto expected = std::string{
      R"({"highlight":{"height":4,"type":"rectangle","width":5},)"
      R"("shapes":[{"radius":1.5,"type":"circle"},{"height":2,"type":"rectangle","width":3}]})"};

  const auto direct = reflection::detail::encode_value(drawing);
  REQUIRE(direct);
  CHECK(direct->text == expected);
  const auto canonical = reflection::encode(drawing);
  REQUIRE(canonical);
  CHECK(canonical->text == expected);

  CHECK(reflection::encode(Edge{EmptyAlternative{}})->text == R"({"type":"empty"})");
  CHECK(reflection::encode(Edge{LateAlternative{.zulu = 1}})->text ==
        R"({"type":"late","zulu":1})");
}

TEST_CASE("tagged variants decode by tag and round trip") {
  const auto text =
      std::string{R"({"highlight":null,"shapes":[{"radius":1.5,"type":"circle"},)"
                  R"({"height":2,"type":"rectangle","width":3}]})"};
  const auto decoded = decode_text<Drawing>(text);
  REQUIRE(decoded);
  REQUIRE(decoded->shapes.size() == 2);
  REQUIRE(std::holds_alternative<Circle>(decoded->shapes[0]));
  CHECK(std::get<Circle>(decoded->shapes[0]).radius == 1.5);
  REQUIRE(std::holds_alternative<Rectangle>(decoded->shapes[1]));
  CHECK(std::get<Rectangle>(decoded->shapes[1]).width == 3.0);
  CHECK_FALSE(decoded->highlight.has_value());
  CHECK(reflection::encode(*decoded)->text == text);

  const auto empty = decode_text<Edge>(R"({"type":"empty"})");
  REQUIRE(empty);
  CHECK(std::holds_alternative<EmptyAlternative>(*empty));
}

TEST_CASE("tagged variant decode failures list the declared tags") {
  const auto failure = [](const std::string_view text) {
    const auto decoded = decode_text<Drawing>(text);
    REQUIRE_FALSE(decoded);
    CHECK(decoded.error().category == scry::ErrorCategory::invalid_argument);
    CHECK(decoded.error().message ==
          "reflected JSON at " + decoded.error().model_message);
    return decoded.error().model_message;
  };

  CHECK(failure(R"({"shapes":[{"radius":1}]})") ==
        "$.shapes[0].type is a required member; must be one of: circle, rectangle");
  CHECK(failure(R"({"shapes":[{"type":"hexagon"}]})") ==
        "$.shapes[0].type must be one of: circle, rectangle");
  CHECK(failure(R"({"shapes":[{"type":7}]})") ==
        "$.shapes[0].type must be one of: circle, rectangle");
  CHECK(failure(R"({"shapes":[3]})") == "$.shapes[0] must be an object");
  CHECK(failure(R"({"shapes":[{"radius":"big","type":"circle"}]})") ==
        "$.shapes[0].radius must be a number");
  CHECK(failure(R"({"shapes":[{"radius":1,"side":2,"type":"circle"}]})") ==
        R"($.shapes[0] contains unknown member "side")");
  CHECK(failure(R"({"highlight":{"type":"square"},"shapes":[]})") ==
        "$.highlight.type must be one of: circle, rectangle");
}

TEST_CASE("a tagged class outside a variant neither writes nor accepts its tag") {
  CHECK(reflection::encode(Left{.value = 1})->text == R"({"value":1})");
  const auto decoded = decode_text<Left>(R"({"type":"left","value":1})");
  REQUIRE_FALSE(decoded);
  CHECK(decoded.error().model_message == R"($ contains unknown member "type")");
}

TEST_CASE("tagged variant arguments reach a reflected tool handler") {
  auto handler = reflection::detail::make_tool_handler<Drawing>([](Drawing drawing) {
    double area = 0;
    for (const auto& shape : drawing.shapes) {
      if (const auto* rectangle = std::get_if<Rectangle>(&shape)) {
        area += rectangle->height * rectangle->width;
      }
    }
    return area;
  });
  auto result = handler(
      {},
      scry::Json{.text = R"({"shapes":[{"type":"rectangle","height":2,"width":3}]})"});
  REQUIRE(result);
  CHECK(result->text == "6");

  result = handler({}, scry::Json{.text = R"({"shapes":[{"type":"triangle"}]})"});
  REQUIRE_FALSE(result);
  // The tool path keeps its own category; only the public decode reports
  // invalid_argument.
  CHECK(result.error().category == scry::ErrorCategory::tool);
  CHECK(result.error().model_message ==
        "$.shapes[0].type must be one of: circle, rectangle");
}

TEST_CASE("name annotations replace keys in encoding decoding and errors") {
  const Renamed value{.limit = 5, .zed = "z"};
  CHECK(reflection::encode(value)->text == R"({"a_first":"z","max_tokens":5})");

  const auto decoded = decode_text<Renamed>(R"({"a_first":"y","max_tokens":7})");
  REQUIRE(decoded);
  CHECK(decoded->limit == 7U);
  CHECK(decoded->zed == "y");

  auto rejected = decode_text<Renamed>(R"({"a_first":"y","limit":7})");
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().model_message == R"($ contains unknown member "limit")");

  rejected = decode_text<Renamed>(R"({"a_first":"y","max_tokens":-1})");
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().message ==
        "reflected JSON at $.max_tokens is outside the integer range");

  rejected = decode_text<Renamed>(R"({"max_tokens":1})");
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().model_message == "$.a_first is a required member");

  // With several bad members, the first in declaration order is reported, as it
  // was before keys could be renamed, even though its key sorts later.
  rejected = decode_text<Renamed>(R"({"a_first":1,"max_tokens":"x"})");
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().model_message == "$.max_tokens must be an integer");
}

TEST_CASE("skip_null omits disengaged members and reads their absence as null") {
  CHECK(reflection::encode(Sparse{.absent = std::nullopt,
                                  .explicit_null = std::nullopt,
                                  .initialized = std::nullopt})
            ->text == R"({"explicit_null":null,"plain":0})");
  CHECK(reflection::encode(Sparse{.absent = 1, .explicit_null = 2, .initialized = 3})
            ->text == R"({"absent":1,"explicit_null":2,"initialized":3,"plain":0})");

  // Absence overrides the initializer, so an encoded value decodes to itself.
  const auto decoded = decode_text<Sparse>(R"({"explicit_null":null})");
  REQUIRE(decoded);
  CHECK_FALSE(decoded->absent.has_value());
  CHECK_FALSE(decoded->explicit_null.has_value());
  CHECK_FALSE(decoded->initialized.has_value());
  CHECK(decoded->plain == 0);

  // An explicit null is still accepted where the member may be omitted.
  const auto explicit_null =
      decode_text<Sparse>(R"({"absent":null,"explicit_null":1})");
  REQUIRE(explicit_null);
  CHECK_FALSE(explicit_null->absent.has_value());
  CHECK(explicit_null->explicit_null == 1);

  const auto missing = decode_text<Sparse>("{}");
  REQUIRE_FALSE(missing);
  CHECK(missing.error().model_message == "$.explicit_null is a required member");

  CHECK(reflection::encode(MemberSkip{.note = std::nullopt, .other = std::nullopt})
            ->text == R"({"other":null})");
  CHECK(reflection::encode(MemberSkip{.note = "n", .other = std::nullopt})->text ==
        R"({"note":"n","other":null})");
  const auto member = decode_text<MemberSkip>(R"({"other":"o"})");
  REQUIRE(member);
  CHECK_FALSE(member->note.has_value());
  CHECK(member->other == "o");
}

TEST_CASE("ignore_unknown decodes leniently at its own level only") {
  const auto lenient = decode_text<LenientEvent>(
      R"({"inner":{"extra":1,"kept":2},"kind":"k","other":[1,2]})");
  REQUIRE(lenient);
  CHECK(lenient->inner.kept == 2);
  CHECK(lenient->kind == "k");

  // Unknown members are ignored; the declared ones are as strict as ever.
  auto rejected = decode_text<LenientEvent>(R"({"inner":{"kept":"two"},"kind":"k"})");
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().model_message == "$.inner.kept must be an integer");
  rejected = decode_text<LenientEvent>(R"({"inner":{}})");
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().model_message == "$.kind is a required member");

  // A strict class around a lenient one stays strict.
  CHECK(decode_text<StrictHolder>(R"({"inner":{"extra":1,"kept":2}})"));
  const auto strict = decode_text<StrictHolder>(R"({"inner":{"kept":2},"surprise":1})");
  REQUIRE_FALSE(strict);
  CHECK(strict.error().model_message == R"($ contains unknown member "surprise")");

  // Leniency is per alternative, and never swallows the tag.
  const auto delta =
      decode_text<StreamEvent>(R"({"index":0,"text":"hi","type":"delta"})");
  REQUIRE(delta);
  CHECK(std::get<LenientDelta>(*delta).text == "hi");
  const auto stop = decode_text<StreamEvent>(R"({"index":0,"type":"stop"})");
  REQUIRE_FALSE(stop);
  CHECK(stop.error().model_message == R"($ contains unknown member "index")");
}

TEST_CASE("wire-like values borrow text and splice JSON verbatim") {
  const std::string model = "model-name";
  const WireBody body{
      .max_tokens = 1024,
      .model = model,
      .temperature = 0.7,
      .tools = std::vector<WireTool>{{
          .description = "Tab\there",
          .input_schema = {.text = R"({"type":"object", "properties":{}})"},
          .name = "lookup",
      }},
  };

  const auto direct = reflection::detail::encode_value(body);
  REQUIRE(direct);
  CHECK(direct->text ==
        R"({"max_tokens":1024,"model":"model-name","stream":true,"temperature":0.7,)"
        R"("tools":[{"description":"Tab\there","input_schema":{"type":"object", )"
        R"("properties":{}},"name":"lookup"}]})");

  const auto canonical = reflection::encode(body);
  REQUIRE(canonical);
  CHECK(canonical->text ==
        R"({"max_tokens":1024,"model":"model-name","stream":true,"temperature":0.7,)"
        R"("tools":[{"description":"Tab\there","input_schema":{"properties":{},)"
        R"("type":"object"},"name":"lookup"}]})");

  CHECK(reflection::encode(WireBody{.model = "m", .system = "s", .top_p = 0.5})->text ==
        R"({"model":"m","stream":true,"system":"s","temperature":0,"top_p":0.5})");
}

TEST_CASE("an invalid spliced JSON payload fails encoding at its path") {
  for (const auto* text : {"", "   ", "{", R"({"a":1} 2)", "nul"}) {
    const WireBody body{
        .model = "m",
        .tools = std::vector<WireTool>{{.input_schema = {.text = text}}},
    };
    const auto encoded = reflection::detail::encode_value(body);
    REQUIRE_FALSE(encoded);
    CHECK(encoded.error().category == scry::ErrorCategory::tool);
    CHECK(encoded.error().message ==
          "reflected JSON at $.tools[0].input_schema is not valid JSON");
    CHECK(encoded.error().model_message.empty());
  }
}

TEST_CASE("document-like values round trip byte for byte") {
  const auto text = std::string{
      R"({"messages":[{"content":[{"text":"hello","type":"text"}],"role":"user"},)"
      R"({"content":[{"text":"calling","type":"text"},{"arguments":{"city":"Oslo",)"
      R"("days":[1,2]},"id":"call-1","name":"forecast","type":"tool_call"}],)"
      R"("role":"assistant"},{"content":[{"is_error":false,"result":{"summary":)"
      R"("sunny","temperature_c":21.5},"tool_call_id":"call-1","type":"tool_result"}],)"
      R"("role":"user"}],"system_prompt":"Be brief.","version":1})"};

  const auto decoded = decode_text<Document>(text);
  REQUIRE(decoded);
  REQUIRE(decoded->messages.size() == 3);
  CHECK(decoded->messages[1].role == Role::assistant);
  const auto& call = std::get<DocToolCall>(decoded->messages[1].content[1]);
  CHECK(call.arguments.text == R"({"city":"Oslo","days":[1,2]})");
  const auto& result = std::get<DocToolResult>(decoded->messages[2].content[0]);
  CHECK(result.result.text == R"({"summary":"sunny","temperature_c":21.5})");

  const auto direct = reflection::detail::encode_value(*decoded);
  REQUIRE(direct);
  CHECK(direct->text == text);
  const auto canonical = reflection::encode(*decoded);
  REQUIRE(canonical);
  CHECK(canonical->text == text);
}

TEST_CASE("decoding a JSON member captures the canonical text of any value") {
  const auto decoded = decode_text<Payload>(R"({"body":{"b":[1, 2.50],"a":null}})");
  REQUIRE(decoded);
  CHECK(decoded->body.text == R"({"a":null,"b":[1,2.5]})");

  CHECK(decode_text<Payload>(R"({"body":null})")->body.text == "null");
  CHECK_FALSE(decode_text<Payload>("{}"));
  CHECK(reflection::encode(ConstView{.value = 4})->text == R"({"value":4})");
  CHECK(decode_text<Payload>(R"({"body":"text"})")->body.text == R"("text")");
  CHECK(decode_text<scry::Json>("[true]")->text == "[true]");
  CHECK_FALSE(decode_text<std::optional<scry::Json>>("null")->has_value());
  CHECK(decode_text<std::optional<scry::Json>>("{}")->value().text == "{}");
}

TEST_CASE("JsonView serializes the viewed node canonically") {
  const auto parsed = scry::JsonView::parse(
      scry::Json{.text = R"({"z":[1, 2],"a":{"d":null,"c":"x"}})"});
  REQUIRE(parsed);
  CHECK(parsed->to_json().text == R"({"a":{"c":"x","d":null},"z":[1,2]})");
  CHECK(parsed->find("a")->to_json().text == R"({"c":"x","d":null})");
  CHECK(parsed->find("z")->at(1)->to_json().text == "2");
  CHECK(scry::JsonView{}.to_json().text == "null");
}

TEST_CASE("public decoding reports invalid_argument with path-based messages") {
  auto decoded = decode_text<Renamed>("{");
  REQUIRE_FALSE(decoded);
  CHECK(decoded.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(decoded.error().message == "reflected JSON text is not valid JSON");
  CHECK(decoded.error().model_message == "JSON text is not valid JSON");

  decoded = decode_text<Renamed>("[]");
  REQUIRE_FALSE(decoded);
  CHECK(decoded.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(decoded.error().message == "reflected JSON at $ must be an object");
  CHECK(decoded.error().model_message == "$ must be an object");

  const auto view = scry::JsonView::parse(scry::Json{.text = R"({"a_first":"v"})"});
  REQUIRE(view);
  const auto from_view = reflection::decode<Renamed>(*view);
  REQUIRE(from_view);
  CHECK(from_view->zed == "v");
  CHECK(from_view->limit == 0U);

  CHECK(decode_text<std::vector<Role>>(R"(["user","assistant"])").value() ==
        std::vector<Role>{Role::user, Role::assistant});
  const auto role = decode_text<Role>(R"("system")");
  REQUIRE_FALSE(role);
  CHECK(role.error().model_message ==
        "$ is not a declared enumerator; must be one of: user, assistant");
}

TEST_CASE("floats encode as their shortest round-trip spelling") {
  CHECK(reflection::detail::encode_value(0.7)->text == "0.7");
  CHECK(reflection::detail::encode_value(0.7F)->text == "0.7");
  CHECK(reflection::detail::encode_value(1e20)->text == "1e+20");
  CHECK(reflection::detail::encode_value(0.1 + 0.2)->text == "0.30000000000000004");
  CHECK(reflection::encode(0.7)->text == "0.7");
  CHECK(reflection::encode(0.7F)->text == "0.7");
  CHECK(reflection::encode(1e20)->text == "1E20");

  // Tool results pass through the runtime's canonical writer, which prints the
  // double a float widens to: the shortest double spelling agrees with the direct
  // one, and a float result now reads as the float it is rather than as the
  // nine-digit double spelling of its value.
  struct Measured {
    float approximate{};
    double precise{};
  };
  auto handler = reflection::detail::make_tool_handler<Circle>([](Circle circle) {
    return Measured{.approximate = 0.7F, .precise = circle.radius + 0.6};
  });
  const auto tools = snapshot_of("measure", std::move(handler));
  const auto result = scry::detail::dispatch_tool(
      tools,
      scry::detail::ToolCallBlock{.id = "call-1",
                                  .name = "measure",
                                  .arguments = {.text = R"({"radius":0.1})"}},
      {}, 1024);
  REQUIRE(result);
  CHECK(result->result.text == R"({"approximate":0.7,"precise":0.7})");
  CHECK(reflection::detail::encode_value(Measured{.approximate = 0.7F, .precise = 0.7})
            ->text == result->result.text);
}

TEST_CASE("encoding reports a valueless variant instead of throwing") {
  struct ThrowingText {
    // NOLINTNEXTLINE(google-explicit-constructor): the conversion is the point.
    operator std::string() const { throw 1; }
  };
  DocBlock block{DocText{.text = "before"}};
  try {
    block.emplace<DocText>(ThrowingText{});
  } catch (const int) {
  }
  REQUIRE(block.valueless_by_exception());

  const auto encoded = reflection::encode(block);
  REQUIRE_FALSE(encoded);
  CHECK(encoded.error().message ==
        "reflected JSON at $ is a variant left valueless by an exception");
}
