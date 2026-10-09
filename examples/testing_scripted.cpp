// A complete downstream test with no test framework: the exit code is the
// result. This is the shape a consumer's CI can copy — script the provider,
// register the tools the application registers, run the turn, then assert on
// what the application observed and on the request bytes Scry actually sent.
//
// The provider is a scripted HTTP server on 127.0.0.1, and the Harness comes from
// the ordinary Harness::create. Thus everything between the script and the
// assertions is the shipping runtime: libcurl, the dialect's request encoder and
// stream decoder, tool dispatch, and the pump. Nothing leaves the machine.

#include <cstdio>
#include <scry/scry.hpp>
#include <scry/testing/scripted_server.hpp>
#include <scry/testing/streams.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace {

/// Arguments of the tool the application under test registers.
struct RoomArguments {
  std::string room{};
};

[[nodiscard]] scry::Config
scripted_config(const scry::testing::ScriptedServer& server) {
  auto config = scry::Config{
      .base_url = server.url(),
      .api_key = "example-key",
      .model = "example-model",
  };
  config.retry.max_attempts = 1;
  return config;
}

[[nodiscard]] bool check(const bool condition, const std::string_view what) {
  if (!condition) {
    std::fprintf(stderr, "scripted test failed: %.*s\n", static_cast<int>(what.size()),
                 what.data());
  }
  return condition;
}

// Two rounds: the model asks for the tool, then answers using its result.
void script_two_rounds(scry::testing::ScriptedServer& server) {
  server.enqueue({
      .body_chunks = {scry::testing::anthropic_tool_stream({
          {.id = "call-1", .name = "read_sensor", .arguments = R"({"room":"attic"})"},
      })},
  });
  server.enqueue({
      .body_chunks = {scry::testing::anthropic_text_stream("The attic is at 19C.")},
  });
}

} // namespace

int main() {
  auto started = scry::testing::ScriptedServer::create();
  if (!check(started.has_value(), "the scripted server was started")) {
    return 1;
  }
  auto server = std::move(*started);
  script_two_rounds(server);

  auto created = scry::Harness::create(scripted_config(server));
  if (!check(created.has_value(), "the harness was created")) {
    return 1;
  }
  auto harness = std::move(*created);

  auto conversation = scry::Conversation::create();
  if (!check(conversation.has_value(), "a conversation was created")) {
    return 1;
  }

  std::string requested_room;
  const auto registered = harness.tools().add<RoomArguments>(
      {
          .name = "read_sensor",
          .description = "Read the temperature of one room",
      },
      [&requested_room](RoomArguments arguments) {
        requested_room = arguments.room;
        return std::string{"19C"};
      });
  if (!check(registered.has_value(), "the reflected tool was registered")) {
    return 1;
  }

  // The callbacks a host would attach, delivered on this thread by update().
  std::string streamed;
  std::string observed_tool;
  auto outcome = scry::Result<scry::Completion>{std::unexpect, scry::Error{}};
  bool finished = false;
  auto turn = harness.send(
      *conversation, "How warm is the attic?",
      {
          .on_text_delta =
              [&streamed](const std::string_view text) { streamed.append(text); },
          .on_tool_call =
              [&observed_tool](const scry::ToolCall& call) {
                observed_tool = call.name;
              },
          .on_finished =
              [&outcome, &finished](scry::Result<scry::Completion> result) {
                outcome = std::move(result);
                finished = true;
              },
      });
  if (!check(turn.has_value(), "the turn was accepted")) {
    return 1;
  }
  while (!finished) {
    static_cast<void>(harness.update());
  }
  if (!check(outcome.has_value(), "the scripted turn completed")) {
    return 1;
  }

  const auto requests = server.requests();
  const auto passed =
      check(outcome->text == "The attic is at 19C.", "the final text was decoded") &&
      check(streamed == "The attic is at 19C.", "the text arrived as deltas") &&
      check(requested_room == "attic", "the tool handler saw the decoded arguments") &&
      check(observed_tool == "read_sensor", "the tool call was observed") &&
      check(conversation->message_count() == 4, "the whole round trip committed") &&
      check(requests.size() == 2, "two requests were sent") &&
      check(requests.front().target == "/v1/messages",
            "the requests went to the Anthropic endpoint") &&
      check(requests.back().body.find("19C") != std::string::npos,
            "the tool result reached the second request") &&
      check(requests.front().body.find("example-key") == std::string::npos,
            "the API key stayed out of the request body");
  return passed ? 0 : 1;
}
