#include <iostream>
#include <optional>
#include <scrydantic/scrydantic.hpp>
#include <string>
#include <variant>
#include <vector>

// Scrydantic on its own: no Scry header, no model, no Harness. Scry links
// Scrydantic publicly, so a program that links scry::scry can use it for its own
// JSON too. One annotated type yields its JSON Schema, a strict decoder with
// model-readable errors, and a canonical encoder. A tool's arguments and a typed
// answer go through exactly this, so this is also how to see what a model will be
// sent and what it will be told when it gets a value wrong.
namespace {

enum class Priority {
  low,
  high,
};

// A tagged variant: each alternative names its "type" value.
struct[[= scrydantic::tag{"reminder"}]] Reminder {
  [[
    = scrydantic::description{"What to remind the user of"},
    = scrydantic::length{.min = 1}
  ]] std::string text;
  Priority priority{Priority::low};
};

struct[[= scrydantic::tag{"note"}]] Note {
  [[= scrydantic::name{"body"}]] std::string text;
};

using Action = std::variant<Reminder, Note>;

// skip_null leaves a disengaged optional out of the output instead of writing null.
struct[[= scrydantic::skip_null]] Plan {
  [[= scrydantic::items{.max = 5}]] std::vector<Action> actions;
  std::optional<std::string> summary;
};

void print(const char* label, const scrydantic::Result<scrydantic::Json>& json) {
  std::cout << label << ": " << (json ? json->text : json.error().message) << '\n';
}

} // namespace

int main() {
  // The schema a host would hand a model as the expected answer shape.
  std::cout << "schema: " << scrydantic::schema_v<Plan> << '\n';

  const auto answer = scrydantic::decode<Plan>(scrydantic::Json{
      .text = R"({"actions":[{"type":"reminder","text":"stretch","priority":"high"},)"
              R"({"type":"note","body":"water the plants"}]})"});
  if (!answer) {
    std::cerr << answer.error().message << '\n';
    return 1;
  }
  print("round trip", scrydantic::encode(*answer));

  // A wrong answer fails with one line for each issue: its JSON path and what the
  // schema required there. model_message is written to be sent back to the model
  // as-is, and issues holds the same problems as data.
  const auto wrong = scrydantic::decode<Plan>(scrydantic::Json{
      .text =
          R"({"actions":[{"type":"alarm","text":"now"},{"type":"reminder","text":""}]})"});
  if (wrong) {
    return 1;
  }
  std::cout << "rejected:\n" << wrong.error().model_message << '\n';
  for (const auto& issue : wrong.error().issues) {
    std::cout << "issue at " << scrydantic::json_path(issue.path) << '\n';
  }
  return 0;
}
