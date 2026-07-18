#include <scry/scry.hpp>

int main() {
  const scry::Config config{
      .base_url = "http://localhost:8080",
      .model = "package-smoke",
  };
  return config.model.empty() ? 1 : 0;
}
