#include "TestSupport.hpp"

#include "job/JobScheduler.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace fiberecs;

FIBERECS_TEST_CASE("JobCounter: zero references is immediately done")
{
  JobCounter counter;
  FIBERECS_CHECK(counter.pending() == 0);
  FIBERECS_CHECK(counter.done());
}

FIBERECS_TEST_CASE("JobCounter: add_ref / release tracks outstanding work")
{
  JobCounter counter;
  counter.add_ref();
  counter.add_ref(3);
  FIBERECS_CHECK_EQ(counter.pending(), 4u);
  FIBERECS_CHECK(!counter.done());

  counter.release();
  counter.release();
  counter.release();
  FIBERECS_CHECK(!counter.done());
  counter.release();
  FIBERECS_CHECK(counter.done());
  FIBERECS_CHECK_EQ(counter.pending(), 0u);
}

FIBERECS_TEST_CASE("Scheduler: runs a single job to completion")
{
  JobScheduler scheduler;
  scheduler.initialize(2);

  JobCounter counter;
  std::atomic<int> ran{0};
  scheduler.schedule([&ran] { ran.store(1, std::memory_order_release); }, &counter);
  scheduler.wait(&counter);

  FIBERECS_CHECK_EQ(ran.load(std::memory_order_acquire), 1);
  FIBERECS_CHECK(counter.done());
  FIBERECS_CHECK_EQ(scheduler.jobs_executed(), std::uint64_t{1});
}

FIBERECS_TEST_CASE("Scheduler: 100,000 jobs all complete exactly once")
{
  constexpr int kTasks = 100000;

  JobScheduler scheduler;
  scheduler.initialize();

  JobCounter counter;
  std::atomic<std::uint64_t> sum{0};

  for (int i = 0; i < kTasks; ++i)
  {
    scheduler.schedule([&sum] { sum.fetch_add(1, std::memory_order_relaxed); }, &counter);
  }
  scheduler.wait(&counter);
  scheduler.drain();

  FIBERECS_CHECK_EQ(sum.load(std::memory_order_relaxed), static_cast<std::uint64_t>(kTasks));
  FIBERECS_CHECK(counter.done());
  FIBERECS_CHECK_EQ(counter.pending(), 0u);
  FIBERECS_CHECK_EQ(scheduler.jobs_executed(), static_cast<std::uint64_t>(kTasks));
  FIBERECS_CHECK(scheduler.idle());
}

FIBERECS_TEST_CASE("Scheduler: jobs see a consistent view of shared state")
{
  constexpr int kJobs = 20000;
  constexpr int kPerJob = 8;

  JobScheduler scheduler;
  scheduler.initialize();

  JobCounter counter;
  std::atomic<std::uint64_t> total{0};

  for (int i = 0; i < kJobs; ++i)
  {
    scheduler.schedule(
        [&total] {
          for (int k = 0; k < kPerJob; ++k)
          {
            total.fetch_add(1, std::memory_order_relaxed);
          }
        },
        &counter);
  }
  scheduler.wait(&counter);

  FIBERECS_CHECK_EQ(total.load(std::memory_order_relaxed),
                    static_cast<std::uint64_t>(kJobs) * kPerJob);
}

FIBERECS_TEST_CASE("Scheduler: parent/child chain completes before the parent resolves")
{
  JobScheduler scheduler;
  scheduler.initialize();

  JobCounter root;
  JobCounter leaf;
  std::atomic<int> leaf_runs{0};
  std::atomic<bool> parent_saw_children{false};

  constexpr int kParents = 32;
  constexpr int kChildren = 32;

  for (int p = 0; p < kParents; ++p)
  {
    scheduler.schedule_child(
        [&] {
          for (int c = 0; c < kChildren; ++c)
          {
            scheduler.schedule_child(
                [&] {
                  leaf_runs.fetch_add(1, std::memory_order_relaxed);
                },
                &root, &leaf);
          }
        },
        &root, nullptr);
  }

  scheduler.wait(&root);

  // If the parent could resolve early, some children would still be pending.
  parent_saw_children.store(leaf_runs.load(std::memory_order_acquire) ==
                                kParents * kChildren,
                            std::memory_order_release);

  scheduler.wait(&leaf);
  scheduler.drain();

  FIBERECS_CHECK(parent_saw_children.load(std::memory_order_acquire));
  FIBERECS_CHECK_EQ(leaf_runs.load(std::memory_order_acquire), kParents * kChildren);
  FIBERECS_CHECK(root.done() && leaf.done());
}

FIBERECS_TEST_CASE("Scheduler: serial dependency chain observes strictly increasing stages")
{
  JobScheduler scheduler;
  scheduler.initialize(4);

  JobCounter counter;
  std::atomic<int> stage{0};
  std::atomic<bool> in_order{true};

  constexpr int kStages = 500;
  for (int i = 0; i < kStages; ++i)
  {
    scheduler.schedule_child(
        [&stage, &in_order, i] {
          const int expected = i - 1;
          if (i > 0 && stage.load(std::memory_order_acquire) != expected)
          {
            in_order.store(false, std::memory_order_relaxed);
          }
          stage.store(i, std::memory_order_release);
        },
        &counter, nullptr);
    // Each stage takes its own reference, so wait() returns only once that
    // stage has actually run.
    scheduler.wait(&counter);
  }

  FIBERECS_CHECK(in_order.load(std::memory_order_relaxed));
  FIBERECS_CHECK_EQ(stage.load(std::memory_order_acquire), kStages - 1);
  FIBERECS_CHECK(counter.done());
}

FIBERECS_TEST_CASE("Scheduler: nested submission from inside a worker drains fully")
{
  constexpr int kRoots = 128;
  constexpr int kPerRoot = 32;

  JobScheduler scheduler;
  scheduler.initialize();

  JobCounter counter;
  std::atomic<std::uint64_t> total{0};
  std::atomic<int> roots_observed{0};

  for (int r = 0; r < kRoots; ++r)
  {
    scheduler.schedule_child(
        [&] {
          JobCounter local;
          // schedule() takes its own reference, so no pre-arming here.
          for (int i = 0; i < kPerRoot; ++i)
          {
            scheduler.schedule(
                [&total] { total.fetch_add(1, std::memory_order_relaxed); }, &local);
          }
          scheduler.wait(&local);
          roots_observed.fetch_add(1, std::memory_order_relaxed);
        },
        &counter, nullptr);
  }

  scheduler.wait(&counter);
  scheduler.drain();

  FIBERECS_CHECK_EQ(total.load(std::memory_order_relaxed),
                    static_cast<std::uint64_t>(kRoots) * kPerRoot);
  FIBERECS_CHECK_EQ(roots_observed.load(std::memory_order_relaxed), kRoots);
}

FIBERECS_TEST_CASE("Scheduler: a job waiting on a counter does not deadlock the pool")
{
  JobScheduler scheduler;
  scheduler.initialize();

  JobCounter outer;
  std::atomic<int> done{0};
  std::atomic<int> groups_observed{0};

  // Each of these jobs blocks on its own sub-graph. If wait() parked the OS
  // thread instead of stealing, the pool would have no worker left to make
  // progress and this test would hang.
  constexpr int kGroups = 200;
  constexpr int kPerGroup = 16;

  for (int g = 0; g < kGroups; ++g)
  {
    scheduler.schedule_child(
        [&] {
          JobCounter sub;
          // schedule() takes its own reference, so no pre-arming here.
          for (int i = 0; i < kPerGroup; ++i)
          {
            scheduler.schedule([&done] { done.fetch_add(1, std::memory_order_relaxed); },
                               &sub);
          }
          scheduler.wait(&sub);
          groups_observed.fetch_add(1, std::memory_order_relaxed);
        },
        &outer, nullptr);
  }

  scheduler.wait(&outer);
  scheduler.drain();

  FIBERECS_CHECK_EQ(done.load(std::memory_order_relaxed), kGroups * kPerGroup);
  FIBERECS_CHECK_EQ(groups_observed.load(std::memory_order_relaxed), kGroups);
  FIBERECS_CHECK(outer.done());
}

FIBERECS_TEST_CASE("Scheduler: many external producer threads all complete")
{
  constexpr int kProducers = 6;
  constexpr int kPerProducer = 4000;

  JobScheduler scheduler;
  scheduler.initialize();

  JobCounter counter;
  std::atomic<std::uint64_t> ran{0};

  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (int p = 0; p < kProducers; ++p)
  {
    producers.emplace_back([&] {
      for (int i = 0; i < kPerProducer; ++i)
      {
        scheduler.schedule(
            [&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, &counter);
      }
    });
  }
  for (auto& t : producers)
  {
    t.join();
  }

  scheduler.wait(&counter);
  scheduler.drain();

  FIBERECS_CHECK_EQ(ran.load(std::memory_order_relaxed),
                    static_cast<std::uint64_t>(kProducers) * kPerProducer);
  FIBERECS_CHECK_EQ(scheduler.jobs_executed(),
                    static_cast<std::uint64_t>(kProducers) * kPerProducer);
}

FIBERECS_TEST_CASE("Scheduler: work stealing happens under real contention")
{
  JobScheduler scheduler;
  scheduler.initialize();

  // Work has to originate *inside* the pool for stealing to be possible: a job
  // submitted from an external thread lands in the injection queue and is
  // executed by whichever worker claims it, never stolen off another worker.
  //
  // A single root concentrates the whole backlog on one worker's deque. Spreading
  // the roots over many jobs would balance the load across the workers by
  // construction and leave nothing worth stealing, which is what makes this
  // deterministic instead of timing-dependent.
  constexpr int kTotal = 100000;

  JobCounter counter;
  std::atomic<std::uint64_t> ran{0};

  scheduler.schedule_child(
      [&] {
        for (int k = 0; k < kTotal; ++k)
        {
          scheduler.schedule([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, nullptr);
        }
      },
      &counter, nullptr);

  // Deliberately *not* wait(): a non-worker thread calling wait() drains the
  // injection queue itself, so the calling thread would execute the root and
  // every child single-threaded and no steal could ever be observed. Spinning on
  // the counter leaves the root in the injection queue for a worker to claim,
  // which is what puts the backlog on that worker's own deque where the idle
  // workers can take it.
  while (!counter.done())
  {
    std::this_thread::yield();
  }
  scheduler.drain();

  FIBERECS_CHECK_EQ(ran.load(std::memory_order_relaxed), static_cast<std::uint64_t>(kTotal));
  // Far more jobs than workers, so idle workers must have taken work from the
  // busy one rather than the single deque being drained by its owner.
  FIBERECS_CHECK(scheduler.jobs_stolen() > 0);
}

FIBERECS_TEST_CASE("Scheduler: jobs injected from an external thread are never stolen")
{
  // Documents the design: external submissions bypass the per-worker Chase-Lev
  // queues entirely, so every one of them is claimed, not stolen.
  JobScheduler scheduler;
  scheduler.initialize();

  constexpr int kTasks = 20000;
  JobCounter counter;
  std::atomic<std::uint64_t> ran{0};

  for (int i = 0; i < kTasks; ++i)
  {
    scheduler.schedule([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, &counter);
  }
  scheduler.wait(&counter);
  scheduler.drain();

  FIBERECS_CHECK_EQ(ran.load(std::memory_order_relaxed), static_cast<std::uint64_t>(kTasks));
  FIBERECS_CHECK_EQ(scheduler.jobs_stolen(), std::uint64_t{0});
}

FIBERECS_TEST_CASE("Scheduler: fire-and-forget jobs (null counter) still run")
{
  JobScheduler scheduler;
  scheduler.initialize();

  std::atomic<std::uint64_t> ran{0};
  for (int i = 0; i < 1000; ++i)
  {
    scheduler.schedule([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
  }
  scheduler.drain();

  FIBERECS_CHECK_EQ(ran.load(std::memory_order_relaxed), std::uint64_t{1000});
}

FIBERECS_TEST_CASE("Scheduler: waits on a null counter are a no-op")
{
  JobScheduler scheduler;
  scheduler.initialize(2);
  scheduler.wait(nullptr);
  JobCounter done_counter;
  scheduler.wait(&done_counter);
  FIBERECS_CHECK(done_counter.done());
}

FIBERECS_TEST_CASE("Scheduler: worker_count respects hardware concurrency")
{
  JobScheduler scheduler;
  scheduler.initialize();
  FIBERECS_CHECK_EQ(scheduler.worker_count(),
                    static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));

  JobScheduler explicit_pool;
  explicit_pool.initialize(3);
  FIBERECS_CHECK_EQ(explicit_pool.worker_count(), 3);
}

FIBERECS_TEST_CASE("Scheduler: drain() waits for full quiescence")
{
  JobScheduler scheduler;
  scheduler.initialize();

  JobCounter counter;
  std::atomic<std::uint64_t> ran{0};
  for (int i = 0; i < 5000; ++i)
  {
    scheduler.schedule([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, &counter);
  }
  scheduler.drain();

  FIBERECS_CHECK_EQ(ran.load(std::memory_order_relaxed), std::uint64_t{5000});
  FIBERECS_CHECK(scheduler.idle());
  FIBERECS_CHECK_EQ(scheduler.jobs_executed(), std::uint64_t{5000});
}

FIBERECS_TEST_CASE("Scheduler: repeated initialize() is a no-op")
{
  JobScheduler scheduler;
  scheduler.initialize(2);
  const int workers = scheduler.worker_count();
  scheduler.initialize(8);
  FIBERECS_CHECK_EQ(scheduler.worker_count(), workers);
}

FIBERECS_TEST_CASE("Scheduler: job dependencies published atomically are observed in order")
{
  JobScheduler scheduler;
  scheduler.initialize();

  JobCounter counter;
  std::atomic<int> published{0};
  std::atomic<bool> monotonic{true};

  constexpr int kSteps = 400;
  for (int i = 1; i <= kSteps; ++i)
  {
    scheduler.schedule_child(
        [&published, &monotonic, i] {
          const int previous = published.load(std::memory_order_acquire);
          if (i > 1 && previous != i - 1)
          {
            monotonic.store(false, std::memory_order_relaxed);
          }
          published.store(i, std::memory_order_release);
        },
        &counter, nullptr);

    // Block until this step lands before scheduling the next one, which makes
    // the dependency explicit rather than relying on queue ordering.
    // No add_ref() here: schedule_child() already took this step's reference.
    while (published.load(std::memory_order_acquire) < i)
    {
      std::this_thread::yield();
    }
  }
  scheduler.wait(&counter);

  FIBERECS_CHECK(monotonic.load(std::memory_order_relaxed));
  FIBERECS_CHECK_EQ(published.load(std::memory_order_acquire), kSteps);
  FIBERECS_CHECK(counter.done());
}