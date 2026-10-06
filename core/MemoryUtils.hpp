#pragma once

#include <cstddef>
#include <cstdint>

namespace fiberecs
{

inline constexpr std::size_t kCacheLineSize = 64;

[[nodiscard]] constexpr bool is_power_of_two(std::size_t v) noexcept
{
  return v != 0 && (v & (v - 1)) == 0;
}

// Rounds `value` up to the next multiple of `alignment` (a power of two).
[[nodiscard]] constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept
{
  return (value + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]] constexpr std::size_t align_down(std::size_t value, std::size_t alignment) noexcept
{
  return value & ~(alignment - 1);
}

// Largest power of two <= value (0 when value is 0). Used to size ring buffers
// so their cursors can be masked instead of divided. Written as a portable
// constexpr loop: __builtin_clzll does not exist on MSVC, and this only runs
// when an arena is constructed.
[[nodiscard]] constexpr std::size_t round_down_pow2(std::size_t value) noexcept
{
  if (value == 0)
  {
    return 0;
  }
  std::size_t result = 1;
  while ((result << 1) != 0 && (result << 1) <= value)
  {
    result <<= 1;
  }
  return result;
}

// Cache-line alignment: every arena slot and every hot scheduler field starts
// on its own 64-byte line so neighbouring workers never share one.
inline void* align_cache_line(void* p) noexcept
{
  const auto addr = reinterpret_cast<std::uintptr_t>(p);
  return reinterpret_cast<void*>((addr + kCacheLineSize - 1) & ~(kCacheLineSize - 1));
}

// Number of bytes that must be *added* to `address` for it to satisfy
// `alignment` (a power of two). This is deliberately a delta, not an aligned
// absolute address: bump allocators advance a cursor by this much.
[[nodiscard]] inline std::size_t padding_for(std::uintptr_t address, std::size_t alignment) noexcept
{
  return align_up(address, alignment) - address;
}

} // namespace fiberecs