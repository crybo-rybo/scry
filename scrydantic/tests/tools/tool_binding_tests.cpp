// Tool binding without a host library: free functions, namespaces, toolboxes,
// and typed handlers become BoundTools whose handlers decode, call, and encode,
// with or without a call context of the caller's choosing.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// A host's call context. Scrydantic only passes it through.
struct Session {
  std::string user{};
};

struct Position {
  std::int32_t x{};
  std::int32_t y{};
};

} // namespace

namespace grid_tools {

[[= scrydantic::tool{"Move one tile east"}]] inline Position step_east(Position from) {
  return Position{.x = from.x + 1, .y = from.y};
}

[[ = scrydantic::tool{"Add two numbers"},
   = scrydantic::name{"sum"} ]] inline std::int32_t add(std::int32_t left,
                                                        std::int32_t right) {
  return left + right;
}

[[= scrydantic::tool{"Refuse every call"}]] inline scrydantic::Status refuse() {
  return std::unexpected(scrydantic::tool_error("not today", "refused a call"));
}

} // namespace grid_tools

namespace session_tools {

[[= scrydantic::tool{"Greet the session's user"}]] inline std::string
    greet(const Session& session, std::string greeting) {
  return greeting + ", " + session.user;
}

} // namespace session_tools

namespace {

class Counter {
public:
  [[= scrydantic::tool{"Add to the counter"}]] std::int64_t bump(std::int64_t by) {
    total_ += by;
    return total_;
  }

  [[= scrydantic::tool{"Read the counter for a user"}]] std::string
      read(const Session& session) const {
    return session.user + ":" + std::to_string(total_);
  }

  [[nodiscard]] std::int64_t total() const noexcept { return total_; }

private:
  std::int64_t total_{};
};

[[nodiscard]] scrydantic::Json json(std::string text) {
  return scrydantic::Json{.text = std::move(text)};
}

[[nodiscard]] std::vector<std::string>
names_of(const std::vector<scrydantic::BoundTool<>>& tools) {
  std::vector<std::string> names{};
  for (const auto& tool : tools) {
    names.push_back(tool.name);
  }
  return names;
}

} // namespace

static_assert(scrydantic::Toolbox<Counter, Session>);
// Without a context, read's Session parameter is not a context at all: a lone
// aggregate parameter is the argument object the model supplies.
static_assert(scrydantic::Toolbox<Counter>);
// bump is not const.
static_assert(!scrydantic::Toolbox<const Counter, Session>);

TEST_CASE("a namespace binds its tools in declaration order") {
  auto tools = scrydantic::bind<^^grid_tools>();

  REQUIRE(names_of(tools) == std::vector<std::string>{"step_east", "sum", "refuse"});
  CHECK(tools[0].description == "Move one tile east");
  CHECK(tools[0].input_schema.text == scrydantic::input_schema_v<Position>);
  CHECK(tools[1].input_schema.text ==
        R"({"additionalProperties":false,"properties":{"left":{"maximum":2147483647,)"
        R"("minimum":-2147483648,"type":"integer"},"right":{"maximum":2147483647,)"
        R"("minimum":-2147483648,"type":"integer"}},"required":["left","right"],)"
        R"("type":"object"})");
  CHECK(
      tools[2].input_schema.text ==
      R"({"additionalProperties":false,"properties":{},"required":[],"type":"object"})");

  const auto stepped = tools[0].invoke(json(R"({"x":1,"y":2})"));
  REQUIRE(stepped);
  CHECK(stepped->text == R"({"x":2,"y":2})");

  const auto sum = tools[1].invoke(json(R"({"left":2,"right":40})"));
  REQUIRE(sum);
  CHECK(sum->text == "42");
}

TEST_CASE("bound handlers report decode and handler failures as Errors") {
  auto tools = scrydantic::bind<^^grid_tools>();
  REQUIRE(tools.size() == 3);

  const auto missing = tools[1].invoke(json(R"({"left":2})"));
  REQUIRE_FALSE(missing);
  CHECK(missing.error().message == "reflected JSON at $.right is a required member");
  CHECK(missing.error().model_message == "$.right is a required member");

  const auto malformed = tools[1].invoke(json("{"));
  REQUIRE_FALSE(malformed);
  CHECK(malformed.error().message == "reflected tool arguments are not valid JSON");
  CHECK(malformed.error().model_message == "tool arguments are not valid JSON");

  const auto refused = tools[2].invoke(json("{}"));
  REQUIRE_FALSE(refused);
  CHECK(refused.error().message == "refused a call");
  CHECK(refused.error().model_message == "not today");
}

TEST_CASE("one function binds alone") {
  auto tools = scrydantic::bind<^^grid_tools::add>();
  REQUIRE(tools.size() == 1);
  CHECK(tools.front().name == "sum");
  CHECK(tools.front().description == "Add two numbers");
}

TEST_CASE("a context reaches tools that take it first and is not an argument") {
  auto tools = scrydantic::bind<^^session_tools, Session>();
  REQUIRE(tools.size() == 1);
  // The context parameter is not part of the argument object.
  CHECK(tools.front().input_schema.text ==
        R"({"additionalProperties":false,"properties":{"greeting":{"type":"string"}},)"
        R"("required":["greeting"],"type":"object"})");

  const Session session{.user = "ada"};
  const auto greeted = tools.front().invoke(session, json(R"({"greeting":"hello"})"));
  REQUIRE(greeted);
  CHECK(greeted->text == R"("hello, ada")");
}

TEST_CASE("a shared toolbox binds its member tools to the object") {
  auto counter = std::make_shared<Counter>();
  auto tools = scrydantic::bind<Session>(counter);

  REQUIRE(tools.size() == 2);
  CHECK(tools[0].name == "bump");
  CHECK(tools[1].name == "read");

  const Session session{.user = "bo"};
  REQUIRE(tools[0].invoke(session, json(R"({"by":5})")));
  REQUIRE(tools[0].invoke(session, json(R"({"by":2})")));
  CHECK(counter->total() == 7);

  const auto read = tools[1].invoke(session, json("{}"));
  REQUIRE(read);
  CHECK(read->text == R"("bo:7")");

  // The handlers share the toolbox, so it outlives the host's pointer.
  counter.reset();
  const auto again = tools[1].invoke(session, json("{}"));
  REQUIRE(again);
  CHECK(again->text == R"("bo:7")");

  CHECK(scrydantic::bind<Session>(std::shared_ptr<Counter>{}).empty());
}

TEST_CASE("typed handlers bind with and without a context") {
  auto plain = scrydantic::bind_handler<Position>(
      [](Position position) { return position.x * position.y; });
  const auto product = plain(json(R"({"x":6,"y":7})"));
  REQUIRE(product);
  CHECK(product->text == "42");

  auto acknowledging = scrydantic::bind_handler<Position, Session>(
      [](const Session& session, Position) -> scrydantic::Status {
        if (session.user.empty()) {
          return std::unexpected(scrydantic::tool_error("sign in first"));
        }
        return {};
      });
  const auto accepted = acknowledging(Session{.user = "cy"}, json(R"({"x":0,"y":0})"));
  REQUIRE(accepted);
  CHECK(accepted->text == "{}");
  const auto rejected = acknowledging(Session{}, json(R"({"x":0,"y":0})"));
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().model_message == "sign in first");
  CHECK(rejected.error().message == "sign in first");
}

TEST_CASE("descriptors describe tools at compile time without handlers") {
  static constexpr auto grid = scrydantic::describe<^^grid_tools>();
  static_assert(grid.size() == 3);
  static_assert(grid[1].name == "sum");
  static_assert(grid[1].description == "Add two numbers");

  static constexpr auto counter = scrydantic::describe<Counter, Session>();
  static_assert(counter.size() == 2);
  static_assert(counter[1].input_schema ==
                R"({"additionalProperties":false,"properties":{},"required":[],)"
                R"("type":"object"})");
  // Without the context, read takes a Session from the model instead.
  static constexpr auto contextless = scrydantic::describe<Counter>();
  static_assert(contextless[1].input_schema == scrydantic::input_schema_v<Session>);

  const auto bound = scrydantic::bind<^^grid_tools>();
  REQUIRE(bound.size() == grid.size());
  for (std::size_t index = 0; index < grid.size(); ++index) {
    CHECK(bound[index].name == grid[index].name);
    CHECK(bound[index].description == grid[index].description);
    CHECK(bound[index].input_schema.text == grid[index].input_schema);
  }
}
