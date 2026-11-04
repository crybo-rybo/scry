#include <scry/scry.hpp>
#include <scrydantic/scrydantic.hpp>
#include <string>

#ifdef TESTING_SUPPORT
#include <scry/testing/scripted_server.hpp>
#include <scry/testing/streams.hpp>
#endif

namespace tools {
[[= scrydantic::tool{"Echo a label"}]] std::string echo(std::string label) {
  return label;
}
} // namespace tools

struct Answer {
  std::string label;
};

int main() {
  scry::ToolRegistry registry;
  if (!registry.add<^^tools>()) {
    return 1;
  }
  auto manifest = registry.to_json();
  if (!manifest || !scrydantic::JsonView::parse(*manifest)) {
    return 2;
  }
  auto bound = scrydantic::bind<^^tools>();
  auto echoed = bound.front().invoke(scrydantic::Json{.text = R"({"label":"ok"})"});
  if (!echoed || echoed->text != R"("ok")") {
    return 3;
  }
  auto format = scry::response_format<Answer>();
  if (!format.validate(scrydantic::Json{.text = R"({"label":"ok"})"}) ||
      format.validate(scrydantic::Json{.text = R"({"label":1})"})) {
    return 4;
  }
  auto conversation = scry::Conversation::create();
  if (!conversation) {
    return 5;
  }
  auto document = conversation->to_json();
  if (!document || !scry::Conversation::from_json(*document)) {
    return 6;
  }
#ifdef TESTING_SUPPORT
  auto server = scry::testing::ScriptedServer::create();
  if (!server) {
    return 7;
  }
  server->enqueue({.body_chunks = {scry::testing::anthropic_text_stream("ok")}});
#endif
  return 0;
}
