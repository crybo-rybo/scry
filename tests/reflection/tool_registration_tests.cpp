#include "runtime/tool_registry_impl.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <scry/tool_registry.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Registration reflects over these declarations by name, so they live in named
// namespaces rather than in an anonymous one.
namespace registration {

struct Position {
  std::int32_t x{};
  std::int32_t y{};
};

struct StrideArguments {
  [[= scry::reflection::description{"Tiles to move east"}]] std::int32_t tiles{};
};

// Every form a free tool function can take.
namespace grid {

// No parameters: the tool takes {} and nothing else.
[[= scry::reflection::tool{"Report the origin"}]] inline Position origin() {
  return {};
}

// Plain parameters: an argument object is synthesized from them.
[[= scry::reflection::tool{"Offset the origin"}]] inline Position
offset(std::int32_t dx, const std::int32_t& dy) {
  return {.x = dx, .y = dy};
}

// The context, then one aggregate: exactly like add<StrideArguments>(), and named
// by annotation rather than by the function.
[[
  = scry::reflection::tool{"Stride east"},
  = scry::reflection::name{"stride_east"}
]] inline Position
stride(const scry::ToolCallContext& context, StrideArguments arguments) {
  return {.x = arguments.tiles, .y = static_cast<std::int32_t>(context.round)};
}

// Not a tool: namespace registration skips it.
inline Position unannotated() { return {}; }

} // namespace grid

// The context followed by synthesized parameters.
[[= scry::reflection::tool{"Label the call"}]] inline std::string
label(const scry::ToolCallContext& context, std::string prefix) {
  return prefix + ":" + std::string{context.tool_name};
}

// Status acknowledges with {} or refuses with the handler's error.
[[= scry::reflection::tool{"Accept only positive numbers"}]] inline scry::Status
require_positive(std::int32_t value) {
  if (value <= 0) {
    return std::unexpected(scry::tool_error("value must be positive"));
  }
  return {};
}

class Counter {
public:
  [[= scry::reflection::tool{"Add to the counter"}]] std::int32_t
  increment(std::int32_t by) {
    value_ += by;
    return value_;
  }

  [[= scry::reflection::tool{"Read the counter"}]] std::int32_t read() const {
    return value_;
  }

  [[= scry::reflection::tool{"Reset the counter"}]] void reset() { value_ = 0; }

  [[= scry::reflection::tool{"Name the call being serviced"}]] std::string
  whoami(const scry::ToolCallContext& context) const {
    return std::string{context.tool_name};
  }

  [[= scry::reflection::tool{"Count one tool call"}]] static std::int32_t one() {
    return 1;
  }

  // Not a tool, and callable by the host.
  [[nodiscard]] std::int32_t value() const noexcept { return value_; }

private:
  std::int32_t value_{};
};

// Only const tools, so it registers as a const toolbox too.
struct Reader {
  std::int32_t fixed{};

  [[= scry::reflection::tool{"Read the fixed value"}]] std::int32_t peek() const {
    return fixed;
  }
};

// Its third tool is named like one of Counter's.
struct Colliding {
  [[= scry::reflection::tool{"First"}]] std::int32_t alpha() const { return 1; }
  [[= scry::reflection::tool{"Second"}]] std::int32_t beta() const { return 2; }
  [[ = scry::reflection::tool{"Third"},
     = scry::reflection::name{"reset"} ]] std::int32_t
  gamma() const {
    return 3;
  }
};

namespace colliding_grid {
[[= scry::reflection::tool{"First"}]] inline std::int32_t first() { return 1; }
[[= scry::reflection::tool{"Second"}]] inline std::int32_t second() { return 2; }
[[= scry::reflection::tool{"Third"}]] inline std::int32_t origin() { return 3; }
} // namespace colliding_grid

} // namespace registration

namespace {

// Invokes a registered tool the way dispatch does, naming the call it stands in
// for. The snapshot is released on return, so it never extends a toolbox's life.
[[nodiscard]] scry::Result<scry::Json>
call(scry::ToolRegistry& registry, const std::string_view name, std::string arguments) {
  const auto snapshot = scry::detail::ToolRegistryAccess::snapshot(registry).entries;
  const auto* tool = scry::detail::find_tool(*snapshot, name);
  REQUIRE(tool != nullptr);
  return tool->handler(
      scry::ToolCallContext{
          .call_id = "call-1",
          .tool_name = tool->definition.name,
          .round = 2,
      },
      scry::Json{.text = std::move(arguments)});
}

[[nodiscard]] std::string schema_of(scry::ToolRegistry& registry,
                                    const std::string_view name) {
  const auto snapshot = scry::detail::ToolRegistryAccess::snapshot(registry).entries;
  const auto* tool = scry::detail::find_tool(*snapshot, name);
  REQUIRE(tool != nullptr);
  return tool->definition.input_schema.text;
}

constexpr std::string_view empty_object_schema =
    R"({"additionalProperties":false,"properties":{},"required":[],"type":"object"})";
constexpr std::string_view int32_schema =
    R"({"maximum":2147483647,"minimum":-2147483648,"type":"integer"})";

} // namespace

static_assert(scry::reflection::Toolbox<registration::Counter>);
static_assert(scry::reflection::Toolbox<registration::Reader>);
static_assert(scry::reflection::Toolbox<const registration::Reader>);
static_assert(!scry::reflection::Toolbox<const registration::Counter>);
static_assert(!scry::reflection::Toolbox<registration::Position>);
static_assert(!scry::reflection::Toolbox<std::int32_t>);

TEST_CASE("a function without parameters takes the empty object only") {
  scry::ToolRegistry registry;
  REQUIRE(registry.add<^^registration::grid::origin>());
  CHECK(registry.names() == std::vector<std::string>{"origin"});
  CHECK(schema_of(registry, "origin") == empty_object_schema);

  const auto result = call(registry, "origin", "{}");
  REQUIRE(result);
  CHECK(result->text == R"({"x":0,"y":0})");

  const auto rejected = call(registry, "origin", R"({"x":1})");
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().model_message == R"($ contains unknown member "x")");
}

TEST_CASE("plain parameters are synthesized into one required argument object") {
  scry::ToolRegistry registry;
  REQUIRE(registry.add<^^registration::grid::offset>());
  CHECK(schema_of(registry, "offset") ==
        std::string{R"({"additionalProperties":false,"properties":{"dx":)"} +
            std::string{int32_schema} + R"(,"dy":)" + std::string{int32_schema} +
            R"(},"required":["dx","dy"],"type":"object"})");

  const auto result = call(registry, "offset", R"({"dx":3,"dy":-4})");
  REQUIRE(result);
  CHECK(result->text == R"({"x":3,"y":-4})");

  const auto missing = call(registry, "offset", R"({"dx":3})");
  REQUIRE_FALSE(missing);
  CHECK(missing.error().model_message == "$.dy is a required member");
}

TEST_CASE("a context parameter precedes an aggregate or synthesized arguments") {
  scry::ToolRegistry registry;
  REQUIRE(registry.add<^^registration::grid::stride>());
  REQUIRE(registry.add<^^registration::label>());
  CHECK(registry.names() == std::vector<std::string>{"stride_east", "label"});

  // The aggregate form keeps its member descriptions, exactly as add<Args>() does.
  CHECK(schema_of(registry, "stride_east") ==
        scry::reflection::input_schema_v<registration::StrideArguments>);
  auto result = call(registry, "stride_east", R"({"tiles":5})");
  REQUIRE(result);
  CHECK(result->text == R"({"x":5,"y":2})");

  CHECK(
      schema_of(registry, "label") ==
      R"({"additionalProperties":false,"properties":{"prefix":{"type":"string"}},"required":["prefix"],"type":"object"})");
  result = call(registry, "label", R"({"prefix":"tool"})");
  REQUIRE(result);
  CHECK(result->text == R"("tool:label")");
}

TEST_CASE("a namespace registers every annotated function in declaration order") {
  scry::ToolRegistry registry;
  REQUIRE(registry.add<^^registration::grid>());
  CHECK(registry.names() ==
        std::vector<std::string>{"origin", "offset", "stride_east"});
  CHECK_FALSE(registry.contains("unannotated"));
}

TEST_CASE("Status and void tools acknowledge with an empty object") {
  scry::ToolRegistry registry;
  REQUIRE(registry.add<^^registration::require_positive>());

  auto result = call(registry, "require_positive", R"({"value":1})");
  REQUIRE(result);
  CHECK(result->text == "{}");

  result = call(registry, "require_positive", R"({"value":0})");
  REQUIRE_FALSE(result);
  CHECK(result.error().model_message == "value must be positive");
}

TEST_CASE("a shared toolbox binds every tool to the object the host still holds") {
  auto counter = std::make_shared<registration::Counter>();
  std::weak_ptr<registration::Counter> observer = counter;
  {
    scry::ToolRegistry registry;
    REQUIRE(registry.add(counter));
    CHECK(registry.names() ==
          std::vector<std::string>{"increment", "read", "reset", "whoami", "one"});

    REQUIRE(call(registry, "increment", R"({"by":2})"));
    const auto incremented = call(registry, "increment", R"({"by":3})");
    REQUIRE(incremented);
    CHECK(incremented->text == "5");
    CHECK(counter->value() == 5);

    const auto whoami = call(registry, "whoami", "{}");
    REQUIRE(whoami);
    CHECK(whoami->text == R"("whoami")");
    const auto one = call(registry, "one", "{}");
    REQUIRE(one);
    CHECK(one->text == "1");

    const auto reset = call(registry, "reset", "{}");
    REQUIRE(reset);
    CHECK(reset->text == "{}");
    CHECK(counter->value() == 0);

    // The registrations share ownership: the toolbox outlives the host's handle.
    counter.reset();
    CHECK_FALSE(observer.expired());
    REQUIRE(call(registry, "increment", R"({"by":4})"));
    const auto read = call(registry, "read", "{}");
    REQUIRE(read);
    CHECK(read->text == "4");
  }
  CHECK(observer.expired());
}

TEST_CASE("an owned toolbox is moved in and keeps its state across its tools") {
  registration::Counter counter;
  static_cast<void>(counter.increment(10));

  scry::ToolRegistry registry;
  REQUIRE(registry.add(std::move(counter)));
  REQUIRE(call(registry, "increment", R"({"by":1})"));
  const auto read = call(registry, "read", "{}");
  REQUIRE(read);
  CHECK(read->text == "11");
}

TEST_CASE("a const toolbox registers its const tools") {
  scry::ToolRegistry registry;
  REQUIRE(registry.add(
      std::make_shared<const registration::Reader>(registration::Reader{.fixed = 7})));
  const auto read = call(registry, "peek", "{}");
  REQUIRE(read);
  CHECK(read->text == "7");
}

TEST_CASE("a null toolbox is rejected") {
  scry::ToolRegistry registry;
  const auto status = registry.add(std::shared_ptr<registration::Counter>{});
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(registry.empty());
}

TEST_CASE("a toolbox that collides on its third tool registers none of them") {
  scry::ToolRegistry registry;
  REQUIRE(registry.add(std::make_shared<registration::Counter>()));
  const auto before = registry.names();

  const auto status = registry.add(registration::Colliding{});
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(status.error().message == R"(a tool named "reset" is already registered)");
  CHECK(registry.names() == before);
  CHECK_FALSE(registry.contains("alpha"));
  CHECK_FALSE(registry.contains("beta"));
}

TEST_CASE("a namespace that collides on its third tool registers none of them") {
  scry::ToolRegistry registry;
  REQUIRE(registry.add<^^registration::grid::origin>());

  const auto status = registry.add<^^registration::colliding_grid>();
  REQUIRE_FALSE(status);
  CHECK(status.error().message == R"(a tool named "origin" is already registered)");
  CHECK(registry.names() == std::vector<std::string>{"origin"});
}

TEST_CASE("a second object of one toolbox class is a duplicate registration") {
  scry::ToolRegistry registry;
  REQUIRE(registry.add(registration::Reader{.fixed = 1}));
  const auto again = registry.add(registration::Reader{.fixed = 2});
  REQUIRE_FALSE(again);
  CHECK(again.error().category == scry::ErrorCategory::invalid_argument);

  const auto read = call(registry, "peek", "{}");
  REQUIRE(read);
  CHECK(read->text == "1");
}

TEST_CASE("the manifest shows the schemas generated for every registration form") {
  scry::ToolRegistry registry;
  REQUIRE(registry.add<^^registration::grid>());
  REQUIRE(registry.add(registration::Reader{}));

  const auto manifest = registry.to_json();
  REQUIRE(manifest);
  CHECK(
      manifest->text ==
      std::string{R"({"tools":[)"} +
          R"({"description":"Report the origin","input_schema":)" +
          std::string{empty_object_schema} + R"(,"name":"origin"},)" +
          R"({"description":"Offset the origin","input_schema":{"additionalProperties":false,"properties":{"dx":)" +
          std::string{int32_schema} + R"(,"dy":)" + std::string{int32_schema} +
          R"(},"required":["dx","dy"],"type":"object"},"name":"offset"},)" +
          R"({"description":"Stride east","input_schema":{"additionalProperties":false,"properties":{"tiles":{"description":"Tiles to move east",)" +
          std::string{int32_schema}.substr(1) +
          R"(},"required":[],"type":"object"},"name":"stride_east"},)" +
          R"({"description":"Read the fixed value","input_schema":)" +
          std::string{empty_object_schema} + R"(,"name":"peek"}],"version":1})");
}
