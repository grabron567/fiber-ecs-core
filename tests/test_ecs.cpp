#include "TestSupport.hpp"

#include "ecs/dispatcher.hpp"
#include "ecs/world.hpp"
#include "job/JobScheduler.hpp"

#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

using fiberecs::JobScheduler;
namespace ecs = fiberecs::ecs;
using ecs::Read;
using ecs::Write;

namespace
{

struct Position
{
  float x{0};
  float y{0};
  float z{0};
};

struct Velocity
{
  float x{0};
  float y{0};
  float z{0};
};

struct Health
{
  float value{0};
};

struct alignas(128) GridCell
{
  std::uint64_t lo{0};
  std::uint64_t hi{0};
};

// Compile-time proof of the access contract: Write<T> hands out T*, Read<T>
// (or a bare T) hands out const T*. Called from inside a matching dispatch so
// the view type really is ChunkView<Write<Position>, Read<Velocity>>.
template <typename View>
void expect_access_spec(View& view)
{
  static_assert(std::is_same_v<decltype(view.template data<Position>()), Position*>,
                "Write<Position> must expose a mutable column");
  static_assert(std::is_same_v<decltype(view.template data<Velocity>()), const Velocity*>,
                "Read<Velocity> must expose a const column");
  FIBERECS_CHECK(view.template data<Position>() != nullptr);
  FIBERECS_CHECK(view.template data<Velocity>() != nullptr);
}

} // namespace

FIBERECS_TEST_CASE("ECS: spawn, validate, destroy and recycle entity slots")
{
  ecs::World world;

  const ecs::Entity first = world.spawn<Position>();
  const ecs::Entity second = world.spawn<Position>();
  const ecs::Entity third = world.spawn<Position>();
  FIBERECS_CHECK_EQ(world.alive(), std::size_t{3});
  FIBERECS_CHECK(world.valid(first));
  FIBERECS_CHECK(world.valid(second));
  FIBERECS_CHECK(world.valid(third));

  FIBERECS_CHECK(world.destroy(second));
  FIBERECS_CHECK(!world.valid(second));
  FIBERECS_CHECK_EQ(world.alive(), std::size_t{2});

  // Destroying a stale handle is a reported no-op, never an abort.
  FIBERECS_CHECK(!world.destroy(second));
  FIBERECS_CHECK_EQ(world.alive(), std::size_t{2});

  // The free list is LIFO, so the next spawn reuses the hole -- but with a
  // bumped generation, which is what keeps the stale handle stale.
  const ecs::Entity recycled = world.spawn<Position>();
  FIBERECS_CHECK_EQ(recycled.index, second.index);
  FIBERECS_CHECK(recycled != second);
  FIBERECS_CHECK(!world.valid(second));
  FIBERECS_CHECK(world.valid(recycled));
  FIBERECS_CHECK_EQ(world.alive(), std::size_t{3});

  // An out-of-range handle never validates.
  const ecs::Entity stranger{ecs::Entity::kInvalidIndex, 0};
  FIBERECS_CHECK(!world.valid(stranger));
}

FIBERECS_TEST_CASE("ECS: components default-construct and store supplied values")
{
  ecs::World world;

  const ecs::Entity bare = world.spawn();
  FIBERECS_CHECK(!world.has<Position>(bare));
  FIBERECS_CHECK(world.try_get<Position>(bare) == nullptr);

  const ecs::Entity typed = world.spawn<Position, Health>();
  FIBERECS_CHECK(world.has<Position>(typed));
  FIBERECS_CHECK(world.has<Health>(typed));
  FIBERECS_CHECK(!world.has<Velocity>(typed));
  FIBERECS_CHECK_EQ(world.get<Position>(typed).x, 0.0f);
  FIBERECS_CHECK_EQ(world.get<Health>(typed).value, 0.0f);

  const ecs::Entity valued =
      world.spawn_with(Position{1.0f, 2.0f, 3.0f}, Health{42.0f}, Velocity{-1.0f, -2.0f, -3.0f});
  FIBERECS_CHECK_EQ(world.get<Position>(valued).x, 1.0f);
  FIBERECS_CHECK_EQ(world.get<Position>(valued).y, 2.0f);
  FIBERECS_CHECK_EQ(world.get<Position>(valued).z, 3.0f);
  FIBERECS_CHECK_EQ(world.get<Health>(valued).value, 42.0f);
  FIBERECS_CHECK_EQ(world.get<Velocity>(valued).x, -1.0f);

  Position* const pointer = world.try_get<Position>(valued);
  FIBERECS_CHECK(pointer != nullptr);
  pointer->x = 7.0f;
  FIBERECS_CHECK_EQ(world.get<Position>(valued).x, 7.0f);
}

FIBERECS_TEST_CASE("ECS: add and remove move the entity between archetypes")
{
  ecs::World world;
  const ecs::Entity instance = world.spawn_with(Position{5.0f, 6.0f, 7.0f});
  FIBERECS_CHECK_EQ(world.archetype_count(), std::size_t{1});

  world.add<Velocity>(instance, Velocity{1.0f, 2.0f, 3.0f});
  FIBERECS_CHECK_EQ(world.archetype_count(), std::size_t{2});
  FIBERECS_CHECK(world.has<Velocity>(instance));
  // The shared column must have been carried across the transition.
  FIBERECS_CHECK_EQ(world.get<Position>(instance).x, 5.0f);
  FIBERECS_CHECK_EQ(world.get<Position>(instance).y, 6.0f);
  FIBERECS_CHECK_EQ(world.get<Velocity>(instance).z, 3.0f);

  // Re-adding a component the entity already owns is a plain assignment: no
  // third archetype, no data loss.
  world.add<Velocity>(instance, Velocity{9.0f, 9.0f, 9.0f});
  FIBERECS_CHECK_EQ(world.archetype_count(), std::size_t{2});
  FIBERECS_CHECK_EQ(world.get<Velocity>(instance).x, 9.0f);
  FIBERECS_CHECK_EQ(world.get<Position>(instance).x, 5.0f);

  world.remove<Velocity>(instance);
  FIBERECS_CHECK(!world.has<Velocity>(instance));
  FIBERECS_CHECK_EQ(world.get<Position>(instance).x, 5.0f);
  // Archetypes are retained once created; the emptied one just stops matching
  // queries, which the next check covers.
  FIBERECS_CHECK_EQ(world.archetype_count(), std::size_t{2});
  FIBERECS_CHECK_EQ(world.query<Velocity>().size(), std::size_t{0});
  FIBERECS_CHECK_EQ(world.query<Position>().size(), std::size_t{1});
}

FIBERECS_TEST_CASE("ECS: destroy repairs the location swap-and-pop leaves behind")
{
  constexpr int kCount = 3000;
  constexpr int kStride = 3;

  ecs::World world;
  std::vector<ecs::Entity> handles;
  handles.reserve(kCount);
  for (int i = 0; i < kCount; ++i)
  {
    handles.push_back(world.spawn_with(Position{static_cast<float>(i), 0.0f, 0.0f},
                                       Health{static_cast<float>(i) + 1.0f}));
  }

  int destroy_failures = 0;
  for (int i = 0; i < kCount; i += kStride)
  {
    if (!world.destroy(handles[static_cast<std::size_t>(i)]))
    {
      ++destroy_failures;
    }
  }
  FIBERECS_CHECK_EQ(destroy_failures, 0);
  FIBERECS_CHECK_EQ(world.alive(), std::size_t{kCount} - std::size_t{kCount / kStride});

  // Every survivor must still read its own components: a missed location
  // repair shows up here as another entity's data.
  int mismatches = 0;
  for (int i = 0; i < kCount; ++i)
  {
    const ecs::Entity handle = handles[static_cast<std::size_t>(i)];
    const bool should_be_dead = (i % kStride) == 0;
    if (should_be_dead)
    {
      if (world.valid(handle))
      {
        ++mismatches;
      }
      continue;
    }
    if (!world.valid(handle))
    {
      ++mismatches;
      continue;
    }
    if (world.get<Position>(handle).x != static_cast<float>(i))
    {
      ++mismatches;
    }
    if (world.get<Health>(handle).value != static_cast<float>(i) + 1.0f)
    {
      ++mismatches;
    }
  }
  FIBERECS_CHECK_EQ(mismatches, 0);

  // every() must report exactly the live set.
  std::size_t visited = 0;
  world.each([&visited](ecs::Entity handle) {
    if (handle.valid())
    {
      ++visited;
    }
  });
  FIBERECS_CHECK_EQ(visited, world.alive());
}

FIBERECS_TEST_CASE("ECS: queries visit every matching entity exactly once")
{
  constexpr int kHalf = 1500;

  ecs::World world;
  std::vector<ecs::Entity> handles;
  handles.reserve(static_cast<std::size_t>(kHalf * 2));
  for (int i = 0; i < kHalf; ++i)
  {
    handles.push_back(world.spawn_with(Position{static_cast<float>(i), 0.0f, 0.0f}));
  }
  for (int i = 0; i < kHalf; ++i)
  {
    handles.push_back(
        world.spawn_with(Position{static_cast<float>(kHalf + i), 1.0f, 0.0f}, Velocity{1.0f, 0.0f, 0.0f}));
  }
  FIBERECS_CHECK_EQ(world.alive(), std::size_t{kHalf * 2});
  FIBERECS_CHECK_EQ(world.archetype_count(), std::size_t{2});

  std::vector<bool> seen(handles.size(), false);
  int wrong_archetype = 0;
  std::size_t visited = 0;
  for (ecs::Chunk* const chunk : world.query<Position, Velocity>())
  {
    for (std::uint32_t row = 0; row < chunk->count(); ++row)
    {
      const ecs::Entity handle = chunk->entity(row);
      if (static_cast<std::size_t>(handle.index) >= seen.size() || seen[handle.index])
      {
        ++wrong_archetype;
        continue;
      }
      seen[handle.index] = true;
      ++visited;
      if (!world.has<Velocity>(handle) || world.get<Position>(handle).y != 1.0f)
      {
        ++wrong_archetype;
      }
    }
  }
  FIBERECS_CHECK_EQ(wrong_archetype, 0);
  FIBERECS_CHECK_EQ(visited, std::size_t{kHalf});

  std::size_t all = 0;
  for (ecs::Chunk* const chunk : world.query<Position>())
  {
    all += chunk->count();
  }
  FIBERECS_CHECK_EQ(all, std::size_t{kHalf * 2});
}

FIBERECS_TEST_CASE("ECS: parallel dispatch updates every entity exactly once")
{
  constexpr int kCount = 20000;

  JobScheduler scheduler;
  scheduler.initialize();

  ecs::World world;
  std::vector<ecs::Entity> handles;
  handles.reserve(kCount);
  for (int i = 0; i < kCount; ++i)
  {
    handles.push_back(world.spawn_with(Position{static_cast<float>(i), 0.0f, 0.0f},
                                       Velocity{1.0f, 1.0f, 1.0f}));
  }

  ecs::Dispatcher dispatch(world, scheduler);
  const std::uint64_t jobs_before = scheduler.jobs_executed();

  dispatch.run<Write<Position>, Read<Velocity>>([](auto& view) {
    expect_access_spec(view);
    auto* const position = view.template data<Position>();
    const auto* const velocity = view.template data<Velocity>();
    const std::uint32_t rows = view.count();
    for (std::uint32_t row = 0; row < rows; ++row)
    {
      position[row].x += velocity[row].x;
      position[row].y += velocity[row].y;
      position[row].z += velocity[row].z;
    }
  });

  const std::uint64_t jobs = scheduler.jobs_executed() - jobs_before;
  // Exactly one job per non-empty chunk, and 20k entities cannot fit in one.
  FIBERECS_CHECK_EQ(jobs, dispatch.chunk_jobs_last_run());
  FIBERECS_CHECK(dispatch.chunk_jobs_last_run() > 1);

  int mismatches = 0;
  for (int i = 0; i < kCount; ++i)
  {
    const Position& position = world.get<Position>(handles[static_cast<std::size_t>(i)]);
    if (position.x != static_cast<float>(i) + 1.0f || position.y != 1.0f || position.z != 1.0f)
    {
      ++mismatches;
    }
  }
  FIBERECS_CHECK_EQ(mismatches, 0);

  // A second run over the same world must be idempotent in shape: same chunk
  // count, no structural change, no leaked tasks.
  const std::size_t chunks_last = dispatch.chunk_jobs_last_run();
  dispatch.run<Write<Position>, Read<Velocity>>([](auto& view) {
    auto* const position = view.template data<Position>();
    for (std::uint32_t row = 0; row < view.count(); ++row)
    {
      position[row].y += 1.0f;
    }
  });
  FIBERECS_CHECK_EQ(dispatch.chunk_jobs_last_run(), chunks_last);
  FIBERECS_CHECK_EQ(world.get<Position>(handles[10]).y, 2.0f);
  FIBERECS_CHECK_EQ(world.alive(), std::size_t{kCount});

  scheduler.drain();
}

FIBERECS_TEST_CASE("ECS: dispatching over an empty world is a no-op")
{
  JobScheduler scheduler;
  scheduler.initialize(2);

  ecs::World world;
  ecs::Dispatcher dispatch(world, scheduler);

  int invocations = 0;
  const std::uint64_t jobs_before = scheduler.jobs_executed();
  dispatch.run<Write<Position>>([&invocations](auto& view) {
    ++invocations;
    if (view.count() != 0)
    {
      ++invocations;
    }
  });
  FIBERECS_CHECK_EQ(dispatch.chunk_jobs_last_run(), std::size_t{0});
  FIBERECS_CHECK_EQ(scheduler.jobs_executed(), jobs_before);
  FIBERECS_CHECK_EQ(invocations, 0);

  // Entities exist but none match the declared components.
  for (int i = 0; i < 100; ++i)
  {
    world.spawn<Velocity>();
  }
  dispatch.run<Write<Position>>([&invocations](auto& view) {
    ++invocations;
    if (view.count() != 0)
    {
      ++invocations;
    }
  });
  FIBERECS_CHECK_EQ(dispatch.chunk_jobs_last_run(), std::size_t{0});
  FIBERECS_CHECK_EQ(scheduler.jobs_executed(), jobs_before);
  FIBERECS_CHECK_EQ(invocations, 0);

  scheduler.drain();
}

FIBERECS_TEST_CASE("ECS: access declarations detect read/write conflicts")
{
  const ecs::Access write_position = ecs::access_of<ecs::Write<Position>>();
  const ecs::Access read_position = ecs::access_of<ecs::Read<Position>>();
  const ecs::Access bare_position = ecs::access_of<Position>();
  const ecs::Access read_position_write_velocity =
      ecs::access_of<ecs::Read<Position>, ecs::Write<Velocity>>();
  const ecs::Access read_velocity_write_position =
      ecs::access_of<ecs::Read<Velocity>, ecs::Write<Position>>();

  // writer vs reader, and writer vs writer, on the same component.
  FIBERECS_CHECK(write_position.conflicts_with(read_position));
  FIBERECS_CHECK(read_position.conflicts_with(write_position));
  FIBERECS_CHECK(write_position.conflicts_with(write_position));
  // A bare T is a read.
  FIBERECS_CHECK(bare_position.conflicts_with(write_position));
  FIBERECS_CHECK(!bare_position.conflicts_with(read_position));
  // Reader against a reader/writer pair that also writes that component.
  FIBERECS_CHECK(write_position.conflicts_with(read_position_write_velocity));
  FIBERECS_CHECK(!read_position.conflicts_with(read_position_write_velocity));
  // Two systems that write the same component in different ways still clash.
  FIBERECS_CHECK(read_position_write_velocity.conflicts_with(read_velocity_write_position));
  // Disjoint component sets never clash.
  FIBERECS_CHECK(!read_position_write_velocity.conflicts_with(ecs::access_of<ecs::Write<Health>>()));
  FIBERECS_CHECK(!ecs::access_of<ecs::Read<Position>>().conflicts_with(
      ecs::access_of<ecs::Read<Velocity>, ecs::Write<Health>>()));
}

FIBERECS_TEST_CASE("ECS: concurrent dispatchers serialise conflicting systems")
{
  constexpr int kCount = 5000;
  constexpr int kRounds = 8;

  JobScheduler scheduler;
  scheduler.initialize(2);

  ecs::World world_a;
  ecs::World world_b;
  for (int i = 0; i < kCount; ++i)
  {
    const Position origin{static_cast<float>(i), 0.0f, 0.0f};
    const Velocity step{1.0f, 0.0f, 0.0f};
    world_a.spawn_with(origin, step);
    world_b.spawn_with(origin, step);
  }

  ecs::Dispatcher dispatch_a(world_a, scheduler);

  // Both worlds use Position, so both dispatches declare Write<Position> and
  // the process-wide gate must serialise them. If the gate could not release,
  // this test would hang rather than fail.
  std::thread other([&] {
    ecs::Dispatcher dispatch_b(world_b, scheduler);
    for (int round = 0; round < kRounds; ++round)
    {
      dispatch_b.run<Write<Position>, Read<Velocity>>([](auto& view) {
        auto* const position = view.template data<Position>();
        const auto* const velocity = view.template data<Velocity>();
        for (std::uint32_t row = 0; row < view.count(); ++row)
        {
          position[row].x += velocity[row].x;
        }
      });
    }
  });

  for (int round = 0; round < kRounds; ++round)
  {
    dispatch_a.run<Write<Position>, Read<Velocity>>([](auto& view) {
      auto* const position = view.template data<Position>();
      const auto* const velocity = view.template data<Velocity>();
      for (std::uint32_t row = 0; row < view.count(); ++row)
      {
        position[row].x += velocity[row].x;
      }
    });
  }
  other.join();

  int mismatches = 0;
  const float expected = static_cast<float>(kRounds);
  world_a.each([&](ecs::Entity handle) {
    const Position& position = world_a.get<Position>(handle);
    if (position.y != 0.0f || position.x - static_cast<float>(handle.index) != expected)
    {
      ++mismatches;
    }
  });
  FIBERECS_CHECK_EQ(mismatches, 0);

  // world_b was written entirely by the other thread; verify every row.
  std::size_t checked = 0;
  world_b.each([&](ecs::Entity handle) {
    const Position& position = world_b.get<Position>(handle);
    if (position.x - static_cast<float>(handle.index) != expected)
    {
      ++mismatches;
    }
    ++checked;
  });
  FIBERECS_CHECK_EQ(checked, std::size_t{kCount});
  FIBERECS_CHECK_EQ(mismatches, 0);

  scheduler.drain();
}

FIBERECS_TEST_CASE("ECS: over-aligned components keep their alignment inside chunks")
{
  constexpr std::size_t kCount = 64;

  ecs::World world;
  ecs::Entity first{};
  for (std::size_t i = 0; i < kCount; ++i)
  {
    const ecs::Entity handle = world.spawn<GridCell>();
    if (i == 0)
    {
      first = handle;
    }
  }

  const std::vector<ecs::Chunk*> chunks = world.query<GridCell>();
  FIBERECS_CHECK(!chunks.empty());

  std::size_t rows = 0;
  int misaligned = 0;
  for (ecs::Chunk* const chunk : chunks)
  {
    GridCell* const cells = chunk->column<GridCell>(0);
    if (reinterpret_cast<std::uintptr_t>(cells) % alignof(GridCell) != 0)
    {
      ++misaligned;
    }
    for (std::uint32_t row = 0; row < chunk->count(); ++row)
    {
      if (reinterpret_cast<std::uintptr_t>(cells + row) % alignof(GridCell) != 0)
      {
        ++misaligned;
      }
      if (chunk->entity(row).index == 0)
      {
        cells[row].lo = 0xABCDEFull;
      }
    }
    rows += chunk->count();
  }
  FIBERECS_CHECK_EQ(misaligned, 0);
  FIBERECS_CHECK_EQ(rows, kCount);

  // The first entity is the one whose slot we just touched: reading it back
  // through the row machinery confirms entities and columns stay in step.
  FIBERECS_CHECK_EQ(world.get<GridCell>(first).lo, 0xABCDEFull);
}
