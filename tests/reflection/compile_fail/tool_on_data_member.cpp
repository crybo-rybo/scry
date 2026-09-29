#include <scry/reflection.hpp>
#include <string>

// tool describes functions. On a member of an argument aggregate it is almost
// certainly a description written with the wrong annotation.
struct Arguments {
  [[= scry::reflection::tool{"City to query"}]] std::string city;
};

constexpr auto schema = scry::reflection::input_schema_v<Arguments>;
