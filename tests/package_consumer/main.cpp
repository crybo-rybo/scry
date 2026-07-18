#include <scry/error.hpp>
#include <scry/scry.hpp>

int main() {
  const auto conversation = scry::Conversation::create();
  if (!conversation) {
    return 1;
  }
  const auto harness = scry::Harness::create(scry::Config{
      .base_url = "http://localhost:8080",
      .model = "package-smoke",
  });
  return !harness && harness.error().category == scry::ErrorCategory::invalid_config
             ? 0
             : 2;
}
