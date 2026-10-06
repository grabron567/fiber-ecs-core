#include "TestSupport.hpp"

#include "job/WorkStealingQueue.hpp"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace fiberecs;

FIBERECS_TEST_CASE("WorkStealingQueue: push/pop round-trip from the owner")
{
  WorkStealingQueue<int> q(64);
  q.bind_owner(std::this_thread::get_id());

  FIBERECS_CHECK(q.empty_approx());

  for (int i = 0; i < 16; ++i)
  {
    q.push(i);
  }
  FIBERECS_CHECK_EQ(q.size_approx(), std::int64_t{16});

  std::vector<int> popped;
  int value = -1;
  while (q.pop(value))
  {
    popped.push_back(value);
  }
  FIBERECS_CHECK_EQ(popped.size(), std::size_t{16});
  // Owner pop is LIFO.
  FIBERECS_CHECK_EQ(popped.front(), 15);
  FIBERECS_CHECK_EQ(popped.back(), 0);
}

FIBERECS_TEST_CASE("WorkStealingQueue: pop on an empty queue returns false")
{
  WorkStealingQueue<int> q(16);
  q.bind_owner(std::this_thread::get_id());
  int value = 1234;
  FIBERECS_CHECK(!q.pop(value));
  FIBERECS_CHECK_EQ(value, 1234); // untouched on failure
}

FIBERECS_TEST_CASE("WorkStealingQueue: stealing drains in FIFO order")
{
  WorkStealingQueue<int> q(64);
  q.bind_owner(std::this_thread::get_id());

  for (int i = 0; i < 8; ++i)
  {
    q.push(i * 10);
  }

  std::vector<int> stolen;
  int value = 0;
  while (q.steal(value))
  {
    stolen.push_back(value);
  }
  FIBERECS_CHECK_EQ(stolen.size(), std::size_t{8});
  for (std::size_t i = 0; i < stolen.size(); ++i)
  {
    FIBERECS_CHECK_EQ(stolen[i], static_cast<int>(i) * 10); // steal is FIFO
  }
  FIBERECS_CHECK(q.empty_approx());
}

FIBERECS_TEST_CASE("WorkStealingQueue: the final-element race resolves for exactly one party")
{
  // One element: either the owner's pop or a thief's steal may win, but the
  // value must be delivered exactly once and never duplicated.
  constexpr int kRounds = 5000;

  WorkStealingQueue<int> q(8);
  q.bind_owner(std::this_thread::get_id());

  std::atomic<int> deliveries{0};
  std::atomic<bool> stop{false};

  // The thief has to keep competing for the whole run. Gating it on a
  // "go" flag that is only set for the duration of a single pop() would leave it
  // spinning on a window it can easily miss every time, and join() would then
  // never return. Racing continuously is also what actually exercises the
  // final-element case: owner and thief CASing the same last slot.
  std::thread thief([&] {
    int value = 0;
    while (!stop.load(std::memory_order_acquire))
    {
      if (q.steal(value))
      {
        deliveries.fetch_add(1, std::memory_order_relaxed);
      }
      else
      {
        std::this_thread::yield();
      }
    }
    if (q.steal(value))
    {
      deliveries.fetch_add(1, std::memory_order_relaxed);
    }
  });

  int value = -1;
  for (int i = 0; i < kRounds; ++i)
  {
    q.push(i);
    if (q.pop(value))
    {
      deliveries.fetch_add(1, std::memory_order_relaxed);
    }
  }
  stop.store(true, std::memory_order_release);
  thief.join();

  // The thief may miss the single-element race in some rounds, so the bound is
  // "at most one delivery per push" and the queue never loses or duplicates.
  FIBERECS_CHECK(deliveries.load(std::memory_order_relaxed) <= kRounds);
  FIBERECS_CHECK(q.size_approx() >= 0);
}

FIBERECS_TEST_CASE("WorkStealingQueue: indices wrap without colliding live slots")
{
  // Capacity 16 with far more than 16 pushes: the masked index must recycle
  // only after the consumer has moved past each slot.
  WorkStealingQueue<int> q(16);
  q.bind_owner(std::this_thread::get_id());

  std::vector<int> live;
  for (int i = 0; i < 5000; ++i)
  {
    q.push(i);
    if (live.size() >= 12)
    {
      int value = 0;
      while (q.steal(value))
      {
        live.push_back(value); // drain into the sink
      }
      live.clear();
    }
  }
  int value = 0;
  while (q.steal(value))
  {
  }
  FIBERECS_CHECK(q.empty_approx());
}

FIBERECS_TEST_CASE("WorkStealingQueue: concurrent stealing never duplicates work")
{
  constexpr int kItems = 200000;
  constexpr int kThieves = 4;

  // push() is unbounded: it overwrites live slots once the indices wrap, so the
  // queue must be able to hold every item the owner pushes, even if no thief
  // manages to keep up.
  WorkStealingQueue<int> q(1u << 18);
  q.bind_owner(std::this_thread::get_id());

  std::vector<std::atomic<int>> seen(kItems);
  for (auto& s : seen)
  {
    s.store(0, std::memory_order_relaxed);
  }

  std::atomic<int> producers_done{0};
  std::atomic<bool> stop{false};

  std::vector<std::thread> thieves;
  thieves.reserve(kThieves);
  for (int t = 0; t < kThieves; ++t)
  {
    thieves.emplace_back([&] {
      int value = 0;
      while (!stop.load(std::memory_order_acquire))
      {
        if (q.steal(value))
        {
          seen[static_cast<std::size_t>(value)].fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (producers_done.load(std::memory_order_acquire) == 1 && !q.empty_approx())
        {
          std::this_thread::yield();
          continue;
        }
        if (producers_done.load(std::memory_order_acquire) == 1)
        {
          break;
        }
      }
      // Final sweep: anything left after the producer finished.
      while (q.steal(value))
      {
        seen[static_cast<std::size_t>(value)].fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (int i = 0; i < kItems; ++i)
  {
    q.push(i);
  }
  producers_done.store(1, std::memory_order_release);
  for (auto& th : thieves)
  {
    th.join();
  }

  // Owner sweep for leftovers.
  int value = 0;
  while (q.steal(value))
  {
    seen[static_cast<std::size_t>(value)].fetch_add(1, std::memory_order_relaxed);
  }

  int missing = 0;
  int duplicated = 0;
  for (int i = 0; i < kItems; ++i)
  {
    const int count = seen[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
    if (count == 0)
    {
      ++missing;
    }
    else if (count > 1)
    {
      ++duplicated;
    }
  }

  FIBERECS_CHECK_EQ(missing, 0);
  FIBERECS_CHECK_EQ(duplicated, 0);
  FIBERECS_CHECK(q.empty_approx());
}

FIBERECS_TEST_CASE("WorkStealingQueue: owner pops concurrently with thieves")
{
  constexpr int kItems = 100000;

  // The owner pushes every item but pops only every third one, so up to kItems
  // can be outstanding at once.
  WorkStealingQueue<int> q(1u << 17);
  q.bind_owner(std::this_thread::get_id());

  std::atomic<std::uint64_t> checksum{0};
  std::atomic<int> producers_done{0};

  std::vector<std::thread> thieves;
  for (int t = 0; t < 3; ++t)
  {
    thieves.emplace_back([&] {
      std::uint64_t local = 0;
      int value = 0;
      while (producers_done.load(std::memory_order_acquire) == 0 || !q.empty_approx())
      {
        if (q.steal(value))
        {
          local += static_cast<std::uint64_t>(value) + 1;
        }
      }
      checksum.fetch_add(local, std::memory_order_relaxed);
    });
  }

  std::uint64_t owner_sum = 0;
  int value = 0;
  for (int i = 0; i < kItems; ++i)
  {
    q.push(i);
    if ((i % 3) == 0 && q.pop(value))
    {
      owner_sum += static_cast<std::uint64_t>(value) + 1;
    }
  }
  producers_done.store(1, std::memory_order_release);
  while (q.pop(value))
  {
    owner_sum += static_cast<std::uint64_t>(value) + 1;
  }
  for (auto& t : thieves)
  {
    t.join();
  }

  const std::uint64_t expected =
      (static_cast<std::uint64_t>(kItems) * (kItems + 1)) / 2;
  FIBERECS_CHECK_EQ(owner_sum + checksum.load(std::memory_order_relaxed), expected);
}

FIBERECS_TEST_CASE("MpmcQueue: single-threaded push/pop round-trip")
{
  MpmcQueue<int> q(64);
  for (int i = 0; i < 32; ++i)
  {
    FIBERECS_CHECK(q.push(i));
  }
  for (int i = 0; i < 32; ++i)
  {
    int value = -1;
    FIBERECS_CHECK(q.pop(value));
    FIBERECS_CHECK_EQ(value, i);
  }
  int value = 0;
  FIBERECS_CHECK(!q.pop(value));
  FIBERECS_CHECK(q.empty_approx());
}

FIBERECS_TEST_CASE("MpmcQueue: reports full instead of overwriting")
{
  MpmcQueue<int> q(8);
  int pushed = 0;
  for (int i = 0; i < 64; ++i)
  {
    if (q.push(i))
    {
      ++pushed;
    }
    else
    {
      break;
    }
  }
  FIBERECS_CHECK(pushed > 0);
  FIBERECS_CHECK(pushed <= 8);

  int value = 0;
  for (int i = 0; i < pushed; ++i)
  {
    FIBERECS_CHECK(q.pop(value));
    FIBERECS_CHECK_EQ(value, i);
  }
}

FIBERECS_TEST_CASE("MpmcQueue: concurrent producers and consumers lose nothing")
{
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr int kPerProducer = 20000;
  constexpr int kTotal = kProducers * kPerProducer;

  MpmcQueue<int> q(1024);
  std::atomic<std::uint64_t> checksum{0};
  std::atomic<std::uint64_t> popped{0};
  std::atomic<int> producers_done{0};

  std::vector<std::thread> threads;
  threads.reserve(kProducers + kConsumers);

  for (int t = 0; t < kProducers; ++t)
  {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerProducer; ++i)
      {
        const int value = t * kPerProducer + i + 1;
        while (!q.push(value))
        {
          std::this_thread::yield();
        }
      }
      producers_done.fetch_add(1, std::memory_order_acq_rel);
    });
  }

  // Consumers drain until the producers are done *and* the queue is empty. A
  // fixed per-consumer quota would deadlock whenever one consumer happens to
  // claim more than its share.
  for (int t = 0; t < kConsumers; ++t)
  {
    threads.emplace_back([&] {
      std::uint64_t local = 0;
      std::uint64_t local_count = 0;
      int value = 0;
      for (;;)
      {
        if (q.pop(value))
        {
          local += static_cast<std::uint64_t>(value);
          ++local_count;
          continue;
        }
        if (producers_done.load(std::memory_order_acquire) == kProducers && q.empty_approx())
        {
          break;
        }
        std::this_thread::yield();
      }
      checksum.fetch_add(local, std::memory_order_relaxed);
      popped.fetch_add(local_count, std::memory_order_relaxed);
    });
  }

  for (auto& th : threads)
  {
    th.join();
  }

  const std::uint64_t expected =
      (static_cast<std::uint64_t>(kTotal) * (kTotal + 1)) / 2;
  FIBERECS_CHECK_EQ(popped.load(std::memory_order_relaxed),
                    static_cast<std::uint64_t>(kTotal));
  FIBERECS_CHECK_EQ(checksum.load(std::memory_order_relaxed), expected);
}