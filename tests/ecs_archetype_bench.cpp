// fiberecs -- archetypal ECS throughput benchmark.
//
//   ecs_archetype_bench [entity_count]
//
// Builds a single 100,000-entity archetype, then runs a frame of three
// non-conflicting systems (movement, health regen, shield decay) as parallel
// chunk jobs over the work-stealing scheduler, warmup first and then timed.
//
// Three things are measured: wall time per frame against the 2-5 ms design
// target, the memory traffic implied by the SoA layout (bytes per entity per
// frame, working-set size, resulting bandwidth), and -- decisively -- global
// heap traffic during the timed section. Systems operate on arena-owned
// chunks, so a steady-state frame must call operator new zero times; the
// replaced allocator below counts every call in this binary.
//
// Exit status: 0 when the frame time stays within target, the timed section
// performed no heap allocation, and every entity matches the serial arithmetic
// exactly.

#include "ecs/dispatcher.hpp"
#include "ecs/world.hpp"
#include "job/JobScheduler.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>

#if defined(_WIN32)
#include <malloc.h>
#endif

// ---------------------------------------------------------------------------
// Heap accounting (same probe as main.cpp: every operator new/delete in this
// binary is counted).
// ---------------------------------------------------------------------------
namespace alloc_probe
{
std::atomic<std::uint64_t> g_allocations{0};
std::atomic<std::uint64_t> g_deallocations{0};
std::atomic<std::uint64_t> g_bytes{0};

inline void record(std::size_t bytes) noexcept
{
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  g_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

struct Snapshot
{
  std::uint64_t allocations;
  std::uint64_t deallocations;
  std::uint64_t bytes;
};

inline Snapshot take() noexcept
{
  return {g_allocations.load(std::memory_order_relaxed),
          g_deallocations.load(std::memory_order_relaxed), g_bytes.load(std::memory_order_relaxed)};
}

void* aligned(std::size_t size, std::size_t alignment) noexcept;
void aligned_free(void* p) noexcept;
} // namespace alloc_probe

// GCC's -Wmismatched-new-delete misreads a malloc/free forwarding replacement
// as a mismatched pair, so the diagnostic is silenced for this block only.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

namespace alloc_probe
{
void* aligned(std::size_t size, std::size_t alignment) noexcept
{
  record(size);
  const std::size_t align = alignment < sizeof(void*) ? sizeof(void*) : alignment;
  const std::size_t bytes = size == 0 ? 1 : size;
#if defined(_WIN32)
  return _aligned_malloc(bytes, align);
#else
  void* p = nullptr;
  if (posix_memalign(&p, align, bytes) != 0)
  {
    return nullptr;
  }
  return p;
#endif
}

void aligned_free(void* p) noexcept
{
  if (p == nullptr)
  {
    return;
  }
  g_deallocations.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
  _aligned_free(p);
#else
  std::free(p);
#endif
}
} // namespace alloc_probe

void* operator new(std::size_t size)
{
  alloc_probe::record(size);
  void* p = std::malloc(size == 0 ? 1 : size);
  if (p == nullptr)
  {
    throw std::bad_alloc();
  }
  return p;
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  alloc_probe::record(size);
  return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept
{
  return ::operator new(size, tag);
}

void* operator new(std::size_t size, std::align_val_t align)
{
  void* p = alloc_probe::aligned(size, static_cast<std::size_t>(align));
  if (p == nullptr)
  {
    throw std::bad_alloc();
  }
  return p;
}

void* operator new[](std::size_t size, std::align_val_t align)
{
  return ::operator new(size, align);
}

void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept
{
  return alloc_probe::aligned(size, static_cast<std::size_t>(align));
}

void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept
{
  return ::operator new(size, align, std::nothrow);
}

void operator delete(void* p) noexcept
{
  if (p != nullptr)
  {
    alloc_probe::g_deallocations.fetch_add(1, std::memory_order_relaxed);
  }
  std::free(p);
}

void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }

void operator delete(void* p, std::align_val_t) noexcept { alloc_probe::aligned_free(p); }
void operator delete[](void* p, std::align_val_t a) noexcept
{
  alloc_probe::aligned_free(p);
  static_cast<void>(a);
}
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept
{
  alloc_probe::aligned_free(p);
  static_cast<void>(a);
}
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept
{
  alloc_probe::aligned_free(p);
  static_cast<void>(a);
}
void operator delete(void* p, std::align_val_t a, const std::nothrow_t&) noexcept
{
  alloc_probe::aligned_free(p);
  static_cast<void>(a);
}
void operator delete[](void* p, std::align_val_t a, const std::nothrow_t&) noexcept
{
  alloc_probe::aligned_free(p);
  static_cast<void>(a);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

// ---------------------------------------------------------------------------
namespace
{
using Clock = std::chrono::steady_clock;
namespace ecs = fiberecs::ecs;

struct Position
{
  float x;
  float y;
  float z;
};

struct Velocity
{
  float x;
  float y;
  float z;
};

struct Health
{
  float value;
};

struct Shield
{
  float value;
};

constexpr int kDefaultEntities = 100000;
constexpr int kWarmupFrames = 3;
constexpr int kTimedFrames = 10;
constexpr int kSystemsPerFrame = 3;

// One frame's memory traffic per entity:
//   movement  12 B read (Velocity) + 12 B read-modify-write (Position)
//   regen      4 B read-modify-write (Health)
//   shield     4 B read-modify-write (Shield)
constexpr double kBytesPerEntityPerFrame = 40.0;

double ms_since(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double ms_between(Clock::time_point start, Clock::time_point end)
{
  return std::chrono::duration<double, std::milli>(end - start).count();
}

// Replays the exact float operation the systems perform, so the verifier can
// demand bit-identical results instead of a fuzzy tolerance.
float repeat_op(float initial, float delta, int frames)
{
  float value = initial;
  for (int frame = 0; frame < frames; ++frame)
  {
    value += delta;
  }
  return value;
}
} // namespace

int main(int argc, char** argv)
{
  int entity_count = kDefaultEntities;
  if (argc > 1)
  {
    const int parsed = std::atoi(argv[1]);
    if (parsed > 0)
    {
      entity_count = parsed;
    }
  }

  std::printf("================================================================\n");
  std::printf(" fiberecs archetypal ECS benchmark\n");
  std::printf("================================================================\n");

  fiberecs::JobScheduler scheduler;
  scheduler.initialize();

  ecs::World world;
  const Clock::time_point spawn_start = Clock::now();
  for (int i = 0; i < entity_count; ++i)
  {
    world.spawn_with(Position{static_cast<float>(i), 0.0f, 0.0f}, Velocity{1.0f, -0.5f, 0.25f},
                     Health{100.0f}, Shield{50.0f});
  }
  const double spawn_ms = ms_since(spawn_start);

  std::size_t chunks = 0;
  std::size_t rows_of_capacity = 0;
  double working_set_bytes = 0.0;
  world.each_archetype([&](const ecs::Signature&, const ecs::Archetype& archetype_instance) {
    chunks += archetype_instance.chunk_count();
    rows_of_capacity += static_cast<std::size_t>(archetype_instance.chunk_capacity()) *
                        archetype_instance.chunk_count();
    working_set_bytes += static_cast<double>(archetype_instance.occupied_bytes());
  });

  const double fill_percent =
      rows_of_capacity == 0
          ? 0.0
          : 100.0 * static_cast<double>(entity_count) / static_cast<double>(rows_of_capacity);

  // -------------------------------------------------------------------------
  // Systems: pairwise disjoint writers, so the access gate never serialises
  // them against each other.
  // -------------------------------------------------------------------------
  ecs::Dispatcher dispatch(world, scheduler);

  const auto movement = [](auto& view) {
    auto* const position = view.template data<Position>();
    const auto* const velocity = view.template data<Velocity>();
    const std::uint32_t rows = view.count();
    for (std::uint32_t row = 0; row < rows; ++row)
    {
      position[row].x += velocity[row].x;
      position[row].y += velocity[row].y;
      position[row].z += velocity[row].z;
    }
  };

  const auto regen = [](auto& view) {
    auto* const health = view.template data<Health>();
    const std::uint32_t rows = view.count();
    for (std::uint32_t row = 0; row < rows; ++row)
    {
      health[row].value -= 0.001f;
    }
  };

  const auto shield_decay = [](auto& view) {
    auto* const shield = view.template data<Shield>();
    const std::uint32_t rows = view.count();
    for (std::uint32_t row = 0; row < rows; ++row)
    {
      shield[row].value += 0.002f;
    }
  };

  const auto run_frame = [&] {
    dispatch.run<ecs::Write<Position>, ecs::Read<Velocity>>(movement);
    dispatch.run<ecs::Write<Health>>(regen);
    dispatch.run<ecs::Write<Shield>>(shield_decay);
  };

  // -------------------------------------------------------------------------
  // Warmup: pays every first-call cost (signature/access statics, task-slot
  // and chunk-list buffers, the first batch arena block) before the probe is
  // armed. drain() between frames guarantees the submission arena is fully
  // rewound, which is what makes the zero-allocation claim deterministic
  // rather than a race against the last job's bookkeeping.
  // -------------------------------------------------------------------------
  for (int frame = 0; frame < kWarmupFrames; ++frame)
  {
    scheduler.drain();
    run_frame();
  }
  scheduler.drain();

  const alloc_probe::Snapshot before = alloc_probe::take();
  const std::uint64_t jobs_before = scheduler.jobs_executed();

  std::array<double, kTimedFrames> frame_times{};
  double total_ms = 0.0;
  double min_ms = 1.0e30;
  double max_ms = 0.0;
  for (int frame = 0; frame < kTimedFrames; ++frame)
  {
    scheduler.drain();
    const Clock::time_point start = Clock::now();
    run_frame();
    const double elapsed = ms_between(start, Clock::now());
    frame_times[static_cast<std::size_t>(frame)] = elapsed;
    total_ms += elapsed;
    min_ms = elapsed < min_ms ? elapsed : min_ms;
    max_ms = elapsed > max_ms ? elapsed : max_ms;
  }

  const std::uint64_t jobs_timed = scheduler.jobs_executed() - jobs_before;
  const alloc_probe::Snapshot after = alloc_probe::take();
  scheduler.drain();

  // -------------------------------------------------------------------------
  // Verification: every entity must match the serial replay exactly.
  // -------------------------------------------------------------------------
  const int total_frames = kWarmupFrames + kTimedFrames;
  const float expected_y = repeat_op(0.0f, -0.5f, total_frames);
  const float expected_z = repeat_op(0.0f, 0.25f, total_frames);
  const float expected_health = repeat_op(100.0f, -0.001f, total_frames);
  const float expected_shield = repeat_op(50.0f, 0.002f, total_frames);

  int mismatches = 0;
  double checksum = 0.0;
  std::size_t visited = 0;
  world.each([&](ecs::Entity handle) {
    const float expected_x =
        static_cast<float>(handle.index) + static_cast<float>(total_frames);
    const Position& position = world.get<Position>(handle);
    const Velocity& velocity = world.get<Velocity>(handle);
    const Health& health = world.get<Health>(handle);
    const Shield& shield = world.get<Shield>(handle);
    if (position.x != expected_x || position.y != expected_y || position.z != expected_z ||
        velocity.x != 1.0f || velocity.y != -0.5f || velocity.z != 0.25f ||
        health.value != expected_health || shield.value != expected_shield)
    {
      ++mismatches;
    }
    checksum += static_cast<double>(position.x);
    ++visited;
  });

  const double average_ms = total_ms / kTimedFrames;
  const double traffic_bytes =
      static_cast<double>(entity_count) * kBytesPerEntityPerFrame;
  const double bandwidth_gbs = traffic_bytes / (average_ms / 1000.0) / 1.0e9;
  const double entities_per_second = static_cast<double>(entity_count) * 1000.0 / average_ms;
  const double entity_systems_per_second =
      entities_per_second * static_cast<double>(kSystemsPerFrame);

  const std::uint64_t heap_allocations = after.allocations - before.allocations;
  const std::uint64_t heap_bytes = after.bytes - before.bytes;
  const std::uint64_t heap_frees = after.deallocations - before.deallocations;

  const bool zero_heap = heap_allocations == 0;
  const bool correct = mismatches == 0 && visited == static_cast<std::size_t>(entity_count);
  const bool within_target = average_ms <= 5.0;

  // -------------------------------------------------------------------------
  // Report
  // -------------------------------------------------------------------------
  std::printf("  %-26s %d\n", "entities", entity_count);
  std::printf("  %-26s %d\n", "archetypes", static_cast<int>(world.archetype_count()));
  std::printf("  %-26s %zu\n", "chunks", chunks);
  std::printf("  %-26s %zu rows (%.1f%% fill)\n", "chunk capacity",
              rows_of_capacity, fill_percent);
  std::printf("  %-26s %.1f KiB\n", "working set", working_set_bytes / 1024.0);
  std::printf("  %-26s %d workers\n", "scheduler", scheduler.worker_count());
  std::printf("  %-26s %.2f ms\n", "entity spawn", spawn_ms);
  std::printf("\n");
  std::printf("  %-26s %d\n", "systems per frame", kSystemsPerFrame);
  std::printf("  %-26s %d\n", "warmup frames", kWarmupFrames);
  std::printf("  %-26s %d\n", "timed frames", kTimedFrames);
  std::printf("  %-26s %.3f ms\n", "frame min", min_ms);
  std::printf("  %-26s", "frame times (ms)");
  for (int frame = 0; frame < kTimedFrames; ++frame)
  {
    std::printf(" %.3f", frame_times[static_cast<std::size_t>(frame)]);
  }
  std::printf("\n");
  std::printf("  %-26s %.3f ms\n", "frame avg", average_ms);
  std::printf("  %-26s %.3f ms\n", "frame max", max_ms);
  std::printf("  %-26s %.3f ms\n", "total timed", total_ms);
  std::printf("  %-26s %.2f M entity/s\n", "throughput", entities_per_second / 1.0e6);
  std::printf("  %-26s %.2f M entity-system/s\n", "system throughput",
              entity_systems_per_second / 1.0e6);
  std::printf("  %-26s %.1f KiB/frame\n", "traffic", traffic_bytes / 1024.0);
  std::printf("  %-26s %.2f GB/s\n", "memory bandwidth", bandwidth_gbs);
  std::printf("  %-26s %llu\n", "chunk jobs (timed)",
              static_cast<unsigned long long>(jobs_timed));
  std::printf("  %-26s %llu (%llu B, %llu frees)\n", "heap allocs (timed)",
              static_cast<unsigned long long>(heap_allocations),
              static_cast<unsigned long long>(heap_bytes),
              static_cast<unsigned long long>(heap_frees));
  std::printf("  %-26s %d (%.1f checksum)\n", "entity mismatches", mismatches, checksum);
  std::printf("\n");
  std::printf("  %-26s %s\n", "zero heap in timed section", zero_heap ? "yes" : "NO");
  std::printf("  %-26s %s\n", "results exact", correct ? "yes" : "NO");
  std::printf("  %-26s %s (target 2-5 ms)\n", "frame time", average_ms > 2.0 ? (within_target ? "within target" : "OVER TARGET") : (within_target ? "below target, still within 5 ms" : "OVER TARGET"));
  std::printf("================================================================\n");
  std::printf(" RESULT: %s\n", zero_heap && correct && within_target ? "PASS" : "FAIL");
  std::printf("================================================================\n");

  return zero_heap && correct && within_target ? 0 : 1;
}
