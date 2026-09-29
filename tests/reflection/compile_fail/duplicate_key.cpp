#include <scry/reflection.hpp>

// A name annotation that lands on another member's key would write one key twice.
struct Limits {
  [[= scry::reflection::name{"max_tokens"}]] int limit{};
  int max_tokens{};
};

scry::Result<scry::Json> encode_limits(const Limits& limits) {
  return scry::reflection::encode(limits);
}
