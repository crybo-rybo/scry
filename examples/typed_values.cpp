#include <iostream>
#include <optional>
#include <scry/scry.hpp>
#include <string>
#include <variant>
#include <vector>

// Scry's reflected codec on its own: no model, no Harness. One annotated type
// yields its JSON Schema, a strict decoder with model-readable errors, and a
// canonical encoder.
namespace {

enum class Priority {
  low,
  high,
};

// A tagged variant: each alternative names its "type" value.
struct[[= scry::reflection::tag{"reminder"}]] Reminder {
  [[= scry::reflection::description{"What to remind the user of"}]] std::string text;
  Priority priority{Priority::low};
};

struct[[= scry::reflection::tag{"note"}]] Note {
  [[= scry::reflection::name{"body"}]] std::string text;
};

using Action = std::variant<Reminder, Note>;

// skip_null leaves a disengaged optional out of the output instead of writing null.
struct[[= scry::reflection::skip_null]] Plan {
  std::vector<Action> actions;
  std::optional<std::string> summary;
};

void print(const char* label, const scry::Result<scry::Json>& json) {
  std::cout << label << ": " << (json ? json->text : json.error().message) << '\n';
}

} // namespace

int main() {
  // The schema a host would hand a model as the expected answer shape.
  std::cout << "schema: " << scry::reflection::schema_v<Plan> << '\n';

  const auto answer = scry::reflection::decode<Plan>(scry::Json{
      .text = R"({"actions":[{"type":"reminder","text":"stretch","priority":"high"},)"
              R"({"type":"note","body":"water the plants"}]})"});
  if (!answer) {
    std::cerr << answer.error().message << '\n';
    return 1;
  }
  print("round trip", scry::reflection::encode(*answer));

  // A wrong answer fails with the JSON path and what the schema required there;
  // model_message is written to be sent back to the model as-is.
  const auto wrong = scry::reflection::decode<Plan>(
      scry::Json{.text = R"({"actions":[{"type":"alarm","text":"now"}]})"});
  if (wrong) {
    return 1;
  }
  std::cout << "rejected: " << wrong.error().model_message << '\n';
  return 0;
}
