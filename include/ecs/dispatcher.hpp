#pragma once

// ---------------------------------------------------------------------------
// Dispatcher: Read/Write declared systems executed as parallel chunk jobs.
//
// A system declares the components it touches:
//
//   Dispatcher dispatch(world, scheduler);
//   dispatch.run<Write<Position>, Read<Velocity>>(
//       [](auto& view) {
//         auto* pos = view.template data<Position>();     // Position*
//         const auto* vel = view.template data<Velocity>(); // const Velocity*
//         for (std::uint32_t i = 0; i < view.count(); ++i) { ... }
//       });
//
// The world is queried once, every matching non-empty chunk becomes one job,
// and the jobs are handed to the work-stealing scheduler. Jobs capture only a
// pointer to a pre-sized task slot, so submission performs no heap traffic and
// a system running over existing chunks never allocates at all.
//
// Access discipline: a process-wide gate holds one reader/writer count per
// component id. A dispatch waits until its declared set is compatible with
// every dispatch already in flight, then registers itself for the duration of
// the wait below. Conflicting systems therefore serialise, non-conflicting
// systems overlap, and no caller has to reason about it.
// ---------------------------------------------------------------------------

#include "ecs/world.hpp"
#include "job/JobScheduler.hpp"

#include <array>
#include <bitset>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace fiberecs::ecs
{

// Read<T> / Write<T> are the two access specs. A bare T (no spec) is treated
// as a read, which keeps one-component systems terse.
template <typename T>
struct Read
{
  using component_type = T;
};

template <typename T>
struct Write
{
  using component_type = T;
};

namespace detail
{

inline constexpr std::size_t kIndexNotFound = static_cast<std::size_t>(~std::size_t{0});

template <typename Spec>
struct spec_traits
{
  using type = Spec;
  static constexpr bool is_write = false;
};

template <typename T>
struct spec_traits<Read<T>>
{
  using type = T;
  static constexpr bool is_write = false;
};

template <typename T>
struct spec_traits<Write<T>>
{
  using type = T;
  static constexpr bool is_write = true;
};

template <typename Spec>
using spec_component_t = typename spec_traits<std::remove_cvref_t<Spec>>::type;

template <typename T, typename... Specs>
inline constexpr bool kIsWriteSpec = (std::is_same_v<std::remove_cvref_t<Specs>, Write<T>> || ...);

template <typename T, typename... Specs>
inline constexpr bool kIsReadSpec =
    ((std::is_same_v<std::remove_cvref_t<Specs>, Read<T>> ||
      std::is_same_v<std::remove_cvref_t<Specs>, T>) ||
     ...);

// Position of the spec that mentions T, or kIndexNotFound.
template <typename T, typename... Specs>
[[nodiscard]] constexpr std::size_t index_of_component() noexcept
{
  if constexpr (sizeof...(Specs) == 0)
  {
    return kIndexNotFound;
  }
  else
  {
    const bool matches[] = {
        (std::is_same_v<std::remove_cvref_t<Specs>, Read<T>> ||
         std::is_same_v<std::remove_cvref_t<Specs>, Write<T>> ||
         std::is_same_v<std::remove_cvref_t<Specs>, T>)...};
    for (std::size_t position = 0; position < sizeof...(Specs); ++position)
    {
      if (matches[position])
      {
        return position;
      }
    }
    return kIndexNotFound;
  }
}

} // namespace detail

// ---------------------------------------------------------------------------
// Access: what a system touches, as per-component read/write counts.
// ---------------------------------------------------------------------------
struct Access
{
  std::bitset<kMaxComponentTypes> reads{};
  std::bitset<kMaxComponentTypes> writes{};
  // One past the highest component id referenced, so the gate only walks the
  // ids this system actually names instead of all 256 slots.
  std::uint32_t id_limit{0};

  template <typename Spec>
  void declare()
  {
    using Component = detail::spec_component_t<Spec>;
    const ComponentId id = component_id<Component>();
    if constexpr (detail::spec_traits<std::remove_cvref_t<Spec>>::is_write)
    {
      writes.set(static_cast<std::size_t>(id));
    }
    else
    {
      reads.set(static_cast<std::size_t>(id));
    }
    if (id >= id_limit)
    {
      id_limit = id + 1;
    }
  }

  // Two accesses conflict when either writes a component the other touches.
  [[nodiscard]] bool conflicts_with(const Access& other) const noexcept
  {
    return ((reads & other.writes) | (writes & other.reads) | (writes & other.writes)).any();
  }
};

template <typename... Specs>
[[nodiscard]] inline Access access_of()
{
  Access requested;
  (requested.declare<Specs>(), ...);
  return requested;
}

namespace detail
{

// Process-wide reader/writer gate over component ids. Counters, not bitsets:
// two readers of the same component must both leave before a writer enters.
class DispatcherGate
{
public:
  static DispatcherGate& instance() noexcept
  {
    static DispatcherGate gate;
    return gate;
  }

  void enter(const Access& requested)
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_ready.wait(lock, [this, &requested] { return compatible(requested); });
    acquire(requested);
  }

  void exit(const Access& requested)
  {
    {
      const std::lock_guard<std::mutex> lock(m_mutex);
      release(requested);
    }
    m_ready.notify_all();
  }

private:
  DispatcherGate() = default;

  [[nodiscard]] bool compatible(const Access& requested) const noexcept
  {
    for (std::uint32_t id = 0; id < requested.id_limit; ++id)
    {
      if (requested.writes.test(id) && (m_readers[id] != 0 || m_writers[id] != 0))
      {
        return false;
      }
      if (requested.reads.test(id) && m_writers[id] != 0)
      {
        return false;
      }
    }
    return true;
  }

  void acquire(const Access& requested) noexcept
  {
    for (std::uint32_t id = 0; id < requested.id_limit; ++id)
    {
      if (requested.reads.test(id))
      {
        ++m_readers[id];
      }
      if (requested.writes.test(id))
      {
        ++m_writers[id];
      }
    }
  }

  void release(const Access& requested) noexcept
  {
    for (std::uint32_t id = 0; id < requested.id_limit; ++id)
    {
      if (requested.reads.test(id))
      {
        --m_readers[id];
      }
      if (requested.writes.test(id))
      {
        --m_writers[id];
      }
    }
  }

  std::mutex m_mutex;
  std::condition_variable m_ready;
  std::array<std::uint32_t, kMaxComponentTypes> m_readers{};
  std::array<std::uint32_t, kMaxComponentTypes> m_writers{};
};

// RAII registration of one dispatch with the gate.
class GateGuard
{
public:
  explicit GateGuard(const Access& requested) : m_requested(requested)
  {
    DispatcherGate::instance().enter(m_requested);
  }

  ~GateGuard() { DispatcherGate::instance().exit(m_requested); }

  GateGuard(const GateGuard&) = delete;
  GateGuard& operator=(const GateGuard&) = delete;
  GateGuard(GateGuard&&) = delete;
  GateGuard& operator=(GateGuard&&) = delete;

private:
  Access m_requested;
};

[[nodiscard]] inline int& dispatch_depth() noexcept
{
  thread_local int depth = 0;
  return depth;
}

// run() may not be called from inside a job: the caller already holds the
// gate for that dispatch, so a nested run() would wait for jobs that cannot
// start until it returns. Detect the mistake instead of deadlocking.
class DispatchScope
{
public:
  DispatchScope()
  {
    int& depth = dispatch_depth();
    if (depth > 0)
    {
      std::fprintf(stderr,
                   "[ecs] FATAL: Dispatcher::run() called from inside a job; systems must not "
                   "dispatch recursively\n");
      std::fflush(stderr);
      std::abort();
    }
    depth = 1;
  }

  ~DispatchScope() { dispatch_depth() = 0; }

  DispatchScope(const DispatchScope&) = delete;
  DispatchScope& operator=(const DispatchScope&) = delete;
};

// One scheduled job: a chunk plus the type-erased system call. Lives in the
// dispatcher's pre-sized slot array, so the job lambda captures a single
// pointer and std::function stores it inline.
struct ChunkTask
{
  Chunk* chunk{nullptr};
  const void* callable{nullptr};
  void (*invoke)(const void*, Chunk*){nullptr};

  void run() const { invoke(callable, chunk); }
};

} // namespace detail

// ---------------------------------------------------------------------------
// ChunkView: the system-facing window onto one chunk of matching entities.
// ---------------------------------------------------------------------------
template <typename... Specs>
class ChunkView
{
  static_assert(sizeof...(Specs) >= 1, "a system must declare at least one component");
  static_assert(sizeof...(Specs) <= kMaxComponentsPerArchetype,
                "a system view cannot declare more components than an archetype can hold");

public:
  explicit ChunkView(Chunk* target) noexcept : m_chunk(target)
  {
    fill_columns(std::make_index_sequence<sizeof...(Specs)>{});
  }

  [[nodiscard]] std::uint32_t count() const noexcept { return m_chunk->count(); }
  [[nodiscard]] Entity entity(std::uint32_t local_row) const noexcept
  {
    return m_chunk->entity(local_row);
  }
  [[nodiscard]] Chunk* chunk() const noexcept { return m_chunk; }

  // T* for a Write<T> spec, const T* for Read<T> or a bare T. The pointer
  // addresses element 0 of the column; index it directly.
  template <typename T>
  [[nodiscard]] std::conditional_t<detail::kIsWriteSpec<T, Specs...>, T*, const T*> data()
      const noexcept
  {
    static_assert(detail::kIsWriteSpec<T, Specs...> || detail::kIsReadSpec<T, Specs...>,
                  "T is not declared by this system");
    constexpr std::size_t position = detail::index_of_component<T, Specs...>();
    static_assert(position != detail::kIndexNotFound, "T is not declared by this system");
    return m_chunk->column<T>(m_columns[position]);
  }

  template <typename T>
  [[nodiscard]] std::conditional_t<detail::kIsWriteSpec<T, Specs...>, T&, const T&> at(
      std::uint32_t local_row) const noexcept
  {
    return *(data<T>() + local_row);
  }

private:
  template <std::size_t... I>
  void fill_columns(std::index_sequence<I...>) noexcept
  {
    (fill_column<I>(), ...);
  }

  template <std::size_t I>
  void fill_column() noexcept
  {
    using Spec = std::tuple_element_t<I, std::tuple<Specs...>>;
    using Component = detail::spec_component_t<Spec>;
    const std::size_t column_position = m_chunk->archetype()->column_of(component_id<Component>());
    if (column_position == kColumnNotFound)
    {
      // Unreachable: the dispatcher only hands over archetypes whose signature
      // contains every declared component.
      std::fprintf(stderr, "[ecs] FATAL: view component missing from chunk archetype\n");
      std::fflush(stderr);
      std::abort();
    }
    m_columns[I] = static_cast<std::uint32_t>(column_position);
  }

  Chunk* m_chunk;
  std::array<std::uint32_t, sizeof...(Specs)> m_columns{};
};

namespace detail
{

// Bridges the type-erased ChunkTask back to the system's real signature.
template <typename FnT, typename... Specs>
inline void invoke_task(const void* callable, Chunk* target)
{
  const FnT& system = *static_cast<const FnT*>(callable);
  ChunkView<Specs...> view(target);
  system(view);
}

} // namespace detail

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------
class Dispatcher
{
public:
  Dispatcher(World& world, JobScheduler& scheduler) noexcept
    : m_world(&world), m_scheduler(&scheduler)
  {
  }

  Dispatcher(const Dispatcher&) = delete;
  Dispatcher& operator=(const Dispatcher&) = delete;
  Dispatcher(Dispatcher&&) = delete;
  Dispatcher& operator=(Dispatcher&&) = delete;

  // Runs `fn` (callable as fn(ChunkView<Specs...>&)) once per matching chunk,
  // in parallel, and returns only when every chunk job has finished.
  //
  // The signature and access set of each instantiation are computed once and
  // cached in function-local statics, so a steady-state dispatch performs no
  // heap traffic: the chunk list and task slots are reused buffers, job
  // payloads fit std::function's small-object buffer, and the component
  // registry is only consulted on the first call.
  template <typename... Specs, typename Fn>
  void run(Fn&& fn)
  {
    static_assert(sizeof...(Specs) >= 1, "declare at least one component in a system");
    static_assert(sizeof...(Specs) <= kMaxComponentsPerArchetype,
                  "a system cannot declare more components than an archetype can hold");

    using Callable = std::remove_reference_t<Fn>;

    static const Signature required =
        make_signature({component_id<detail::spec_component_t<Specs>>()...});
    static const Access access = access_of<Specs...>();

    const detail::DispatchScope scope;

    const detail::GateGuard gate(access);

    m_world->collect(required, m_chunk_scratch);
    if (m_chunk_scratch.empty())
    {
      return;
    }

    m_tasks.resize(m_chunk_scratch.size());
    JobCounter counter;

    for (std::size_t task_index = 0; task_index < m_chunk_scratch.size(); ++task_index)
    {
      detail::ChunkTask& task = m_tasks[task_index];
      task.chunk = m_chunk_scratch[task_index];
      task.callable = std::addressof(fn);
      task.invoke = &detail::invoke_task<Callable, Specs...>;
      // Captures one pointer: std::function keeps it in its inline buffer, so
      // scheduling N chunk jobs allocates nothing.
      m_scheduler->schedule([&task] { task.run(); }, &counter);
    }

    m_scheduler->wait(&counter);
  }

  [[nodiscard]] std::size_t chunk_jobs_last_run() const noexcept { return m_chunk_scratch.size(); }

private:
  World* m_world;
  JobScheduler* m_scheduler;
  std::vector<Chunk*> m_chunk_scratch;
  std::vector<detail::ChunkTask> m_tasks;
};

} // namespace fiberecs::ecs
