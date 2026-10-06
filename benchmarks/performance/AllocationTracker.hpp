#pragma once
#ifndef NOVA_RESOURCE_DIAGNOSTICS
#error "AllocationTracker.hpp is only for the nonshipping diagnostic executable"
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

// Include in exactly ONE translation unit. These global replacements cover C++
// ordinary/aligned scalar/array allocations reaching operator new, including
// sized deletes and nothrow forms. They do not measure direct malloc, Swift,
// SQLite C allocation, allocator overhead, or allocations optimized away.
// Each allocation adds a Header and up to alignment-1 padding bytes. Those bytes
// and malloc's overhead are excluded from metrics. All allocations, even outside
// phases, pay this cost plus a bookkeeping spinlock; diagnostic timings must not
// replace the uninstrumented baseline. Not async-signal-safe or fork-safe.
namespace nova_resource {
struct AllocationMetrics {
  uint64_t calls = 0;       // Successful allocations; failures are not counted.
  uint64_t totalBytes = 0;  // Requested bytes; zero-size requests count zero bytes.
  uint64_t liveBytes = 0;   // This phase's allocations surviving at the snapshot.
  uint64_t peakBytes = 0;   // Maximum live requested bytes during this phase.
};
namespace detail {
struct Header { void *raw; size_t size; uint64_t generation; };
struct State {
  std::atomic_flag lock = ATOMIC_FLAG_INIT;
  uint64_t generation = 0;
  bool active = false;
  AllocationMetrics metrics;
};
inline constinit State state;
class Guard {
public:
  Guard() noexcept { while (state.lock.test_and_set(std::memory_order_acquire)) {} }
  ~Guard() { state.lock.clear(std::memory_order_release); }
  Guard(const Guard &) = delete;
  Guard &operator=(const Guard &) = delete;
};
inline void *allocate(size_t requested, size_t alignment) {
  if (!alignment || (alignment & (alignment - 1))) throw std::bad_alloc();
  if (alignment < alignof(Header)) alignment = alignof(Header);
  const size_t bytes = requested ? requested : 1;
  constexpr auto maximum = std::numeric_limits<size_t>::max();
  if (alignment - 1 > maximum - sizeof(Header) ||
      bytes > maximum - sizeof(Header) - (alignment - 1)) throw std::bad_alloc();
  const size_t total = bytes + sizeof(Header) + alignment - 1;
  void *raw = nullptr;
  while (!(raw = std::malloc(total))) {
    auto handler = std::get_new_handler();
    if (!handler) throw std::bad_alloc();
    handler();
  }
  const auto start = reinterpret_cast<uintptr_t>(raw) + sizeof(Header);
  const auto aligned = (start + alignment - 1) & ~(uintptr_t(alignment) - 1);
  auto *header = ::new (reinterpret_cast<void *>(aligned - sizeof(Header))) Header{raw, requested, 0};
  bool overflow = false;
  {
    Guard guard;
    if (state.active) {
      auto &m = state.metrics;
      constexpr auto counter_max = std::numeric_limits<uint64_t>::max();
      overflow = m.calls == counter_max || requested > counter_max - m.totalBytes;
      if (!overflow) {
        header->generation = state.generation;
        ++m.calls; m.totalBytes += requested; m.liveBytes += requested;
        if (m.liveBytes > m.peakBytes) m.peakBytes = m.liveBytes;
      }
    }
  }
  if (overflow) { std::free(raw); throw std::bad_alloc(); }
  return reinterpret_cast<void *>(aligned);
}
inline void release(void *pointer) noexcept {
  if (!pointer) return;
  auto *header = reinterpret_cast<Header *>(static_cast<unsigned char *>(pointer) - sizeof(Header));
  {
    Guard guard;
    if (state.active && header->generation == state.generation)
      state.metrics.liveBytes -= header->size;
  }
  std::free(header->raw);
}
} // namespace detail

// Phases are sequential, process-wide, and attributed when bookkeeping acquires
// the lock. Synchronize worker lifetimes at boundaries for meaningful attribution.
// Nested begin or generation exhaustion is a caller error and aborts. A snapshot
// never resets counters. end_phase freezes metrics; later frees cannot change it.
inline void begin_phase() noexcept {
  detail::Guard guard;
  auto &s = detail::state;
  if (s.active || s.generation == std::numeric_limits<uint64_t>::max()) std::abort();
  ++s.generation; s.metrics = {}; s.active = true;
}
inline AllocationMetrics snapshot() noexcept {
  detail::Guard guard;
  return detail::state.metrics;
}
inline AllocationMetrics end_phase() noexcept {
  detail::Guard guard;
  detail::state.active = false;
  return detail::state.metrics;
}
} // namespace nova_resource

void *operator new(std::size_t n) { return nova_resource::detail::allocate(n, __STDCPP_DEFAULT_NEW_ALIGNMENT__); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void *operator new(std::size_t n, std::align_val_t a) { return nova_resource::detail::allocate(n, static_cast<size_t>(a)); }
void *operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { try { return ::operator new(n); } catch (...) { return nullptr; } }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { try { return ::operator new[](n); } catch (...) { return nullptr; } }
void *operator new(std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept { try { return ::operator new(n, a); } catch (...) { return nullptr; } }
void *operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept { try { return ::operator new[](n, a); } catch (...) { return nullptr; } }
void operator delete(void *p) noexcept { nova_resource::detail::release(p); }
void operator delete[](void *p) noexcept { nova_resource::detail::release(p); }
void operator delete(void *p, std::size_t) noexcept { nova_resource::detail::release(p); }
void operator delete[](void *p, std::size_t) noexcept { nova_resource::detail::release(p); }
void operator delete(void *p, std::align_val_t) noexcept { nova_resource::detail::release(p); }
void operator delete[](void *p, std::align_val_t) noexcept { nova_resource::detail::release(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { nova_resource::detail::release(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { nova_resource::detail::release(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { nova_resource::detail::release(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { nova_resource::detail::release(p); }
void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept { nova_resource::detail::release(p); }
void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept { nova_resource::detail::release(p); }
