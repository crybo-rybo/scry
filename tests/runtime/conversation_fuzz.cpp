#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <scry/conversation.hpp>
#include <scry/json.hpp>
#include <span>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      const std::size_t size) {
  const auto bytes = std::span{data, size};
  if (bytes.empty()) {
    return 0;
  }
  auto input = std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};

  auto restored = scry::Conversation::from_json(scry::Json{.text = std::move(input)});
  if (!restored) {
    return 0;
  }

  // A document the parser accepted must re-encode, and the encoding must be a
  // fixed point: canonical output parses back to an identical document.
  auto encoded = restored->to_json();
  if (!encoded) {
    std::abort();
  }
  auto round_trip = scry::Conversation::from_json(*encoded);
  if (!round_trip) {
    std::abort();
  }
  auto re_encoded = round_trip->to_json();
  if (!re_encoded || re_encoded->text != encoded->text) {
    std::abort();
  }
  return 0;
}
