#include <scry/reflection.hpp>

struct Arguments {
  [[= scry::reflection::description{"First description"}]]
      [[= scry::reflection::description{"Duplicate description"}]] int value{};
};

static_assert(!scry::reflection::input_schema_v<Arguments>.empty());
