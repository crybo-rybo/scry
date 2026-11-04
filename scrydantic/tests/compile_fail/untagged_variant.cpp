#include <scrydantic/codec.hpp>
#include <variant>

// Every alternative of a reflected variant needs a tag: the decoder dispatches on it.
struct Circle {
  double radius{};
};

struct[[= scrydantic::tag{"square"}]] Square {
  double side{};
};

struct Arguments {
  std::variant<Circle, Square> shape;
};

static_assert(!scrydantic::input_schema_v<Arguments>.empty());
