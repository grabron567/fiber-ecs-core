#include "JobScheduler.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace fiberecs
{
namespace
{
constexpr std::size_t kQueueCapacity = 4096;      // power of two
constexpr std::size_t kInjectionCapacity = 16384; // power of two
// Job storage grows in blocks, so a burst of submissions is bounded by real
// demand rather than by a hard-coded ceiling.
constexpr std::size_t kJobBlockBytes = 256u * 1024u;

// Spin budget before parking; keeps short bursts off the syscall path.
constexpr int kSpinBudget = 96;

thread_local JobScheduler* t_scheduler = nullptr;
thread_local std::uint32_t t_worker_id = JobScheduler::kExternalOwner;

// Per-thread cache of the batch this thread last used for each scheduler.
//
// Deliberately trivially destructible. A thread_local with a non-trivial
// destructor is registered with the TLS atexit machinery, and under MinGW's
// emulated TLS that destructor can run on a thread that never touched the
// variable at all -- worker threads never submit external jobs -- in which case
// it walks uninitialised storage and faults. Since the scheduler owns every
// batch, this cache has nothing to release, so it must stay trivial.
constexpr std::size_t kTlsCacheSlots = 16;

struct TlsCache
{
  JobScheduler* scheduler[kTlsCacheSlots];
  std::uint64_t instance_id[kTlsCacheSlots];
  std::uint64_t generation[kTlsCacheSlots];
  JobBatch* batch[kTlsCacheSlots];
  std::size_t next_slot{0};
};

thread_local TlsCache t_cache;

// Process-wide, so every JobScheduler ever constructed gets its own key even
// when two of them share an address.
std::atomic<std::uint64_t> g_next_instance_id{1};
} // namespace

JobScheduler::JobScheduler()
  : m_injection(kInjectionCapacity), m_instance_id(g_next_instance_id.fetch_add(1, std::memory_order_relaxed))
{
}

JobScheduler::~JobScheduler()
{
  m_stopping.store(true, std::memory_order_release);
  for (auto& w : m_workers)
  {
    if (w && w->started && w->thread.joinable())
    {
      w->thread.join();
    }
  }
}

void JobScheduler::initialize(int threads)
{
  // Idempotent: a second call is a no-op rather than a fatal error, so a
  // scheduler can be safely initialised from more than one place.
  if (m_worker_count.load(std::memory_order_acquire) != 0)
  {
    return;
  }

  int count = threads;
  if (count <= 0)
  {
    count = static_cast<int>(std::thread::hardware_concurrency());
  }
  if (count <= 0)
  {
    count = 1;
  }

  m_workers.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i)
  {
    auto worker = std::make_unique<Worker>(kQueueCapacity);
    worker->arena = std::make_unique<BlockArena>(kJobBlockBytes);
    m_workers.push_back(std::move(worker));
  }

  m_worker_count.store(count, std::memory_order_release);
  m_stopping.store(false, std::memory_order_release);
  // A fresh instance must never adopt the previous one's per-thread batches.
  m_generation.fetch_add(1, std::memory_order_release);

  for (int i = 0; i < count; ++i)
  {
    auto& w = *m_workers[static_cast<std::size_t>(i)];
    w.started = true;
    w.thread = std::thread([this, i] { worker_main(static_cast<std::uint32_t>(i)); });
  }
}

JobScheduler::Worker* JobScheduler::current_worker() noexcept
{
  if (t_scheduler != this || t_worker_id == kExternalOwner)
  {
    return nullptr;
  }
  return m_workers[t_worker_id].get();
}

Job* JobScheduler::alloc(Worker& self)
{
  void* mem = self.arena->allocate(sizeof(Job), alignof(Job));
  assert(mem != nullptr && "worker job storage exhausted");
  if (mem == nullptr)
  {
    std::abort();
  }
  Job* job = ::new (mem) Job();
  job->generation = self.generation;
  job->owner = t_worker_id;
  job->counter = nullptr;
  job->parent = nullptr;
  job->batch = nullptr;
  self.live_jobs.fetch_add(1, std::memory_order_relaxed);
  return job;
}

JobBatch* JobScheduler::batch_for_this_thread()
{
  const std::uint64_t instance = m_instance_id;
  const std::uint64_t generation = m_generation.load(std::memory_order_acquire);

  // Keyed by (address, instance id, generation): a stack-allocated scheduler can
  // be recreated at the same address, and the new instance must not inherit the
  // previous one's outstanding-job accounting.
  for (std::size_t i = 0; i < kTlsCacheSlots; ++i)
  {
    if (t_cache.scheduler[i] == this && t_cache.instance_id[i] == instance &&
        t_cache.generation[i] == generation)
    {
      return t_cache.batch[i];
    }
  }

  // First submission from this thread for this generation: mint an arena and
  // hand ownership to the scheduler, which outlives every submitting thread.
  auto batch = std::make_unique<JobBatch>(kJobBlockBytes);
  JobBatch* const raw = batch.get();
  {
    const std::lock_guard<std::mutex> lock(m_external_batches_mutex);
    m_external_batches.push_back(std::move(batch));
  }

  // Rotating slot. On overflow the previous entry is simply forgotten, which
  // costs a fresh arena but nothing else: the batch stays owned by the
  // scheduler either way.
  const std::size_t slot = t_cache.next_slot;
  t_cache.next_slot = (t_cache.next_slot + 1) % kTlsCacheSlots;
  t_cache.scheduler[slot] = this;
  t_cache.instance_id[slot] = instance;
  t_cache.generation[slot] = m_generation.load(std::memory_order_relaxed);
  t_cache.batch[slot] = raw;
  return raw;
}

Job* JobScheduler::alloc_external()
{
  JobBatch* const batch = batch_for_this_thread();

  // Reclaim protocol: this thread is the only one that ever increments `live`,
  // so observing zero proves that every job from the previous round has already
  // run to completion and that nothing can increment it again until we do. The
  // rewind is therefore safe, and costs one atomic load in the common case.
  if (batch->live.load(std::memory_order_acquire) == 0)
  {
    batch->arena.reset();
  }

  void* mem = batch->arena.allocate(sizeof(Job), alignof(Job));
  assert(mem != nullptr && "external job storage exhausted");
  if (mem == nullptr)
  {
    std::abort();
  }
  Job* job = ::new (mem) Job();
  job->generation = 0;
  job->owner = kExternalOwner;
  job->counter = nullptr;
  job->parent = nullptr;
  job->batch = batch;

  batch->live.fetch_add(1, std::memory_order_relaxed);
  m_external_live.fetch_add(1, std::memory_order_relaxed);
  return job;
}

void JobScheduler::complete(Job* job)
{
  // Snapshot every field first: the arena that owns this job may be recycled the
  // instant its outstanding count drops, so the decrement must be the very last
  // thing done with `job`.
  JobCounter* const parent = job->parent;
  JobCounter* const counter = job->counter;
  const std::uint32_t owner = job->owner;
  JobBatch* const batch = (owner == kExternalOwner) ? job->batch : nullptr;

  // Parent before child: a waiter must never observe `counter` as complete
  // while a job in its subtree is still running.
  if (parent != nullptr)
  {
    parent->release();
  }
  if (counter != nullptr)
  {
    counter->release();
  }

  // Credit the allocation back to whichever arena owns it.
  if (batch != nullptr)
  {
    m_external_live.fetch_sub(1, std::memory_order_acq_rel);
    batch->live.fetch_sub(1, std::memory_order_acq_rel);
  }
  else
  {
    m_workers[owner]->live_jobs.fetch_sub(1, std::memory_order_acq_rel);
  }
}

void JobScheduler::schedule(JobFunction fn, JobCounter* counter)
{
  assert(fn != nullptr && "schedule() called with an empty job");
  if (fn == nullptr)
  {
    return;
  }
  if (counter != nullptr)
  {
    counter->add_ref();
  }

  m_jobs_queued.fetch_add(1, std::memory_order_relaxed);

  if (Worker* self = current_worker(); self != nullptr)
  {
    Job* job = alloc(*self);
    job->fn = std::move(fn);
    job->counter = counter;
    self->queue.push(job); // owner-only push
    return;
  }

  Job* job = alloc_external();
  job->fn = std::move(fn);
  job->counter = counter;
  while (!m_injection.push(job))
  {
    m_injection_full.store(true, std::memory_order_relaxed);
    std::this_thread::yield();
  }
}

void JobScheduler::schedule_child(JobFunction fn, JobCounter* parent, JobCounter* child)
{
  assert(fn != nullptr && "schedule_child() called with an empty job");
  if (fn == nullptr)
  {
    return;
  }
  if (parent != nullptr)
  {
    parent->add_ref();
  }
  if (child != nullptr)
  {
    child->add_ref();
  }

  m_jobs_queued.fetch_add(1, std::memory_order_relaxed);

  if (Worker* self = current_worker(); self != nullptr)
  {
    Job* job = alloc(*self);
    job->fn = std::move(fn);
    job->counter = child;
    job->parent = parent;
    self->queue.push(job);
    return;
  }

  Job* job = alloc_external();
  job->fn = std::move(fn);
  job->counter = child;
  job->parent = parent;
  while (!m_injection.push(job))
  {
    m_injection_full.store(true, std::memory_order_relaxed);
    std::this_thread::yield();
  }
}

bool JobScheduler::try_run(Job* job, bool stolen)
{
  if (job == nullptr)
  {
    return false;
  }
  assert(job->fn != nullptr && "job executed with an empty payload");
  job->fn();
  m_jobs_executed.fetch_add(1, std::memory_order_relaxed);
  if (stolen)
  {
    m_jobs_stolen.fetch_add(1, std::memory_order_relaxed);
  }
  complete(job);
  return true;
}

void JobScheduler::reclaim(Worker& self)
{
  // O(1): rewind the bump pointer once every job from this generation has run.
  // The release()/acquire() pair on live_jobs makes this safe against a
  // concurrent reclaim from another worker.
  if (self.live_jobs.load(std::memory_order_acquire) == 0)
  {
    self.arena->reset();
    ++self.generation;
  }
}

void JobScheduler::worker_loop(Worker& self, std::uint32_t id)
{
  int spins = 0;
  Job* job = nullptr;
  const auto n = static_cast<std::uint32_t>(m_workers.size());

  while (!m_stopping.load(std::memory_order_acquire))
  {
    // 1. Own queue: LIFO keeps the hottest job in cache.
    if (self.queue.pop(job))
    {
      try_run(job, false);
      spins = 0;
      continue;
    }

    // 2. External submissions.
    if (m_injection.pop(job))
    {
      try_run(job, false);
      spins = 0;
      continue;
    }

    // 3. Steal, rotating the victim to spread cache-line contention.
    bool stole = false;
    const std::uint32_t start = (id + 1u + static_cast<std::uint32_t>(spins % static_cast<int>(n))) % n;
    for (std::uint32_t k = 1; k < n; ++k)
    {
      const std::uint32_t victim = (start + k) % n;
      if (m_workers[victim]->queue.steal(job))
      {
        try_run(job, true);
        stole = true;
        break;
      }
      m_steals_failed.fetch_add(1, std::memory_order_relaxed);
    }

    if (stole)
    {
      spins = 0;
      continue;
    }

    // Nothing anywhere: back off rather than burn the core.
    if (++spins < kSpinBudget)
    {
      std::this_thread::yield();
    }
    else
    {
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }

    reclaim(self);
  }

  // Shutdown drain: never abandon queued work.
  while (self.queue.pop(job))
  {
    try_run(job, false);
  }
  while (m_injection.pop(job))
  {
    try_run(job, false);
  }
  reclaim(self);
}

void JobScheduler::worker_main(std::uint32_t id)
{
  t_scheduler = this;
  t_worker_id = id;

  Worker& self = *m_workers[id];
  self.owner_id = std::this_thread::get_id();
  self.queue.bind_owner(self.owner_id);

  worker_loop(self, id);

  t_worker_id = kExternalOwner;
  t_scheduler = nullptr;
}

bool JobScheduler::help(Worker& self, JobCounter* counter, int& spins)
{
  Job* job = nullptr;

  if (self.queue.pop(job) || m_injection.pop(job))
  {
    try_run(job, false);
    spins = 0;
    return true;
  }

  const auto n = static_cast<std::uint32_t>(m_workers.size());
  const std::uint32_t id = t_worker_id;
  for (std::uint32_t k = 1; k < n; ++k)
  {
    const std::uint32_t victim = (id + k) % n;
    if (m_workers[victim]->queue.steal(job))
    {
      try_run(job, true);
      spins = 0;
      return true;
    }
  }
  (void)counter;
  return false;
}

void JobScheduler::wait(JobCounter* counter)
{
  if (counter == nullptr)
  {
    return;
  }

  int spins = 0;
  Job* job = nullptr;

  while (!counter->done())
  {
    // Cooperative wait: keep executing stealable work rather than parking the
    // OS thread. A job waiting on a sub-graph can therefore never starve the
    // very workers that would satisfy its dependency.
    if (Worker* self = current_worker(); self != nullptr)
    {
      if (help(*self, counter, spins))
      {
        continue;
      }
    }
    else
    {
      bool worked = false;
      const auto n = static_cast<std::uint32_t>(m_workers.size());
      for (std::uint32_t k = 0; k < n; ++k)
      {
        if (m_workers[k]->queue.steal(job))
        {
          try_run(job, true);
          worked = true;
          break;
        }
      }
      if (!worked)
      {
        while (m_injection.pop(job))
        {
          try_run(job, false);
          worked = true;
        }
      }
      if (worked)
      {
        spins = 0;
        continue;
      }
    }

    if (++spins < kSpinBudget)
    {
      std::this_thread::yield();
    }
    else
    {
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
  }
}

bool JobScheduler::idle() const noexcept
{
  if (!m_injection.empty_approx())
  {
    return false;
  }
  for (const auto& w : m_workers)
  {
    if (!w->queue.empty_approx())
    {
      return false;
    }
  }
  return true;
}

void JobScheduler::drain()
{
  for (int spin = 0; spin < 1000000; ++spin)
  {
    bool quiescent = m_injection.empty_approx() && m_external_live.load(std::memory_order_acquire) == 0;
    for (const auto& w : m_workers)
    {
      if (!w->queue.empty_approx() || w->live_jobs.load(std::memory_order_acquire) != 0)
      {
        quiescent = false;
        break;
      }
    }
    if (quiescent)
    {
      // Nothing is in flight: every batch arena is fully dead storage and will
      // be rewound by its owning thread on the next submission.
      m_drained.store(true, std::memory_order_release);
      return;
    }
    std::this_thread::yield();
  }
  m_drained.store(false, std::memory_order_release);
}

} // namespace fiberecs