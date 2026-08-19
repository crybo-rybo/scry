#include "allocation_tracker.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>

namespace {

std::atomic_flag scope_claimed = ATOMIC_FLAG_INIT;
std::atomic<std::uint64_t> active_epoch{0};
std::atomic<std::uint64_t> next_epoch{1};
std::atomic<std::uint64_t> allocation_calls{0};
std::atomic<std::uint64_t> requested_bytes{0};
std::atomic<std::uint64_t> live_requested_bytes{0};
std::atomic<std::uint64_t> peak_live_requested_bytes{0};
std::atomic<std::uint64_t> recorders_in_flight{0};

struct AllocationHeader {
  void* base{};
  std::size_t requested{};
  std::uint64_t epoch{};
};

[[nodiscard]] std::uint64_t start_tracking() noexcept {
  if (scope_claimed.test_and_set(std::memory_order_acq_rel)) {
    return 0;
  }
  allocation_calls.store(0, std::memory_order_relaxed);
  requested_bytes.store(0, std::memory_order_relaxed);
  live_requested_bytes.store(0, std::memory_order_relaxed);
  peak_live_requested_bytes.store(0, std::memory_order_relaxed);
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
      .live_requested_bytes = live_requested_bytes.load(std::memory_order_relaxed),
      .peak_live_requested_bytes =
          peak_live_requested_bytes.load(std::memory_order_relaxed),
      .valid = true,
  };
  scope_claimed.clear(std::memory_order_release);
  return sample;
}

[[nodiscard]] void* allocate_bytes(const std::size_t requested,
                                   const std::size_t alignment) {
  const auto actual = std::max(requested, std::size_t{1});
  constexpr auto header_bytes = sizeof(AllocationHeader);
  constexpr auto maximum = std::numeric_limits<std::size_t>::max();
  if (alignment == 0 || actual > maximum - header_bytes ||
      actual + header_bytes > maximum - (alignment - 1)) {
    throw std::bad_alloc{};
  }
  const auto storage_bytes = actual + header_bytes + alignment - 1;
  auto* base = std::malloc(storage_bytes);
  if (base == nullptr) {
    throw std::bad_alloc{};
  }
  void* candidate = static_cast<std::byte*>(base) + header_bytes;
  auto remaining = storage_bytes - header_bytes;
  auto* memory = std::align(alignment, actual, candidate, remaining);
  if (memory == nullptr) {
    std::free(base);
    throw std::bad_alloc{};
  }
  auto* header = reinterpret_cast<AllocationHeader*>(static_cast<std::byte*>(memory) -
                                                     header_bytes);
  header->base = base;
  header->requested = requested;
  header->epoch = scry::bench::record_cpp_allocation(requested);
  return memory;
}

void deallocate_bytes(void* memory) noexcept {
  if (memory == nullptr) {
    return;
  }
  auto* header = reinterpret_cast<AllocationHeader*>(static_cast<std::byte*>(memory) -
                                                     sizeof(AllocationHeader));
  scry::bench::record_cpp_deallocation(header->epoch, header->requested);
  std::free(header->base);
}

[[nodiscard]] bool sample_matches(const scry::bench::AllocationSample& sample,
                                  const std::uint64_t calls,
                                  const std::uint64_t requested,
                                  const std::uint64_t live,
                                  const std::uint64_t peak) noexcept {
  return sample.valid && sample.calls == calls && sample.requested_bytes == requested &&
         sample.live_requested_bytes == live &&
         sample.peak_live_requested_bytes == peak;
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

std::uint64_t record_cpp_allocation(const std::size_t requested) noexcept {
  const auto observed_epoch = active_epoch.load(std::memory_order_acquire);
  if (observed_epoch == 0) {
    return 0;
  }
  recorders_in_flight.fetch_add(1, std::memory_order_acq_rel);
  auto recorded_epoch = std::uint64_t{0};
  if (active_epoch.load(std::memory_order_acquire) == observed_epoch) {
    allocation_calls.fetch_add(1, std::memory_order_relaxed);
    requested_bytes.fetch_add(static_cast<std::uint64_t>(requested),
                              std::memory_order_relaxed);
    const auto live =
        live_requested_bytes.fetch_add(static_cast<std::uint64_t>(requested),
                                       std::memory_order_relaxed) +
        static_cast<std::uint64_t>(requested);
    auto peak = peak_live_requested_bytes.load(std::memory_order_relaxed);
    while (peak < live &&
           !peak_live_requested_bytes.compare_exchange_weak(
               peak, live, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
    recorded_epoch = observed_epoch;
  }
  recorders_in_flight.fetch_sub(1, std::memory_order_release);
  return recorded_epoch;
}

void record_cpp_deallocation(const std::uint64_t epoch,
                             const std::size_t requested) noexcept {
  if (epoch == 0 || active_epoch.load(std::memory_order_acquire) != epoch) {
    return;
  }
  recorders_in_flight.fetch_add(1, std::memory_order_acq_rel);
  if (active_epoch.load(std::memory_order_acquire) == epoch) {
    live_requested_bytes.fetch_sub(static_cast<std::uint64_t>(requested),
                                   std::memory_order_relaxed);
  }
  recorders_in_flight.fetch_sub(1, std::memory_order_release);
}

bool validate_allocation_tracker() noexcept {
  void* retained = nullptr;
  void* aligned = nullptr;
  try {
    retained = ::operator new(32);
    AllocationScope first_scope{};
    ::operator delete(retained);
    retained = ::operator new(32);
    aligned = ::operator new(64, std::align_val_t{64});
    const auto aligned_correctly =
        reinterpret_cast<std::uintptr_t>(aligned) % 64U == 0U;
    ::operator delete(retained);
    retained = nullptr;
    const auto first = first_scope.finish();
    if (!aligned_correctly || !sample_matches(first, 2, 96, 64, 96)) {
      ::operator delete(aligned, std::align_val_t{64});
      return false;
    }

    AllocationScope second_scope{};
    ::operator delete(aligned, std::align_val_t{64});
    aligned = nullptr;
    retained = ::operator new(16);
    const auto second = second_scope.finish();
    ::operator delete(retained);
    retained = nullptr;
    if (!sample_matches(second, 1, 16, 16, 16)) {
      return false;
    }

    AllocationScope outer{};
    AllocationScope overlapping{};
    const auto final = outer.finish();
    return !outer.valid() && !overlapping.valid() && sample_matches(final, 0, 0, 0, 0);
  } catch (...) {
    if (aligned != nullptr) {
      ::operator delete(aligned, std::align_val_t{64});
    }
    if (retained != nullptr) {
      ::operator delete(retained);
    }
    return false;
  }
}

} // namespace scry::bench

void* operator new(const std::size_t size) {
  return allocate_bytes(size, alignof(std::max_align_t));
}

void* operator new[](const std::size_t size) {
  return allocate_bytes(size, alignof(std::max_align_t));
}

void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate_bytes(size, alignof(std::max_align_t));
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate_bytes(size, alignof(std::max_align_t));
  } catch (...) {
    return nullptr;
  }
}

void* operator new(const std::size_t size, const std::align_val_t alignment) {
  return allocate_bytes(size, static_cast<std::size_t>(alignment));
}

void* operator new[](const std::size_t size, const std::align_val_t alignment) {
  return allocate_bytes(size, static_cast<std::size_t>(alignment));
}

void* operator new(const std::size_t size, const std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  try {
    return allocate_bytes(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](const std::size_t size, const std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  try {
    return allocate_bytes(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* memory) noexcept { deallocate_bytes(memory); }

void operator delete[](void* memory) noexcept { deallocate_bytes(memory); }

void operator delete(void* memory, const std::size_t) noexcept {
  deallocate_bytes(memory);
}

void operator delete[](void* memory, const std::size_t) noexcept {
  deallocate_bytes(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept {
  deallocate_bytes(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
  deallocate_bytes(memory);
}

void operator delete(void* memory, const std::align_val_t) noexcept {
  deallocate_bytes(memory);
}

void operator delete[](void* memory, const std::align_val_t) noexcept {
  deallocate_bytes(memory);
}

void operator delete(void* memory, const std::size_t, const std::align_val_t) noexcept {
  deallocate_bytes(memory);
}

void operator delete[](void* memory, const std::size_t,
                       const std::align_val_t) noexcept {
  deallocate_bytes(memory);
}

void operator delete(void* memory, const std::align_val_t,
                     const std::nothrow_t&) noexcept {
  deallocate_bytes(memory);
}

void operator delete[](void* memory, const std::align_val_t,
                       const std::nothrow_t&) noexcept {
  deallocate_bytes(memory);
}
