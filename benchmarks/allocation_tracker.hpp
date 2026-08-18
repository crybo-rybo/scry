#pragma once

#include <cstddef>
#include <cstdint>

namespace scry::bench {

struct AllocationSample {
  std::uint64_t calls{};
  std::uint64_t requested_bytes{};
  bool valid{true};
};

class AllocationScope final {
public:
  AllocationScope() noexcept;
  ~AllocationScope();

  AllocationScope(const AllocationScope&) = delete;
  AllocationScope& operator=(const AllocationScope&) = delete;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] AllocationSample finish() noexcept;

private:
  std::uint64_t epoch_{};
  bool active_{};
};

void record_cpp_allocation(std::size_t requested_bytes) noexcept;

} // namespace scry::bench
