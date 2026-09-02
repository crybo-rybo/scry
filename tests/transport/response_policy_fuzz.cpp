#include "transport/transport_policy.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      const std::size_t size) {
  const auto bytes = std::span{data, size};
  if (bytes.empty()) {
    return 0;
  }
  // The first byte selects how the remainder is cut into body chunks, so one
  // input exercises many accounting orders against the same ceiling.
  const auto chunk_size =
      std::max<std::size_t>(1, static_cast<std::size_t>(bytes.front()));
  const auto input =
      std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};

  scry::detail::transport_policy::ResponseState state{.limit = 4096};
  auto remainder = input;
  while (!remainder.empty()) {
    const auto newline = remainder.find('\n');
    const auto line = remainder.substr(
        0, newline == std::string_view::npos ? remainder.size() : newline + 1);
    if (!state.accept_header(line)) {
      break;
    }
    remainder.remove_prefix(line.size());
  }
  for (std::size_t offset = 0; offset < remainder.size();) {
    const auto count = std::min(chunk_size, remainder.size() - offset);
    if (!state.account_body(count)) {
      break;
    }
    offset += count;
  }

  static_cast<void>(
      scry::detail::transport_policy::http_error_detail(input, "anthropic"));
  return 0;
}
