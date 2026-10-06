#include "RingArena.hpp"

namespace fiberecs
{

void RingArena::report_reset(const RingArena& arena) noexcept
{
  std::fprintf(stderr,
               "[fiberecs] RingArena::reset: released %zuB pending across %zu allocations\n",
               arena.pending_bytes(), arena.allocation_count());
}

std::size_t RingArena::validate(const RingArena& arena) noexcept
{
  bool ok = true;
  if (arena.capacity() == 0)
  {
    std::fprintf(stderr, "[fiberecs] RingArena::validate: zero-capacity arena\n");
    ok = false;
  }
  if (arena.pending_bytes() > arena.capacity())
  {
    std::fprintf(stderr, "[fiberecs] RingArena::validate: pending > capacity\n");
    ok = false;
  }
  return ok ? 1u : 0u;
}

} // namespace fiberecs