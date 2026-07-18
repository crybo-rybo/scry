#include <glaze/glaze.hpp>
#include <scry/reflection.hpp>
#include <string>

namespace {

struct ForecastArgs {
  std::string city{};
  int days{3};
};

static_assert(glz::has_reflection26);
static_assert(glz::member_names<ForecastArgs>[0] == "city");
static_assert(glz::member_names<ForecastArgs>[1] == "days");

} // namespace

int main() {
  std::string output;
  const auto result = glz::write_json(ForecastArgs{.city = "Detroit"}, output);
  return result ? 0 : 1;
}
