// Scrydantic without Scry: annotate a few functions and a toolbox, publish them
// as an MCP-style `tools/list` result, and serve one `tools/call` request. The
// program links Scrydantic alone, with no HTTP client, runtime, or model, and
// exits 0 only when what it printed is what it expects.

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace unit_tools {

enum class Scale : std::uint8_t {
  celsius,
  fahrenheit,
};

struct Reading {
  [[= scrydantic::description{"Temperature in degrees"}]] double degrees{};
  Scale scale{Scale::celsius};
};

[[= scrydantic::tool{"Convert a temperature to the other scale"}]] inline Reading
    convert(Reading reading) {
  if (reading.scale == Scale::celsius) {
    return Reading{.degrees = reading.degrees * 9.0 / 5.0 + 32.0,
                   .scale = Scale::fahrenheit};
  }
  return Reading{.degrees = (reading.degrees - 32.0) * 5.0 / 9.0,
                 .scale = Scale::celsius};
}

[[= scrydantic::tool{"Round a number to the nearest whole number"}]] inline std::int64_t
    round_number(double value) {
  return static_cast<std::int64_t>(value < 0 ? value - 0.5 : value + 0.5);
}

} // namespace unit_tools

// A toolbox: its annotated member functions become tools bound to one object.
class Notebook {
public:
  [[
    = scrydantic::tool{"Write a note"}, = scrydantic::name{"write_note"}
  ]] scrydantic::Status write(std::string text) {
    if (text.empty()) {
      return std::unexpected(scrydantic::tool_error("a note needs text"));
    }
    notes_.push_back(std::move(text));
    return {};
  }

  [[= scrydantic::tool{"Count the notes written so far"}]] std::size_t
      count_notes() const {
    return notes_.size();
  }

private:
  std::vector<std::string> notes_{};
};

namespace {

// The MCP shapes, declared as reflected values so Scrydantic writes and reads
// them. Keys that are not C++ identifiers in MCP's spelling get a name annotation.
struct ListedTool {
  std::string_view name;
  std::string_view description;
  [[= scrydantic::name{"inputSchema"}]] scrydantic::Json input_schema;
};

struct ToolList {
  std::vector<ListedTool> tools;
};

struct CallRequest {
  std::string name;
  scrydantic::Json arguments;
};

struct[[= scrydantic::tag{"text"}]] TextContent {
  std::string text;
};

struct CallResult {
  std::vector<std::variant<TextContent>> content;
  [[= scrydantic::name{"isError"}]] bool is_error{};
};

[[nodiscard]] CallResult call(std::vector<scrydantic::BoundTool<>>& tools,
                              const scrydantic::Json& request_text) {
  auto request = scrydantic::decode<CallRequest>(request_text);
  if (!request) {
    return {.content = {TextContent{request.error().model_message}}, .is_error = true};
  }
  for (auto& tool : tools) {
    if (tool.name == request->name) {
      auto result = tool.invoke(std::move(request->arguments));
      if (!result) {
        return {.content = {TextContent{result.error().model_message}},
                .is_error = true};
      }
      return {.content = {TextContent{std::move(result->text)}}};
    }
  }
  return {.content = {TextContent{"unknown tool " + request->name}}, .is_error = true};
}

} // namespace

int main() {
  auto notebook = std::make_shared<Notebook>();
  auto tools = scrydantic::bind<^^unit_tools>();
  for (auto& tool : scrydantic::bind(notebook)) {
    tools.push_back(std::move(tool));
  }

  ToolList listing{};
  for (const auto& tool : tools) {
    listing.tools.push_back(ListedTool{
        .name = tool.name,
        .description = tool.description,
        .input_schema = tool.input_schema,
    });
  }
  const auto listed = scrydantic::encode(listing);
  if (!listed) {
    std::cerr << listed.error().message << '\n';
    return 1;
  }
  std::cout << "tools/list -> " << listed->text << '\n';

  const scrydantic::Json request{
      .text = R"({"name":"convert","arguments":{"degrees":100,"scale":"celsius"}})"};
  const auto answered = scrydantic::encode(call(tools, request));
  const auto refused = scrydantic::encode(
      call(tools, scrydantic::Json{.text = R"({"name":"write_note","arguments":{}})"}));
  if (!answered || !refused) {
    return 1;
  }
  std::cout << "tools/call -> " << answered->text << '\n';
  std::cout << "tools/call -> " << refused->text << '\n';

  // What a client would check: the four tools in declaration order, a decoded and
  // converted reading, and a schema-derived refusal the model can act on.
  const auto parsed = scrydantic::JsonView::parse(*listed);
  const auto names = parsed ? parsed->find("tools") : std::nullopt;
  std::vector<std::string_view> listed_names{};
  for (std::size_t index = 0; names && index < names->size(); ++index) {
    listed_names.push_back(names->at(index)->find("name")->string().value_or(""));
  }
  const bool ok =
      listed_names == std::vector<std::string_view>{"convert", "round_number",
                                                    "write_note", "count_notes"} &&
      answered->text ==
          R"({"content":[{"text":"{\"degrees\":212,\"scale\":\"fahrenheit\"}",)"
          R"("type":"text"}],"isError":false})" &&
      refused->text == R"({"content":[{"text":"$.text is a required member",)"
                       R"("type":"text"}],"isError":true})";
  return ok ? 0 : 1;
}
