#include "protocol/sse.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      const std::size_t size) {
  const auto bytes = std::span{data, size};
  if (bytes.empty()) {
    return 0;
  }

  const auto chunk_size =
      std::max<std::size_t>(1, static_cast<std::size_t>(bytes.front()));
  // One sink reused across chunks, cleared per chunk, as the worker does.
  scry::detail::SseParser parser{4096};
  std::vector<scry::detail::SseEvent> events{};
  for (std::size_t offset = 0; offset < bytes.size();) {
    const auto count = std::min(chunk_size, bytes.size() - offset);
    const auto chunk =
        std::string_view{reinterpret_cast<const char*>(bytes.data() + offset), count};
    events.clear();
    if (!parser.push(chunk, events)) {
      return 0;
    }
    offset += count;
  }
  events.clear();
  static_cast<void>(parser.finish(events));
  return 0;
}
