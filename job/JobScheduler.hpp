#pragma once

#include "WorkStealingQueue.hpp"
#include "../core/BlockArena.hpp"
#include "../core/LinearArena.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fiberecs
{

using JobFunction = std::function<void()>;

// Ref-counted completion handle. Reaching zero references means every job
// registered against it has run to completion.
//
// Parent/child chaining: schedule_child() takes a reference on the parent for
// the lifetime of the child, so a parent cannot be observed as complete while
// any descendant is still running.
//
// Contract, and the easiest way to misuse this class:
//   * schedule()/schedule_child() take their own reference, so a counter must
//     NOT be pre-armed with add_ref(n) for jobs submitted through them -- that
//     double-counts and the wait below never returns.
//   * A counter nobody has armed is already done, so wait() returns at once.
class JobCounter
{
public:
  JobCounter() = default;
  JobCounter(const JobCounter&) = delete;
  JobCounter& operator=(const JobCounter&) = delete;

  void add_ref(std::uint32_t n = 1) noexcept
  {
    m_refs.fetch_add(n, std::memory_order_relaxed);
  }

  void release() noexcept { m_refs.fetch_sub(1, std::memory_order_acq_rel); }

  [[nodiscard]] std::uint32_t pending() const noexcept
  {
    return m_refs.load(std::memory_order_acquire);
  }

  // A counter nobody has armed is already satisfied; wait() must return
  // immediately rather than spin for work that was never scheduled.
  [[nodiscard]] bool done() const noexcept { return m_refs.load(std::memory_order_acquire) == 0; }

private:
  std::atomic<std::uint32_t> m_refs{0};
};

// Storage that backs jobs submitted from one non-worker thread.
//
// The submitting thread owns its arena outright, so no lock is needed on the
// hot path and two external threads never contend on the same bump pointer.
//
// Reclamation protocol (see JobScheduler::alloc_external): `live` is the only
// authority. The producing thread is the *sole* incrementer, so if it observes
// `live == 0` no other thread can resurrect it, and every job from the previous
// round has already run to completion. Completion decrements `live` strictly
// after its last read of the job's storage, so recycling is race-free.
struct JobBatch
{
  explicit JobBatch(std::size_t block_bytes) : arena(block_bytes) {}

  JobBatch(const JobBatch&) = delete;
  JobBatch& operator=(const JobBatch&) = delete;

  BlockArena arena;
  std::atomic<std::uint64_t> live{0};
};

// Job control block. The payload callable lives in the arena of whichever
// thread submitted it, so a scheduled job performs no heap traffic of its own.
struct alignas(64) Job
{
  JobFunction fn;
  JobCounter* counter{nullptr};
  JobCounter* parent{nullptr};
  JobBatch* batch{nullptr}; // external jobs only
  std::uint32_t generation{0};
  std::uint32_t owner{0}; // worker index, or kExternalOwner
};

// Fiber-based work-stealing job scheduler.
//
// Each worker owns a Chase-Lev queue and a private arena. Jobs submitted from
// worker threads go straight onto the caller's own queue; jobs submitted from
// external threads are routed through an MPMC injection queue that every worker
// drains. Waiting is cooperative: a thread with an outstanding counter keeps
// executing stealable work rather than parking, so an unmet dependency never
// blocks an OS thread.
class JobScheduler
{
public:
  static constexpr std::uint32_t kExternalOwner = 0xFFFFFFFFu;

  JobScheduler();
  ~JobScheduler();

  JobScheduler(const JobScheduler&) = delete;
  JobScheduler& operator=(const JobScheduler&) = delete;
  JobScheduler(JobScheduler&&) = delete;
  JobScheduler& operator=(JobScheduler&&) = delete;

  // `threads <= 0` selects std::thread::hardware_concurrency().
  void initialize(int threads = 0);

  // Submit a job. `counter` may be null for fire-and-forget work.
  void schedule(JobFunction fn, JobCounter* counter = nullptr);

  // Submit a job whose completion also releases a reference on `parent`.
  void schedule_child(JobFunction fn, JobCounter* parent, JobCounter* child = nullptr);

  // Cooperative wait: pumps stealable work until `counter` completes.
  void wait(JobCounter* counter);

  [[nodiscard]] int worker_count() const noexcept
  {
    return m_worker_count.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool idle() const noexcept;
  [[nodiscard]] std::uint64_t jobs_executed() const noexcept
  {
    return m_jobs_executed.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t jobs_stolen() const noexcept
  {
    return m_jobs_stolen.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t steals_failed() const noexcept
  {
    return m_steals_failed.load(std::memory_order_relaxed);
  }

  // Blocks until every worker has fully drained. Test/verification helper.
  void drain();

private:
  struct alignas(64) Worker
  {
    std::thread thread;
    WorkStealingQueue<Job*> queue;
    std::unique_ptr<BlockArena> arena;
    std::atomic<std::uint32_t> live_jobs{0};
    std::uint32_t generation{1};
    std::thread::id owner_id;
    bool started{false};

    explicit Worker(std::size_t capacity) : queue(capacity) {}
  };

  void worker_main(std::uint32_t id);
  void worker_loop(Worker& self, std::uint32_t id);
  void reclaim(Worker& self);
  bool help(Worker& self, JobCounter* counter, int& spins);

  Job* alloc(Worker& self);
  Job* alloc_external();
  void complete(Job* job);
  bool try_run(Job* job, bool stolen);

  // Arena of the calling thread's external submissions, created on first use.
  [[nodiscard]] JobBatch* batch_for_this_thread();

  [[nodiscard]] Worker* current_worker() noexcept;

  std::vector<std::unique_ptr<Worker>> m_workers;
  MpmcQueue<Job*> m_injection;

  std::atomic<int> m_worker_count{0};
  std::atomic<bool> m_stopping{false};
  std::atomic<std::uint64_t> m_jobs_executed{0};
  std::atomic<std::uint64_t> m_jobs_stolen{0};
  std::atomic<std::uint64_t> m_steals_failed{0};
  std::atomic<std::uint64_t> m_jobs_queued{0};
  std::atomic<bool> m_injection_full{false};
  std::atomic<bool> m_drained{false};

  // Externally submitted jobs currently outstanding across every batch.
  std::atomic<std::uint64_t> m_external_live{0};

  // Bumped by initialize() so thread-local batches are not reused across
  // successive runs of the same instance.
  std::atomic<std::uint64_t> m_generation{1};

  // Unique for the lifetime of the process. Stack-allocated schedulers are
  // routinely created at the same address one after another, and a thread-local
  // batch key that matched those would hand back an arena belonging to an
  // already destroyed scheduler.
  const std::uint64_t m_instance_id;

  // Owns every external submission batch. A submitting thread's jobs routinely
  // outlive the thread itself -- they sit in the injection queue until a worker
  // picks them up -- so the arena backing them cannot be released when that
  // thread exits. The scheduler outlives all of its submitters, so it keeps the
  // arenas alive and reclaims them in its destructor.
  std::mutex m_external_batches_mutex;
  std::vector<std::unique_ptr<JobBatch>> m_external_batches;
};

} // namespace fiberecs