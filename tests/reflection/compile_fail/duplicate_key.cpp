#include <scrydantic/codec.hpp>

// A name annotation that lands on another member's key would write one key twice.
struct Limits {
  [[= scrydantic::name{"max_tokens"}]] int limit{};
  int max_tokens{};
};

scrydantic::Result<scrydantic::Json> encode_limits(const Limits& limits) {
  return scrydantic::encode(limits);
}
