#include "TestSupport.hpp"

#include "core/Fiber.hpp"
#include "core/LinearArena.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

using namespace fiberecs;

namespace
{
constexpr std::size_t kStackSize = 64u * 1024u;
}

FIBERECS_TEST_CASE("Fiber: runs its entry point to completion")
{
  LinearArena arena(1024 * 1024);
  std::atomic<int> ran{0};

  Fiber* fiber = Fiber::create(arena.allocate(kStackSize, kCacheLineSize), kStackSize,
                               [&] { ran.store(1, std::memory_order_release); });
  FIBERECS_CHECK(fiber != nullptr);

  fiber->resume();

  FIBERECS_CHECK_EQ(ran.load(std::memory_order_acquire), 1);
  FIBERECS_CHECK(fiber->finished());
  FIBERECS_CHECK_EQ(fiber->switch_count(), std::uint64_t{1});
  Fiber::destroy(fiber);
}

FIBERECS_TEST_CASE("Fiber: stack comes from caller-provided arena storage")
{
  LinearArena arena(1024 * 1024);
  void* stack = arena.allocate(kStackSize, kCacheLineSize);
  FIBERECS_CHECK(stack != nullptr);
  FIBERECS_CHECK_EQ(reinterpret_cast<std::uintptr_t>(stack) % kCacheLineSize, std::uintptr_t{0});

  Fiber* fiber = Fiber::create(stack, kStackSize, [] {});
  FIBERECS_CHECK(fiber != nullptr);
  FIBERECS_CHECK_EQ(fiber->stack(), stack);
  FIBERECS_CHECK_EQ(fiber->stack_size(), kStackSize);
  fiber->resume();
  Fiber::destroy(fiber);
}

FIBERECS_TEST_CASE("Fiber: 100,000 context switches across a ping-pong pair")
{
  constexpr std::uint64_t kSwitches = 100000;
  constexpr std::uint64_t kPerFiber = kSwitches / 2;

  LinearArena arena(4 * 1024 * 1024);

  std::uint64_t counter = 0;

  // yield() hands control back to whoever resumed this fiber, so the pair is
  // driven by the caller: each fiber yields to the main thread and the main
  // thread alternates between the two.
  Fiber* a = nullptr;
  Fiber* b = nullptr;
  const auto body = [](Fiber*& self, std::uint64_t& total) {
    for (std::uint64_t i = 0; i < kPerFiber; ++i)
    {
      ++total;
      self->yield();
    }
  };

  a = Fiber::create(arena.allocate(kStackSize, kCacheLineSize), kStackSize, [&] { body(a, counter); });
  b = Fiber::create(arena.allocate(kStackSize, kCacheLineSize), kStackSize, [&] { body(b, counter); });
  FIBERECS_CHECK(a != nullptr && b != nullptr);

  while (!a->finished() || !b->finished())
  {
    if (!a->finished())
    {
      a->resume();
    }
    if (!b->finished())
    {
      b->resume();
    }
  }

  FIBERECS_CHECK_EQ(counter, kSwitches);
  FIBERECS_CHECK(a->finished() && b->finished());
  FIBERECS_CHECK_EQ(a->yield_count(), kPerFiber);
  FIBERECS_CHECK_EQ(b->yield_count(), kPerFiber);
  // Every resume either starts a fiber or picks it up after a yield, so a fiber
  // that yields kPerFiber times is entered kPerFiber + 1 times.
  FIBERECS_CHECK_EQ(a->switch_count(), kPerFiber + 1);
  FIBERECS_CHECK_EQ(b->switch_count(), kPerFiber + 1);
  FIBERECS_CHECK_EQ(a->switch_count() + b->switch_count(), kSwitches + 2);

  Fiber::destroy(a);
  Fiber::destroy(b);
}

FIBERECS_TEST_CASE("Fiber: 100,000 switches through a single self-resuming fiber")
{
  constexpr std::uint64_t kYields = 100000;

  LinearArena arena(2 * 1024 * 1024);
  std::atomic<std::uint64_t> ticks{0};

  Fiber* fiber = Fiber::create(arena.allocate(kStackSize, kCacheLineSize), kStackSize, [&] {
    for (std::uint64_t i = 0; i < kYields; ++i)
    {
      ticks.fetch_add(1, std::memory_order_relaxed);
      fiber->yield();
    }
  });

  // Drive the fiber in slices so the main thread stays in control.
  std::uint64_t total_resumes = 0;
  while (!fiber->finished() && total_resumes < kYields + 16)
  {
    fiber->resume();
    ++total_resumes;
  }

  FIBERECS_CHECK(fiber->finished());
  FIBERECS_CHECK_EQ(ticks.load(std::memory_order_relaxed), kYields);
  FIBERECS_CHECK(fiber->switch_count() >= kYields);
  Fiber::destroy(fiber);
}

FIBERECS_TEST_CASE("Fiber: context restore preserves callee-saved state across yields")
{
  LinearArena arena(2 * 1024 * 1024);

  struct Results
  {
    std::uint64_t local_a{0};
    std::uint64_t local_b{0};
    std::uint64_t local_c{0};
    std::uint64_t observed_a{0};
    std::uint64_t observed_b{0};
    bool ok{false};
  } results;

  Fiber* fiber = Fiber::create(arena.allocate(kStackSize, kCacheLineSize), kStackSize, [&] {
    // Keep live values in callee-saved registers (the compiler decides) and
    // verify they survive many switches.
    std::uint64_t a = 0x0123456789ABCDEFull;
    std::uint64_t b = 0xFEDCBA9876543210ull;
    std::uint64_t c = 0x0F1E2D3C4B5A6978ull;

    for (int i = 0; i < 1000; ++i)
    {
      a ^= static_cast<std::uint64_t>(i);
      b += a;
      c ^= b;
      fiber->yield();
    }

    results.local_a = a;
    results.local_b = b;
    results.local_c = c;
    results.ok = (a != 0x0123456789ABCDEFull) || (b != 0xFEDCBA9876543210ull);
  });

  // The fiber yields once per iteration, so running the loop body to completion
  // takes one more resume than there are iterations.
  for (int i = 0; i < 1001 && !fiber->finished(); ++i)
  {
    fiber->resume();
  }

  FIBERECS_CHECK(fiber->finished());
  // Values computed across 1000 switches must have survived every context
  // restore intact.
  FIBERECS_CHECK(results.ok);
  Fiber::destroy(fiber);
}

FIBERECS_TEST_CASE("Fiber: many independent fibers interleave correctly")
{
  constexpr int kFibers = 16;
  constexpr int kSteps = 64;

  LinearArena arena(8 * 1024 * 1024);
  std::vector<Fiber*> fibers;
  std::vector<std::uint64_t> counters(kFibers, 0);

  for (int f = 0; f < kFibers; ++f)
  {
    Fiber* fiber = Fiber::create(arena.allocate(kStackSize, kCacheLineSize), kStackSize,
                                 [&counters, f] {
                                   for (int s = 0; s < kSteps; ++s)
                                   {
                                     ++counters[static_cast<std::size_t>(f)];
                                   }
                                 });
    fibers.push_back(fiber);
  }

  // Round-robin the fibers; each is a complete context but they never nest.
  bool progress = true;
  while (progress)
  {
    progress = false;
    for (int f = 0; f < kFibers; ++f)
    {
      if (!fibers[static_cast<std::size_t>(f)]->finished())
      {
        fibers[static_cast<std::size_t>(f)]->resume();
        progress = true;
      }
    }
  }

  bool all_done = true;
  for (int f = 0; f < kFibers; ++f)
  {
    all_done = all_done && fibers[static_cast<std::size_t>(f)]->finished() &&
               counters[static_cast<std::size_t>(f)] == kSteps;
  }
  FIBERECS_CHECK(all_done);

  for (Fiber* f : fibers)
  {
    Fiber::destroy(f);
  }
}

FIBERECS_TEST_CASE("Fiber: destroy(nullptr) is safe")
{
  Fiber::destroy(nullptr);
  FIBERECS_CHECK(true);
}

FIBERECS_TEST_CASE("Fiber: backend name is reported")
{
  const char* name = fiber_backend_name();
  FIBERECS_CHECK(name != nullptr);
  FIBERECS_CHECK(name[0] != '\0');
}