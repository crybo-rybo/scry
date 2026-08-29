#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace scry::bench {

inline constexpr std::uint64_t fnv_offset = 14'695'981'039'346'656'037ULL;
inline constexpr std::uint64_t fnv_prime = 1'099'511'628'211ULL;

inline void digest_byte(std::uint64_t& digest, const std::uint8_t value) noexcept {
  digest ^= value;
  digest *= fnv_prime;
}

inline void digest_number(std::uint64_t& digest, std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    digest_byte(digest, static_cast<std::uint8_t>(value & 0xffU));
    value >>= 8U;
  }
}

inline void digest_text(std::uint64_t& digest, const std::string_view text) noexcept {
  digest_number(digest, static_cast<std::uint64_t>(text.size()));
  for (const char value : text) {
    digest_byte(digest, static_cast<std::uint8_t>(value));
  }
}

[[nodiscard]] inline std::string fixture_text(const std::size_t size,
                                              const std::size_t seed) {
  return std::string(size, static_cast<char>('a' + static_cast<char>(seed % 26)));
}

[[nodiscard]] inline std::string padded_text(const std::size_t size,
                                             const std::size_t seed) {
  auto value = fixture_text(size, seed);
  const auto prefix = std::to_string(seed) + ':';
  value.replace(0, std::min(prefix.size(), value.size()), prefix);
  return value;
}

[[nodiscard]] inline std::string representative_schema(const std::size_t size,
                                                       const std::size_t seed) {
  constexpr auto prefix = std::string_view{R"({"description":")"};
  constexpr auto suffix = std::string_view{
      R"(","properties":{"payload":{"type":"string"}},"type":"object"})"};
  const auto fixed_size = prefix.size() + suffix.size();
  if (size <= fixed_size) {
    return R"({"type":"object"})";
  }
  return std::string{prefix} + fixture_text(size - fixed_size, seed) +
         std::string{suffix};
}

} // namespace scry::bench
