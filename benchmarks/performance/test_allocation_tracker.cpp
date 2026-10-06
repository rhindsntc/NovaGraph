// Standalone: clang++ -std=c++20 -pthread test_allocation_tracker.cpp -o /tmp/nova-allocation-test
#define NOVA_RESOURCE_DIAGNOSTICS 1
#include "AllocationTracker.hpp"
#include <cstdio>
#include <limits>
#include <pthread.h>

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)
void expect(uint64_t calls, uint64_t total, uint64_t live, uint64_t peak) {
  auto m = nova_resource::snapshot();
  CHECK(m.calls == calls); CHECK(m.totalBytes == total);
  CHECK(m.liveBytes == live); CHECK(m.peakBytes == peak);
}
void ordinary_and_peak() {
  nova_resource::begin_phase();
  void *a = ::operator new(13), *b = ::operator new[](29);
  CHECK(reinterpret_cast<uintptr_t>(a) % alignof(std::max_align_t) == 0);
  expect(2, 42, 42, 42);
  ::operator delete(a); expect(2, 42, 29, 42);
  void *c = ::operator new(7); expect(3, 49, 36, 42);
  ::operator delete[](b); ::operator delete(c, size_t(7));
  expect(3, 49, 0, 42); nova_resource::end_phase();
}
void aligned_and_delete_forms() {
  nova_resource::begin_phase();
  void *a = ::operator new(11, std::align_val_t(64));
  void *b = ::operator new[](17, std::align_val_t(4096));
  CHECK(reinterpret_cast<uintptr_t>(a) % 64 == 0);
  CHECK(reinterpret_cast<uintptr_t>(b) % 4096 == 0);
  expect(2, 28, 28, 28);
  ::operator delete(a, std::align_val_t(64));
  ::operator delete[](b, size_t(17), std::align_val_t(4096));
  void *c = ::operator new(3, std::align_val_t(128));
  void *d = ::operator new[](5, std::align_val_t(256));
  ::operator delete(c, size_t(3), std::align_val_t(128));
  ::operator delete[](d, std::align_val_t(256));
  void *e = ::operator new[](9); ::operator delete[](e, size_t(9));
  expect(5, 45, 0, 28); nova_resource::end_phase();
}
void generations_and_frozen_boundary() {
  void *untracked = ::operator new(91);
  nova_resource::begin_phase();
  void *old = ::operator new(101);
  auto first = nova_resource::end_phase(); CHECK(first.liveBytes == 101);
  void *between = ::operator new(303);
  nova_resource::begin_phase();
  void *current = ::operator new(19);
  ::operator delete(old); ::operator delete(untracked); ::operator delete(between);
  expect(1, 19, 19, 19);
  auto final = nova_resource::end_phase(); CHECK(final.liveBytes == 19);
  ::operator delete(current); expect(1, 19, 19, 19);
  nova_resource::begin_phase(); expect(0, 0, 0, 0); nova_resource::end_phase();
}
void zero_and_nothrow() {
  nova_resource::begin_phase();
  void *a = ::operator new(0), *b = ::operator new[](0, std::nothrow);
  void *c = ::operator new(0, std::align_val_t(64), std::nothrow);
  CHECK(a && b && c && a != b); expect(3, 0, 0, 0);
  ::operator delete(a); ::operator delete[](b, std::nothrow);
  ::operator delete(c, std::align_val_t(64), std::nothrow);
  void *d = ::operator new(17, std::nothrow);
  void *e = ::operator new[](23, std::align_val_t(128), std::nothrow);
  CHECK(d && e); CHECK(reinterpret_cast<uintptr_t>(e) % 128 == 0);
  expect(5, 40, 40, 40);
  ::operator delete(d, std::nothrow);
  ::operator delete[](e, std::align_val_t(128), std::nothrow);
  ::operator delete(nullptr); ::operator delete[](nullptr, std::align_val_t(64));
  expect(5, 40, 0, 40); nova_resource::end_phase();
}
void overflow_rejected() {
  nova_resource::begin_phase();
  // Call through a pointer to prevent compile-time allocation-size diagnostics.
  auto allocate = static_cast<void *(*)(size_t)>(&::operator new);
  bool threw = false;
  try { void *p = allocate(std::numeric_limits<size_t>::max()); ::operator delete(p); }
  catch (const std::bad_alloc &) { threw = true; }
  CHECK(threw);
  auto aligned = static_cast<void *(*)(size_t, std::align_val_t, const std::nothrow_t &) noexcept>(&::operator new[]);
  void *p = aligned(std::numeric_limits<size_t>::max() - 31, std::align_val_t(4096), std::nothrow);
  CHECK(p == nullptr); if (p) ::operator delete[](p, std::align_val_t(4096));
  auto ordinary = static_cast<void *(*)(size_t, const std::nothrow_t &) noexcept>(&::operator new);
  p = ordinary(std::numeric_limits<size_t>::max(), std::nothrow);
  CHECK(p == nullptr); if (p) ::operator delete(p);
  expect(0, 0, 0, 0); nova_resource::end_phase();
}
struct CrossThread { std::atomic<bool> start{false}; void *old = nullptr; void *current = nullptr; };
void *free_from_worker(void *argument) {
  auto &data = *static_cast<CrossThread *>(argument);
  while (!data.start.load(std::memory_order_acquire)) {}
  ::operator delete(data.old); ::operator delete(data.current);
  for (int i = 0; i < 1000; ++i) { void *p = ::operator new(7); ::operator delete(p); }
  return nullptr;
}
void cross_thread_accounting() {
  CrossThread data; pthread_t worker;
  CHECK(pthread_create(&worker, nullptr, free_from_worker, &data) == 0);
  nova_resource::begin_phase(); data.old = ::operator new(101); nova_resource::end_phase();
  nova_resource::begin_phase(); data.current = ::operator new(23);
  data.start.store(true, std::memory_order_release);
  for (int i = 0; i < 1000; ++i) { void *p = ::operator new(11); ::operator delete(p); }
  CHECK(pthread_join(worker, nullptr) == 0);
  auto m = nova_resource::end_phase();
  CHECK(m.calls == 2001); CHECK(m.totalBytes == 18023); CHECK(m.liveBytes == 0);
  CHECK(m.peakBytes >= 23 && m.peakBytes <= 41);
}
}
int main() {
  ordinary_and_peak(); aligned_and_delete_forms(); generations_and_frozen_boundary();
  zero_and_nothrow(); overflow_rejected(); cross_thread_accounting();
  std::printf("allocation tracker: %d failures\n", failures);
  return failures ? 1 : 0;
}
