#include <scrydantic/codec.hpp>

struct Arguments {
  [[= scrydantic::description{"First description"}]]
      [[= scrydantic::description{"Duplicate description"}]] int value{};
};

static_assert(!scrydantic::input_schema_v<Arguments>.empty());
