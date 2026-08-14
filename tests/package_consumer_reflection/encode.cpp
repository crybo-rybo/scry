#include <scry/reflection.hpp>

namespace {

struct PackageValue {
  bool ready{};
};

} // namespace

bool package_encode_smoke() {
  const auto encoded = scry::reflection::encode(PackageValue{.ready = true});
  return encoded && encoded->text == R"({"ready":true})";
}
