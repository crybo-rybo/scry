#include <array>
#include <scry/reflection.hpp>

struct Arguments {
  int value{};
};

template <> struct scry::reflection::tool_traits<Arguments> {
  static constexpr std::array descriptions{
      scry::reflection::parameter_description{"unknown", "Unknown member"},
  };
};

static_assert(!scry::reflection::input_schema_v<Arguments>.empty());
