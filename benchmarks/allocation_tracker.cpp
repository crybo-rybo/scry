#include "allocation_tracker.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>

namespace {

std::atomic_flag scope_claimed = ATOMIC_FLAG_INIT;
std::atomic<std::uint64_t> active_epoch{0};
std::atomic<std::uint64_t> next_epoch{1};
std::atomic<std::uint64_t> allocation_calls{0};
std::atomic<std::uint64_t> requested_bytes{0};
std::atomic<std::uint64_t> recorders_in_flight{0};

[[nodiscard]] std::uint64_t start_tracking() noexcept {
  if (scope_claimed.test_and_set(std::memory_order_acq_rel)) {
    return 0;
  }
  allocation_calls.store(0, std::memory_order_relaxed);
  requested_bytes.store(0, std::memory_order_relaxed);
  auto epoch = next_epoch.fetch_add(1, std::memory_order_relaxed);
  if (epoch == 0) {
    epoch = next_epoch.fetch_add(1, std::memory_order_relaxed);
  }
  active_epoch.store(epoch, std::memory_order_release);
  return epoch;
}

[[nodiscard]] scry::bench::AllocationSample
stop_tracking(const std::uint64_t epoch) noexcept {
  auto expected = epoch;
  if (!active_epoch.compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
    scope_claimed.clear(std::memory_order_release);
    return {.valid = false};
  }
  while (recorders_in_flight.load(std::memory_order_acquire) != 0) {
  }
  const auto sample = scry::bench::AllocationSample{
      .calls = allocation_calls.load(std::memory_order_relaxed),
      .requested_bytes = requested_bytes.load(std::memory_order_relaxed),
      .valid = true,
  };
  scope_claimed.clear(std::memory_order_release);
  return sample;
}

[[nodiscard]] void* allocate_bytes(const std::size_t requested) {
  const auto actual = std::max(requested, std::size_t{1});
  auto* memory = std::malloc(actual);
  if (memory == nullptr) {
    throw std::bad_alloc{};
  }
  scry::bench::record_cpp_allocation(requested);
  return memory;
}

[[nodiscard]] void* allocate_aligned(const std::size_t requested,
                                     const std::size_t alignment) {
  if (alignment <= alignof(std::max_align_t)) {
    return allocate_bytes(requested);
  }
  const auto actual = std::max(requested, std::size_t{1});
  if (actual > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
    throw std::bad_alloc{};
  }
  const auto rounded = ((actual + alignment - 1) / alignment) * alignment;
  auto* memory = std::aligned_alloc(alignment, rounded);
  if (memory == nullptr) {
    throw std::bad_alloc{};
  }
  scry::bench::record_cpp_allocation(requested);
  return memory;
}

} // namespace

namespace scry::bench {

AllocationScope::AllocationScope() noexcept
    : epoch_(start_tracking()), active_(epoch_ != 0) {}

AllocationScope::~AllocationScope() {
  if (active_) {
    static_cast<void>(stop_tracking(epoch_));
  }
}

bool AllocationScope::valid() const noexcept { return active_; }

AllocationSample AllocationScope::finish() noexcept {
  if (!active_) {
    return {.valid = false};
  }
  active_ = false;
  return stop_tracking(epoch_);
}

void record_cpp_allocation(const std::size_t requested) noexcept {
  const auto observed_epoch = active_epoch.load(std::memory_order_acquire);
  if (observed_epoch == 0) {
    return;
  }
  recorders_in_flight.fetch_add(1, std::memory_order_acq_rel);
  if (active_epoch.load(std::memory_order_acquire) == observed_epoch) {
    allocation_calls.fetch_add(1, std::memory_order_relaxed);
    requested_bytes.fetch_add(static_cast<std::uint64_t>(requested),
                              std::memory_order_relaxed);
  }
  recorders_in_flight.fetch_sub(1, std::memory_order_release);
}

} // namespace scry::bench

void* operator new(const std::size_t size) { return allocate_bytes(size); }

void* operator new[](const std::size_t size) { return allocate_bytes(size); }

void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate_bytes(size);
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate_bytes(size);
  } catch (...) {
    return nullptr;
  }
}

void* operator new(const std::size_t size, const std::align_val_t alignment) {
  return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](const std::size_t size, const std::align_val_t alignment) {
  return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void* operator new(const std::size_t size, const std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  try {
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](const std::size_t size, const std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  try {
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* memory) noexcept { std::free(memory); }

void operator delete[](void* memory) noexcept { std::free(memory); }

void operator delete(void* memory, const std::size_t) noexcept { std::free(memory); }

void operator delete[](void* memory, const std::size_t) noexcept { std::free(memory); }

void operator delete(void* memory, const std::nothrow_t&) noexcept {
  std::free(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
  std::free(memory);
}

void operator delete(void* memory, const std::align_val_t) noexcept {
  std::free(memory);
}

void operator delete[](void* memory, const std::align_val_t) noexcept {
  std::free(memory);
}

void operator delete(void* memory, const std::size_t, const std::align_val_t) noexcept {
  std::free(memory);
}

void operator delete[](void* memory, const std::size_t,
                       const std::align_val_t) noexcept {
  std::free(memory);
}

void operator delete(void* memory, const std::align_val_t,
                     const std::nothrow_t&) noexcept {
  std::free(memory);
}

void operator delete[](void* memory, const std::align_val_t,
                       const std::nothrow_t&) noexcept {
  std::free(memory);
}
