#include "LinearArena.hpp"

namespace fiberecs
{

// Out-of-line diagnostics so the header stays free of reporting code while the
// hot allocate()/reset() paths remain inline.
void LinearArena::report_reset(const LinearArena& arena) noexcept
{
  std::fprintf(stderr,
               "[fiberecs] LinearArena::reset: reclaimed %zuB (peak %zuB) across %zu allocations\n",
               arena.used(), arena.high_water(), arena.allocation_count());
}

std::size_t LinearArena::validate(const LinearArena& arena) noexcept
{
  bool ok = true;
  if (arena.capacity() == 0)
  {
    std::fprintf(stderr, "[fiberecs] LinearArena::validate: zero-capacity arena\n");
    ok = false;
  }
  if (arena.used() > arena.capacity())
  {
    std::fprintf(stderr, "[fiberecs] LinearArena::validate: used > capacity\n");
    ok = false;
  }
  if (arena.high_water() > arena.capacity())
  {
    std::fprintf(stderr, "[fiberecs] LinearArena::validate: high_water > capacity\n");
    ok = false;
  }
  return ok ? 1u : 0u;
}

} // namespace fiberecs