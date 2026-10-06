#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <utility>

#include "MemoryUtils.hpp"

namespace fiberecs
{

// Bump allocator over one contiguous block. reset() is O(1): no destructor is
// run and no free() is issued, so the only cost is moving one pointer.
//
// Ownership model: a LinearArena is single-threaded. Concurrent producers must
// use one arena per thread, or serialise access themselves (see RingArena for
// the SPSC-friendly alternative).
class LinearArena
{
public:
  LinearArena() = default;

  explicit LinearArena(std::size_t bytes, std::size_t alignment = kCacheLineSize)
    : m_capacity(bytes)
  {
    if (bytes == 0 || !is_power_of_two(alignment))
    {
      std::fprintf(stderr,
                   "[fiberecs] LinearArena: invalid arguments (bytes=%zu alignment=%zu); "
                   "alignment must be a non-zero power of two\n",
                   bytes, alignment);
      std::abort();
    }

    // One allocation for the whole lifetime of the arena: after construction
    // the runtime performs zero calls into the allocator.
    void* raw = ::operator new(bytes, std::align_val_t{alignment});
    m_base = static_cast<std::byte*>(align_cache_line(raw));
    m_capacity -= static_cast<std::size_t>(static_cast<std::byte*>(m_base) -
                                          static_cast<std::byte*>(raw));
    m_storage = raw;
    m_alignment = alignment;

    m_current = m_base;
    m_end = m_base + m_capacity;
    m_high_water = 0;
  }

  ~LinearArena()
  {
    if (m_storage != nullptr)
    {
      ::operator delete(m_storage, std::align_val_t{m_alignment});
    }
  }

  LinearArena(const LinearArena&) = delete;
  LinearArena& operator=(const LinearArena&) = delete;
  LinearArena(LinearArena&&) = delete;
  LinearArena& operator=(LinearArena&&) = delete;

  // Returns nullptr on overflow instead of aborting; check `hard_overflow()`.
  [[nodiscard]] void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t))
  {
    if (bytes == 0)
    {
      return m_current;
    }

    if (!is_power_of_two(alignment))
    {
      std::fprintf(stderr, "[fiberecs] LinearArena: alignment %zu is not a power of two\n",
                   alignment);
      std::abort();
    }

    // Capacity is measured from m_base, so the request must be validated
    // against the live offset -- not against the arena's total size.
    const std::size_t offset = static_cast<std::size_t>(m_current - m_base);
    const std::size_t available = m_capacity - offset;
    const std::size_t pad = padding_for(reinterpret_cast<std::uintptr_t>(m_current), alignment);
    if (pad > available || bytes > available - pad)
    {
      report_overflow(bytes, alignment);
      return nullptr;
    }

    std::byte* const p = m_current + pad;
    m_current = p + bytes;

    const std::size_t used_now = static_cast<std::size_t>(m_current - m_base);
    if (used_now > m_high_water)
    {
      m_high_water = used_now;
    }
    m_allocations.fetch_add(1, std::memory_order_relaxed);
    return p;
  }

  // Placement helper: constructs T in arena storage.
  template <typename T, typename... Args>
  [[nodiscard]] T* emplace(Args&&... args)
  {
    void* mem = allocate(sizeof(T), alignof(T));
    if (mem == nullptr)
    {
      return nullptr;
    }
    return ::new (mem) T(std::forward<Args>(args)...);
  }

  // O(1) reset: no free(), no destructor calls.
  void reset() noexcept
  {
    m_current = m_base;
    m_allocations.store(0, std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t used() const noexcept
  {
    return static_cast<std::size_t>(m_current - m_base);
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
  [[nodiscard]] std::size_t remaining() const noexcept { return m_capacity - used(); }
  [[nodiscard]] std::size_t high_water() const noexcept { return m_high_water; }
  [[nodiscard]] std::size_t allocation_count() const noexcept
  {
    return m_allocations.load(std::memory_order_relaxed);
  }
  [[nodiscard]] bool hard_overflow() const noexcept
  {
    return m_overflow.load(std::memory_order_relaxed);
  }

  // Diagnostic hooks (defined in LinearArena.cpp).
  static void report_reset(const LinearArena& arena) noexcept;
  static std::size_t validate(const LinearArena& arena) noexcept;

private:
  void report_overflow(std::size_t bytes, std::size_t alignment) noexcept
  {
    m_overflow.store(true, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[fiberecs] FATAL LinearArena overflow: request=%zuB align=%zuB | "
                 "used=%zuB high_water=%zuB capacity=%zuB remaining=%zuB\n",
                 bytes, alignment, used(), m_high_water, m_capacity, remaining());
    std::fflush(stderr);
    std::abort();
  }

  void* m_storage{nullptr};
  std::byte* m_base{nullptr};
  std::byte* m_current{nullptr};
  std::byte* m_end{nullptr};
  std::size_t m_capacity{0};
  std::size_t m_alignment{kCacheLineSize};
  std::size_t m_high_water{0};
  std::atomic<std::size_t> m_allocations{0};
  std::atomic<bool> m_overflow{false};
};

} // namespace fiberecs