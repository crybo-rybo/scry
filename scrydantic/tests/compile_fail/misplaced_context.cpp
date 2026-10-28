#include <scrydantic/tools.hpp>
#include <string>

struct Session {
  std::string user;
};

namespace greeting_tools {
// The context is only ever the leading parameter.
[[= scrydantic::tool{"Greet someone"}]] inline std::string
    greet(std::string greeting, const Session& session) {
  return greeting + session.user;
}
} // namespace greeting_tools

auto bind_tools() { return scrydantic::bind<^^greeting_tools, Session>(); }
