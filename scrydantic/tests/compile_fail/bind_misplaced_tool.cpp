#include <scrydantic/tools.hpp>

namespace lamp_tools {
[[= scrydantic::tool{"Switch the lamp on"}]] inline bool switch_on() { return true; }

// The annotation applies to functions; a variable carrying it is a mistake the
// namespace walk reports rather than skips.
[[= scrydantic::tool{"Brightness"}]] inline int brightness = 3;
} // namespace lamp_tools

auto bind_tools() { return scrydantic::bind<^^lamp_tools>(); }
