#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>

#if defined(_DEBUG) || !defined(NDEBUG)
#define FIBERECS_QUEUE_ASSERTS 1
#endif

namespace fiberecs
{

// Chase-Lev bounded work-stealing queue.
//
// Contract (violating it corrupts the ring, this is not optional):
//   * push() / pop()  -> SINGLE OWNER THREAD only.
//   * steal()         -> any thread that is not the owner.
//
// Capacity must be a power of two. Indices are monotonically increasing
// 64-bit counters; wrapping is masked, so the queue never runs out of index
// space for any realistic frame budget.
template <typename T>
class WorkStealingQueue
{
public:
  static_assert(std::is_trivially_copyable_v<T>, "slot type must be trivially copyable");

  explicit WorkStealingQueue(std::size_t capacity_pow2)
    : m_mask(capacity_pow2 - 1)
    , m_slots(std::make_unique<std::atomic<T>[]>(capacity_pow2))
  {
    for (std::size_t i = 0; i < capacity_pow2; ++i)
    {
      m_slots[i].store(T{}, std::memory_order_relaxed);
    }
  }

  WorkStealingQueue(const WorkStealingQueue&) = delete;
  WorkStealingQueue& operator=(const WorkStealingQueue&) = delete;

  // ---- owner thread ------------------------------------------------------

  // Unbounded: there is no full check, because the owner is the only writer and
  // is not supposed to outrun the thieves. The capacity therefore has to be at
  // least the largest number of items that can be outstanding at once -- if the
  // indices wrap, push() silently overwrites slots that have not been taken yet.
  void push(T value)
  {
    assert_owner();
    const std::int64_t b = m_bottom.load(std::memory_order_relaxed);
    m_slots[static_cast<std::size_t>(b) & m_mask].store(value, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    m_bottom.store(b + 1, std::memory_order_relaxed);
  }

  bool pop(T& out)
  {
    assert_owner();

    const std::int64_t b = m_bottom.load(std::memory_order_relaxed) - 1;
    m_bottom.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    std::int64_t t = m_top.load(std::memory_order_relaxed);
    if (t <= b)
    {
      out = m_slots[static_cast<std::size_t>(b) & m_mask].load(std::memory_order_relaxed);
      if (t == b)
      {
        // Last element: race the thieves for it.
        if (!m_top.compare_exchange_strong(t, t + 1,
                                           std::memory_order_seq_cst,
                                           std::memory_order_relaxed))
        {
          out = T{};
          m_bottom.store(b + 1, std::memory_order_relaxed);
          return false;
        }
        m_bottom.store(b + 1, std::memory_order_relaxed);
      }
      return true;
    }

    m_bottom.store(b + 1, std::memory_order_relaxed);
    return false;
  }

  // ---- thieves -----------------------------------------------------------

  bool steal(T& out)
  {
    std::int64_t t = m_top.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const std::int64_t b = m_bottom.load(std::memory_order_acquire);

    if (t >= b)
    {
      return false;
    }

    const T value = m_slots[static_cast<std::size_t>(t) & m_mask].load(std::memory_order_relaxed);
    if (!m_top.compare_exchange_strong(t, t + 1,
                                       std::memory_order_seq_cst,
                                       std::memory_order_relaxed))
    {
      return false;
    }

    out = value;
    return true;
  }

  std::int64_t size_approx() const
  {
    const std::int64_t b = m_bottom.load(std::memory_order_acquire);
    const std::int64_t t = m_top.load(std::memory_order_acquire);
    return b - t;
  }

  bool empty_approx() const { return size_approx() <= 0; }

private:
  void assert_owner() const
  {
#if FIBERECS_QUEUE_ASSERTS
    const auto self = std::hash<std::thread::id>{}(std::this_thread::get_id());
    assert(m_owner.load(std::memory_order_relaxed) == self
           && "push/pop must only be called by the owning thread");
#else
    (void)this;
#endif
  }

public:
  // Called once by the worker that owns this queue, before any push/pop.
  void bind_owner(std::thread::id id)
  {
    m_owner.store(std::hash<std::thread::id>{}(id), std::memory_order_relaxed);
  }

private:

  std::size_t m_mask;
  std::unique_ptr<std::atomic<T>[]> m_slots;

  alignas(64) std::atomic<std::int64_t> m_top{0};
  alignas(64) std::atomic<std::int64_t> m_bottom{0};
  alignas(64) std::atomic<std::size_t> m_owner{0};
};

// Bounded multi-producer / multi-consumer queue used for submissions coming
// from threads that do not own a worker queue (the main thread, IO threads,
// etc.). Vyukov-style sequence cells: no locks, no CAS loop, blocking-free.
template <typename T>
class MpmcQueue
{
public:
  explicit MpmcQueue(std::size_t capacity_pow2)
    : m_mask(capacity_pow2 - 1)
    , m_capacity(capacity_pow2)
    , m_cells(std::make_unique<Cell[]>(capacity_pow2))
  {
    for (std::size_t i = 0; i < capacity_pow2; ++i)
    {
      m_cells[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  MpmcQueue(const MpmcQueue&) = delete;
  MpmcQueue& operator=(const MpmcQueue&) = delete;

  bool push(T value)
  {
    std::size_t pos = m_enqueue.load(std::memory_order_relaxed);
    for (;;)
    {
      Cell& cell = m_cells[pos & m_mask];
      const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
      const auto diff = static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos);
      if (diff == 0)
      {
        if (m_enqueue.compare_exchange_weak(pos, pos + 1,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed))
        {
          cell.data = value;
          cell.sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
      }
      else if (diff < 0)
      {
        return false; // full
      }
      else
      {
        pos = m_enqueue.load(std::memory_order_relaxed);
      }
    }
  }

  bool pop(T& out)
  {
    std::size_t pos = m_dequeue.load(std::memory_order_relaxed);
    for (;;)
    {
      Cell& cell = m_cells[pos & m_mask];
      const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
      const auto diff = static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos + 1);
      if (diff == 0)
      {
        if (m_dequeue.compare_exchange_weak(pos, pos + 1,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed))
        {
          out = cell.data;
          cell.sequence.store(pos + m_capacity, std::memory_order_release);
          return true;
        }
      }
      else if (diff < 0)
      {
        return false; // empty
      }
      else
      {
        pos = m_dequeue.load(std::memory_order_relaxed);
      }
    }
  }

  bool empty_approx() const
  {
    return m_dequeue.load(std::memory_order_acquire) >= m_enqueue.load(std::memory_order_acquire);
  }

private:
  struct alignas(64) Cell
  {
    std::atomic<std::size_t> sequence{0};
    T data{};
  };

  std::size_t m_mask;
  std::size_t m_capacity;
  std::unique_ptr<Cell[]> m_cells;

  alignas(64) std::atomic<std::size_t> m_enqueue{0};
  alignas(64) std::atomic<std::size_t> m_dequeue{0};
};

} // namespace fiberecs