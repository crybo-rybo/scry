// Tests an application without a model. scry::testing is a scripted HTTP server on
// 127.0.0.1, so the whole of Scry runs: only the replies are canned.

#include <iostream>
#include <scry/scry.hpp>
#include <scry/testing/scripted_server.hpp>
#include <scry/testing/streams.hpp>
#include <string>
#include <utility>

[[= scrydantic::tool{"Get the current weather in a city"}]] std::string
    weather(std::string city) {
  return city + ": 18C and sunny";
}

int main() {
  auto server = scry::testing::ScriptedServer::create();
  if (!server) {
    std::cerr << server.error().message << '\n';
    return 1;
  }
  // One reply per request: first the model calls the tool, then it answers.
  server->enqueue(
      {.body_chunks = {scry::testing::anthropic_tool_stream(
           {{.id = "call-1", .name = "weather", .arguments = R"({"city":"Oslo"})"}})}});
  server->enqueue(
      {.body_chunks = {scry::testing::anthropic_text_stream("No umbrella needed.")}});

  scry::ToolRegistry tools;
  const auto added = tools.add<^^weather>();
  auto harness = scry::Harness::create(
      {.base_url = server->url(), .api_key = "test-key", .model = "test-model"},
      std::move(tools));
  auto conversation = scry::Conversation::create();
  if (!added || !harness || !conversation) {
    std::cerr << "setup failed\n";
    return 1;
  }

  const auto reply = harness->send_and_wait(*conversation, "Umbrella today?");
  const auto requests = server->requests();
  const bool passed = reply && reply->text == "No umbrella needed." &&
                      reply->tool_call_count == 1 && requests.size() == 2 &&
                      requests[1].body.contains("Oslo: 18C and sunny");
  std::cout << (passed ? "passed" : "failed") << '\n';
  return passed ? 0 : 1;
}
