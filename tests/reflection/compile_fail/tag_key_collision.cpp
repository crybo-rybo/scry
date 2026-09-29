#include <scry/reflection.hpp>
#include <string>
#include <variant>

// "type" carries a tagged alternative's discriminator, so no member may use it.
struct[[= scry::reflection::tag{"event"}]] Event {
  std::string type;
};

scry::Result<std::variant<Event>> decode_event(const scry::Json& json) {
  return scry::reflection::decode<std::variant<Event>>(json);
}
