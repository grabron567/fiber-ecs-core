#include "BlockArena.hpp"

#include <cstdio>
#include <cstdlib>

namespace fiberecs
{

BlockArena::BlockArena(std::size_t block_bytes) noexcept : m_block_bytes(block_bytes)
{
  if (m_block_bytes == 0 || !is_power_of_two(m_block_bytes))
  {
    std::fprintf(stderr,
                 "[fiberecs] BlockArena: block_bytes=%zu must be a non-zero power of two\n",
                 block_bytes);
    std::abort();
  }
}

BlockArena::~BlockArena() = default;

LinearArena* BlockArena::reserve_block(std::size_t bytes, std::size_t alignment)
{
  // Worst-case inter-block padding is alignment-1, so requiring
  // bytes + alignment guarantees the request fits without aborting.
  const std::size_t needed = bytes + alignment;

  while (m_free_cursor < m_free.size())
  {
    LinearArena* const candidate = m_free[m_free_cursor++];
    if (candidate->remaining() >= needed)
    {
      return candidate;
    }
  }
  m_free.clear();
  m_free_cursor = 0;

  // Every recycled block was too small (possible only when a caller varies the
  // request size upwards between rounds); fall back to a fresh, larger block.
  std::size_t block_bytes = m_block_bytes;
  while (block_bytes < needed)
  {
    block_bytes <<= 1;
  }

  m_blocks.push_back(std::make_unique<LinearArena>(block_bytes));
  m_capacity += m_blocks.back()->capacity();
  return m_blocks.back().get();
}

void* BlockArena::allocate(std::size_t bytes, std::size_t alignment)
{
  if (bytes == 0)
  {
    return m_active != nullptr ? m_active->allocate(0) : nullptr;
  }

  if (!is_power_of_two(alignment))
  {
    std::fprintf(stderr, "[fiberecs] BlockArena: alignment %zu is not a power of two\n",
                 alignment);
    std::abort();
  }

  if (m_active == nullptr || m_active->remaining() < bytes + alignment)
  {
    m_active = reserve_block(bytes, alignment);
    if (m_active == nullptr)
    {
      return nullptr;
    }
  }

  const std::size_t before = m_active->used();
  void* const p = m_active->allocate(bytes, alignment);
  if (p == nullptr)
  {
    return nullptr;
  }

  ++m_allocations;
  m_used += m_active->used() - before;
  return p;
}

void BlockArena::reset() noexcept
{
  m_free.clear();
  m_free_cursor = 0;
  for (const auto& block : m_blocks)
  {
    block->reset();
    m_free.push_back(block.get());
  }
  m_free_cursor = 0;
  m_active = m_blocks.empty() ? nullptr : m_blocks.front().get();
  m_allocations = 0;
  m_used = 0;
}

std::size_t BlockArena::used() const noexcept { return m_used; }

std::size_t BlockArena::capacity() const noexcept { return m_capacity; }

std::size_t BlockArena::remaining() const noexcept
{
  return m_active != nullptr ? m_active->remaining() : m_capacity;
}

} // namespace fiberecs
