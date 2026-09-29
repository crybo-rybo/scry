#include <scry/reflection.hpp>
#include <variant>

// Two alternatives under one tag would make decoding ambiguous.
struct[[= scry::reflection::tag{"shape"}]] Circle {
  double radius{};
};

struct[[= scry::reflection::tag{"shape"}]] Square {
  double side{};
};

static_assert(!scry::reflection::schema_v<std::variant<Circle, Square>>.empty());
