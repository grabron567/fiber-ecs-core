#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "MemoryUtils.hpp"

namespace fiberecs
{

// Fixed-capacity ring allocator for producer/consumer handoff between two
// threads. Unlike a bump allocator it never needs the consumer to know when the
// producer wraps: the producer checks the consumer's release cursor before
// committing, so live data is never overwritten.
//
//   producer: allocate(), then publish()  -> hands bytes to the consumer
//   consumer: consume_start() .. consume_end() -> frees bytes back to the ring
//
// Single producer, single consumer. `reset()` rewinds both cursors in O(1).
//
// Published objects are tracked individually so that consume_start() always
// hands back exactly one object, no matter how far the producer has run ahead.
class RingArena
{
public:
  // Objects are tracked in a side ring of lengths, which is sized from the byte
  // capacity divided by this. Anything smaller cannot be published.
  static constexpr std::size_t kMinObjectBytes = 16;

  RingArena() = default;

  explicit RingArena(std::size_t bytes, std::size_t alignment = kCacheLineSize)
    : m_capacity(bytes)
    , m_alignment(alignment)
  {
    if (bytes == 0 || !is_power_of_two(alignment))
    {
      std::fprintf(stderr,
                   "[fiberecs] RingArena: invalid arguments (bytes=%zu alignment=%zu)\n",
                   bytes, alignment);
      std::abort();
    }

    void* raw = ::operator new(bytes, std::align_val_t{alignment});
    const std::uintptr_t addr =
        align_up(reinterpret_cast<std::uintptr_t>(raw),
                 alignment > kCacheLineSize ? alignment : kCacheLineSize);
    m_base = reinterpret_cast<std::byte*>(addr);
    const std::size_t slack = static_cast<std::size_t>(m_base - static_cast<std::byte*>(raw));

    // Both cursors are masked back into the ring, so the usable size has to be a
    // power of two. Round down and keep at least one alignment block.
    m_capacity = (bytes - slack) & ~(alignment - 1);
    m_storage = raw;
    m_mask = m_capacity - 1;

    // One length slot per kMinObjectBytes of ring, rounded down to a power of
    // two so the producer and consumer can mask instead of dividing.
    m_length_capacity = round_down_pow2(m_capacity / kMinObjectBytes);
    if (m_length_capacity == 0)
    {
      std::fprintf(stderr,
                   "[fiberecs] RingArena: capacity %zuB is too small to publish "
                   "objects of %zuB\n",
                   m_capacity, kMinObjectBytes);
      std::abort();
    }
    m_length_mask = m_length_capacity - 1;
    m_lengths = static_cast<std::size_t*>(::operator new(m_length_capacity * sizeof(std::size_t)));

    m_write.store(0, std::memory_order_relaxed);
    m_read.store(0, std::memory_order_relaxed);
  }

  ~RingArena()
  {
    if (m_storage != nullptr)
    {
      ::operator delete(m_storage, std::align_val_t{m_alignment});
    }
    if (m_lengths != nullptr)
    {
      ::operator delete(m_lengths);
    }
  }

  RingArena(const RingArena&) = delete;
  RingArena& operator=(const RingArena&) = delete;
  RingArena(RingArena&&) = delete;
  RingArena& operator=(RingArena&&) = delete;

  // ---- producer side -----------------------------------------------------

  // Caller must pair with publish() once the bytes are initialised. Returns
  // nullptr when the ring is full; the producer is expected to retry once the
  // consumer has drained some space, so a full ring is backpressure rather than
  // an error.
  [[nodiscard]] void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t))
  {
    if (bytes == 0)
    {
      // A zero-byte request consumes nothing; make the matching publish() a no-op
      // instead of leaving a stale cursor behind.
      m_pending_size = 0;
      m_pending_write = m_write.load(std::memory_order_relaxed);
      return m_base;
    }

    if (!is_power_of_two(alignment))
    {
      std::fprintf(stderr, "[fiberecs] RingArena: alignment %zu is not a power of two\n",
                   alignment);
      std::abort();
    }

    if (bytes < kMinObjectBytes)
    {
      std::fprintf(stderr, "[fiberecs] RingArena: object of %zuB is below the %zuB minimum\n",
                   bytes, kMinObjectBytes);
      std::abort();
    }

    const std::size_t write = m_write.load(std::memory_order_relaxed);
    const std::size_t read = m_read.load(std::memory_order_acquire);
    const std::size_t free_bytes = m_capacity - (write - read);

    // Round the span up to the alignment instead of padding in front of the
    // payload: both cursors then move by whole spans, so consume_end() reclaims
    // the slack as well and the payload starts exactly at the span start.
    const std::size_t span = align_up(bytes, alignment);
    const std::size_t ring_pos = write & m_mask;

    // The span has to sit entirely inside the buffer, so when it would not fit
    // before the end of the ring, skip the tail and let it start the next lap.
    // The cursors are absolute and only ever masked back into the ring, so
    // running past m_capacity is harmless; writing past the allocation is not.
    const std::size_t tail_skip = (ring_pos + span > m_capacity) ? (m_capacity - ring_pos) : 0;

    const std::size_t total = tail_skip + span;
    if (total > free_bytes)
    {
      // Recorded, not fatal: the producer retries once the consumer has drained.
      m_overflow.store(true, std::memory_order_relaxed);
      return nullptr;
    }

    m_pending_size = span;
    m_pending_write = write + tail_skip;
    m_allocations.fetch_add(1, std::memory_order_relaxed);

    return m_base + ring_pos;
  }

  // Make the last allocate() visible to the consumer.
  void publish() noexcept
  {
    const std::size_t seq = m_seq.load(std::memory_order_relaxed);
    // Record the boundary before announcing it, so a consumer that observes the
    // new sequence also observes the length.
    m_lengths[seq & m_length_mask] = m_pending_size;
    const std::size_t new_write = m_pending_write + m_pending_size;
    m_pending_size = 0;
    m_write.store(new_write, std::memory_order_relaxed);
    m_seq.store(seq + 1, std::memory_order_release);
  }

  // ---- consumer side -----------------------------------------------------

  // Pointer to the oldest published object, or nullptr when the ring is empty.
  // Holds until consume_end().
  [[nodiscard]] const void* consume_start() noexcept
  {
    // m_seq is the synchronisation point: acquiring it also makes the length and
    // the bytes of the object visible.
    if (m_seq.load(std::memory_order_acquire) == m_seq_read)
    {
      return nullptr;
    }
    const std::size_t read = m_read.load(std::memory_order_relaxed);
    m_consuming = m_lengths[m_seq_read & m_length_mask];
    return m_base + (read & m_mask);
  }

  void consume_end() noexcept
  {
    const std::size_t read = m_read.load(std::memory_order_relaxed) + m_consuming;
    m_consuming = 0;
    ++m_seq_read;
    m_read.store(read, std::memory_order_release);
  }

  // ---- lifecycle ---------------------------------------------------------

  // O(1): both cursors rewind, nothing is freed.
  void reset() noexcept
  {
    m_write.store(0, std::memory_order_relaxed);
    m_read.store(0, std::memory_order_relaxed);
    m_seq.store(0, std::memory_order_relaxed);
    m_seq_read = 0;
    m_allocations.store(0, std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
  [[nodiscard]] std::size_t pending_bytes() const noexcept
  {
    return m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::size_t allocation_count() const noexcept
  {
    return m_allocations.load(std::memory_order_relaxed);
  }
  [[nodiscard]] bool hard_overflow() const noexcept
  {
    return m_overflow.load(std::memory_order_relaxed);
  }

  // Diagnostic hooks (defined in RingArena.cpp).
  static void report_reset(const RingArena& arena) noexcept;
  static std::size_t validate(const RingArena& arena) noexcept;

private:
  void* m_storage{nullptr};
  std::byte* m_base{nullptr};
  std::size_t m_capacity{0};
  std::size_t m_mask{0};
  std::size_t m_alignment{kCacheLineSize};

  // Length of each published object, indexed by sequence number.
  std::size_t* m_lengths{nullptr};
  std::size_t m_length_capacity{0};
  std::size_t m_length_mask{0};

  alignas(64) std::atomic<std::size_t> m_write{0};
  alignas(64) std::atomic<std::size_t> m_read{0};

  std::size_t m_pending_size{0};
  std::size_t m_pending_write{0};
  std::size_t m_consuming{0};

  // m_seq counts published objects (producer side), m_seq_read counts consumed
  // ones (consumer side). m_seq is the release/acquire hand-off.
  alignas(64) std::atomic<std::size_t> m_seq{0};
  std::size_t m_seq_read{0};

  std::atomic<std::size_t> m_allocations{0};
  std::atomic<bool> m_overflow{false};
};

} // namespace fiberecs