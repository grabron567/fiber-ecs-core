#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <vector>

#include "LinearArena.hpp"

namespace fiberecs
{

// Growable, chunked bump allocator built on top of LinearArena.
//
// A single fixed-size arena forces a hard ceiling on a burst: submitting
// 100,000 jobs from one thread while the workers drain them needs more room
// than any reasonable constant. BlockArena instead links 1 MiB blocks, taking a
// new one only when the current block is full, so memory tracks the real peak
// instead of a guess.
//
// reset() stays O(1) per block with no free() and no destructor calls: every
// block is rewound and returned to the free list, so steady-state operation
// performs zero allocator calls no matter how large previous bursts were.
class BlockArena
{
public:
  static constexpr std::size_t kDefaultBlockBytes = 1u << 20; // 1 MiB

  explicit BlockArena(std::size_t block_bytes = kDefaultBlockBytes) noexcept;
  ~BlockArena();

  BlockArena(const BlockArena&) = delete;
  BlockArena& operator=(const BlockArena&) = delete;
  BlockArena(BlockArena&&) = delete;
  BlockArena& operator=(BlockArena&&) = delete;

  // Returns nullptr only if the block size cannot satisfy the request.
  [[nodiscard]] void* allocate(std::size_t bytes,
                               std::size_t alignment = alignof(std::max_align_t));

  // Rewinds every block in O(number of blocks); never shrinks the pool.
  void reset() noexcept;

  [[nodiscard]] std::size_t used() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept;
  [[nodiscard]] std::size_t remaining() const noexcept;
  [[nodiscard]] std::size_t block_count() const noexcept { return m_blocks.size(); }
  [[nodiscard]] std::size_t block_bytes() const noexcept { return m_block_bytes; }
  [[nodiscard]] std::size_t allocation_count() const noexcept { return m_allocations; }

private:
  // Reserves a block with room for `bytes` at `alignment`, or nullptr.
  [[nodiscard]] LinearArena* reserve_block(std::size_t bytes, std::size_t alignment);

  std::size_t m_block_bytes;
  std::vector<std::unique_ptr<LinearArena>> m_blocks; // owning storage, in order
  std::vector<LinearArena*> m_free;                  // rewound blocks awaiting reuse
  std::size_t m_free_cursor{0};                      // next free block to try
  LinearArena* m_active{nullptr};
  std::size_t m_allocations{0};
  std::size_t m_used{0};
  std::size_t m_capacity{0};
};

} // namespace fiberecs
