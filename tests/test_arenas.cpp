#include "TestSupport.hpp"

#include "core/LinearArena.hpp"
#include "core/MemoryUtils.hpp"
#include "core/RingArena.hpp"

#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

using namespace fiberecs;

FIBERECS_TEST_CASE("MemoryUtils::align_up / align_down round to alignment boundaries")
{
  FIBERECS_CHECK(is_power_of_two(64));
  FIBERECS_CHECK(!is_power_of_two(48));
  FIBERECS_CHECK_EQ(align_up(0, 64), std::size_t{0});
  FIBERECS_CHECK_EQ(align_up(1, 64), std::size_t{64});
  FIBERECS_CHECK_EQ(align_up(64, 64), std::size_t{64});
  FIBERECS_CHECK_EQ(align_up(65, 64), std::size_t{128});
  FIBERECS_CHECK_EQ(align_down(65, 64), std::size_t{64});
}

FIBERECS_TEST_CASE("LinearArena: allocations honour requested alignment")
{
  LinearArena arena(64 * 1024);

  for (std::size_t alignment : {std::size_t{8}, std::size_t{16}, std::size_t{64},
                                std::size_t{128}})
  {
    void* p = arena.allocate(64, alignment);
    FIBERECS_CHECK(p != nullptr);
    FIBERECS_CHECK_EQ(reinterpret_cast<std::uintptr_t>(p) % alignment, std::uintptr_t{0});
  }
}

FIBERECS_TEST_CASE("LinearArena: default alignment satisfies max_align_t")
{
  LinearArena arena(16 * 1024);
  for (int i = 0; i < 64; ++i)
  {
    void* p = arena.allocate(8);
    FIBERECS_CHECK_EQ(reinterpret_cast<std::uintptr_t>(p) % alignof(std::max_align_t),
                      std::uintptr_t{0});
  }
}

FIBERECS_TEST_CASE("LinearArena: allocations are packed and do not overlap")
{
  LinearArena arena(16 * 1024);
  std::vector<std::pair<std::byte*, std::byte*>> spans;
  for (int i = 0; i < 32; ++i)
  {
    auto* p = static_cast<std::byte*>(arena.allocate(100, 64));
    spans.emplace_back(p, p + 100);
  }
  for (std::size_t i = 1; i < spans.size(); ++i)
  {
    FIBERECS_CHECK(spans[i - 1].second <= spans[i].first);
  }
}

FIBERECS_TEST_CASE("LinearArena: reset() is O(1) and reclaims everything")
{
  LinearArena arena(128 * 1024);
  for (int i = 0; i < 100; ++i)
  {
    (void)arena.allocate(1024, 64);
  }
  FIBERECS_CHECK_EQ(arena.used(), std::size_t{102400});
  FIBERECS_CHECK(arena.high_water() >= 102400);
  FIBERECS_CHECK_EQ(arena.allocation_count(), std::size_t{100});

  const std::size_t capacity_before = arena.capacity();
  arena.reset();

  FIBERECS_CHECK_EQ(arena.used(), std::size_t{0});
  FIBERECS_CHECK_EQ(arena.allocation_count(), std::size_t{0});
  FIBERECS_CHECK_EQ(arena.capacity(), capacity_before); // no free(), no realloc
}

FIBERECS_TEST_CASE("LinearArena: high_water tracks peak usage across resets")
{
  LinearArena arena(64 * 1024);
  (void)arena.allocate(1024, 64);
  (void)arena.allocate(2048, 64);
  const std::size_t peak = arena.high_water();
  FIBERECS_CHECK(peak >= 3072);

  arena.reset();
  (void)arena.allocate(512, 64);
  FIBERECS_CHECK_EQ(arena.high_water(), peak); // peak is remembered
}

FIBERECS_TEST_CASE("LinearArena: emplace constructs in arena storage")
{
  struct Payload
  {
    int a;
    double b;
    char c;
  };

  LinearArena arena(4096);
  Payload* p = arena.emplace<Payload>(Payload{7, 2.5, 'x'});
  FIBERECS_CHECK(p != nullptr);
  FIBERECS_CHECK_EQ(p->a, 7);
  FIBERECS_CHECK_EQ(p->b, 2.5);
  FIBERECS_CHECK_EQ(p->c, 'x');
  FIBERECS_CHECK_EQ(reinterpret_cast<std::uintptr_t>(p) % alignof(Payload), std::uintptr_t{0});
}

FIBERECS_TEST_CASE("RingArena: producer/consumer round-trip preserves payload")
{
  RingArena ring(64 * 1024);
  for (int i = 0; i < 1000; ++i)
  {
    auto* p = static_cast<unsigned char*>(ring.allocate(128, 64));
    FIBERECS_CHECK(p != nullptr);
    FIBERECS_CHECK_EQ(reinterpret_cast<std::uintptr_t>(p) % 64, std::uintptr_t{0});
    std::memset(p, i & 0xFF, 128);
    ring.publish();

    const auto* c = static_cast<const unsigned char*>(ring.consume_start());
    FIBERECS_CHECK(c != nullptr);
    for (std::size_t k = 0; k < 128; ++k)
    {
      FIBERECS_CHECK_EQ(static_cast<int>(c[k]), i & 0xFF);
    }
    ring.consume_end();
  }
  FIBERECS_CHECK_EQ(ring.pending_bytes(), std::size_t{0});
}

FIBERECS_TEST_CASE("RingArena: backpressure reports empty rather than overwriting")
{
  RingArena ring(4096);
  // Fill to capacity without consuming.
  std::size_t published = 0;
  int allocations = 0;
  for (int i = 0; i < 64; ++i)
  {
    void* p = ring.allocate(64, 64);
    if (p == nullptr)
    {
      break;
    }
    ring.publish();
    published += 64;
    ++allocations;
  }
  FIBERECS_CHECK(published > 0);
  FIBERECS_CHECK_EQ(allocations, 64);
  FIBERECS_CHECK_EQ(ring.pending_bytes(), published);
  FIBERECS_CHECK(ring.consume_start() != nullptr);

  // The ring is full, so the producer is told to wait rather than overwriting
  // data the consumer has not read yet.
  FIBERECS_CHECK(ring.allocate(64, 64) == nullptr);
  FIBERECS_CHECK(ring.hard_overflow());

  // Consuming one object frees exactly one object worth of space.
  ring.consume_end();
  FIBERECS_CHECK_EQ(ring.pending_bytes(), published - 64);
  void* again = ring.allocate(64, 64);
  FIBERECS_CHECK(again != nullptr);
  ring.publish();

  // Draining the rest empties the ring.
  while (ring.consume_start() != nullptr)
  {
    ring.consume_end();
  }
  FIBERECS_CHECK_EQ(ring.pending_bytes(), std::size_t{0});
  FIBERECS_CHECK(ring.consume_start() == nullptr);
}

FIBERECS_TEST_CASE("RingArena: wraps around repeatedly without corruption")
{
  RingArena ring(16 * 1024);
  for (int round = 0; round < 500; ++round)
  {
    auto* p = static_cast<unsigned char*>(ring.allocate(512, 64));
    FIBERECS_CHECK(p != nullptr);
    for (std::size_t k = 0; k < 512; ++k)
    {
      p[k] = static_cast<unsigned char>((static_cast<std::size_t>(round) + k) & 0xFF);
    }
    ring.publish();

    const auto* c = static_cast<const unsigned char*>(ring.consume_start());
    FIBERECS_CHECK(c != nullptr);
    for (std::size_t k = 0; k < 512; ++k)
    {
      FIBERECS_CHECK_EQ(static_cast<int>(c[k]), static_cast<int>((static_cast<std::size_t>(round) + k) & 0xFFU));
    }
    ring.consume_end();
  }
}

FIBERECS_TEST_CASE("RingArena: reset rewinds both cursors in O(1)")
{
  RingArena ring(8192);
  void* p = ring.allocate(256, 64);
  ring.publish();
  FIBERECS_CHECK(ring.pending_bytes() > 0);

  const std::size_t capacity = ring.capacity();
  ring.reset();

  FIBERECS_CHECK_EQ(ring.pending_bytes(), std::size_t{0});
  FIBERECS_CHECK_EQ(ring.allocation_count(), std::size_t{0});
  FIBERECS_CHECK_EQ(ring.capacity(), capacity);
  FIBERECS_CHECK(ring.consume_start() == nullptr);
  (void)p;
}

FIBERECS_TEST_CASE("RingArena: SPSC handoff under concurrent producer and consumer")
{
  RingArena ring(256 * 1024);
  constexpr std::size_t kChunk = 1024;
  constexpr int kCount = 20000;

  std::atomic<bool> producer_done{false};
  std::atomic<std::uint64_t> received{0};
  std::atomic<bool> payload_ok{true};

  std::thread consumer([&] {
    std::vector<unsigned char> expected(kChunk);
    std::size_t index = 0;
    while (index < static_cast<std::size_t>(kCount))
    {
      const auto* c = static_cast<const unsigned char*>(ring.consume_start());
      if (c == nullptr)
      {
        if (producer_done.load(std::memory_order_acquire) &&
            ring.consume_start() == nullptr)
        {
          break;
        }
        std::this_thread::yield();
        continue;
      }
      for (std::size_t k = 0; k < kChunk; ++k)
      {
        expected[k] = static_cast<unsigned char>((index + k) & 0xFF);
      }
      if (std::memcmp(c, expected.data(), kChunk) != 0)
      {
        payload_ok.store(false, std::memory_order_relaxed);
      }
      ring.consume_end();
      received.fetch_add(kChunk, std::memory_order_relaxed);
      ++index;
    }
  });

  std::thread producer([&] {
    for (int i = 0; i < kCount; ++i)
    {
      void* p = nullptr;
      while ((p = ring.allocate(kChunk, 64)) == nullptr)
      {
        std::this_thread::yield();
      }
      auto* bytes = static_cast<unsigned char*>(p);
      for (std::size_t k = 0; k < kChunk; ++k)
      {
        bytes[k] = static_cast<unsigned char>((static_cast<std::size_t>(i) + k) & 0xFF);
      }
      ring.publish();
    }
    producer_done.store(true, std::memory_order_release);
  });

  producer.join();
  consumer.join();

  FIBERECS_CHECK(payload_ok.load(std::memory_order_relaxed));
  FIBERECS_CHECK_EQ(received.load(std::memory_order_relaxed),
                    static_cast<std::uint64_t>(kCount) * kChunk);
}

FIBERECS_TEST_CASE("RingArena: cache-line alignment holds across wrap boundaries")
{
  RingArena ring(8192);
  for (int i = 0; i < 200; ++i)
  {
    auto* p = static_cast<std::byte*>(ring.allocate(257, 64));
    FIBERECS_CHECK(p != nullptr);
    FIBERECS_CHECK_EQ(reinterpret_cast<std::uintptr_t>(p) % 64, std::uintptr_t{0});
    ring.publish();
    (void)ring.consume_start();
    ring.consume_end();
  }
}