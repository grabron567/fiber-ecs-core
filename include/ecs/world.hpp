#pragma once

// ---------------------------------------------------------------------------
// World: entity storage, archetype transitions, queries.
//
// A World owns the entity index space (handles are {index, generation}) and a
// map from signature to Archetype. Every structural operation -- spawn, add,
// remove, destroy -- is a *transition* between archetypes: the shared columns
// are memcpy'd across, the source row is closed with swap-and-pop, and the
// displaced entity's recorded row is repaired.
//
// Threading contract: the World is single-threaded. Structural changes must
// not overlap a dispatch (see dispatcher.hpp, which serialises conflicting
// systems through a process-wide gate), and two threads must not mutate the
// same World at once. Reading component data through a dispatched system runs
// on as many workers as the scheduler has.
// ---------------------------------------------------------------------------

#include "ecs/archetype.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace fiberecs::ecs
{

// Where an entity's row lives. `archetype == nullptr` marks a free index.
struct EntityLocation
{
  Archetype* archetype{nullptr};
  std::uint32_t row{0};

  [[nodiscard]] bool valid() const noexcept { return archetype != nullptr; }
};

namespace detail
{

[[noreturn]] inline void fatal_invalid(const char* operation, std::uint32_t index,
                                        std::uint32_t generation) noexcept
{
  std::fprintf(stderr, "[ecs] FATAL: %s (entity index %u generation %u)\n", operation,
               static_cast<unsigned int>(index), static_cast<unsigned int>(generation));
  std::fflush(stderr);
  std::abort();
}

} // namespace detail

class World
{
public:
  World() : World(kDefaultTargetChunkBytes) {}

  explicit World(std::size_t target_chunk_bytes)
    : m_target_chunk_bytes(target_chunk_bytes < kCacheLineSize ? kCacheLineSize
                                                               : target_chunk_bytes)
  {
  }

  World(const World&) = delete;
  World& operator=(const World&) = delete;
  World(World&&) = delete;
  World& operator=(World&&) = delete;

  // -------------------------------------------------------------------------
  // Entity lifecycle
  // -------------------------------------------------------------------------

  // New entity carrying exactly the components `Cs...`, value-initialised.
  // spawn<>() with no arguments creates a component-less entity.
  template <typename... Cs>
  Entity spawn()
  {
    const std::uint32_t index = acquire_index();
    const Entity instance{index, m_generations[index]};
    Archetype* const destination = archetype_for(make_signature({component_id<Cs>()...}));
    const std::uint32_t row = destination->append(instance);
    m_locations[index] = EntityLocation{destination, row};
    (emplace_default<Cs>(destination, row), ...);
    ++m_alive;
    return instance;
  }

  // Same, with the component values supplied in declaration order. Arguments
  // are taken by value so that lvalue/rvalue-ness never reaches the component
  // type id.
  template <typename... Cs>
  Entity spawn_with(Cs... values)
  {
    const std::uint32_t index = acquire_index();
    const Entity instance{index, m_generations[index]};
    Archetype* const destination = archetype_for(make_signature({component_id<Cs>()...}));
    const std::uint32_t row = destination->append(instance);
    m_locations[index] = EntityLocation{destination, row};
    (emplace_value<Cs>(destination, row, std::move(values)), ...);
    ++m_alive;
    return instance;
  }

  // Returns false for an invalid (already destroyed or stale) handle instead
  // of aborting, so bulk teardown loops can be written naturally.
  [[nodiscard]] bool destroy(Entity instance)
  {
    if (!valid(instance))
    {
      return false;
    }
    const EntityLocation location = m_locations[instance.index];
    Archetype* const source = location.archetype;
    const bool displaced = source->remove_at(location.row);
    if (displaced)
    {
      // Swap-and-pop moved the previous last row into the hole.
      const Entity moved = source->entity_at(location.row);
      m_locations[moved.index].row = location.row;
    }
    m_locations[instance.index] = EntityLocation{};
    m_generations[instance.index] = next_generation(m_generations[instance.index]);
    m_free.push_back(instance.index);
    --m_alive;
    return true;
  }

  [[nodiscard]] bool valid(Entity instance) const noexcept
  {
    if (static_cast<std::size_t>(instance.index) >= m_locations.size())
    {
      return false;
    }
    return m_locations[instance.index].valid() &&
           m_generations[instance.index] == instance.generation;
  }

  // -------------------------------------------------------------------------
  // Component access and archetype transitions
  // -------------------------------------------------------------------------

  // Attaches T. If the entity already has T the assignment is a plain store.
  template <typename T>
  void add(Entity instance, T value = T{})
  {
    require_valid(instance, "add<T>");
    Archetype* const source = m_locations[instance.index].archetype;
    const ComponentId id = component_id<T>();
    if (source->contains(id))
    {
      get<T>(instance) = std::move(value);
      return;
    }
    Signature destination_signature = source->signature();
    destination_signature.push_back(id);
    sort_signature(destination_signature);
    transition(instance, archetype_for(destination_signature));
    get<T>(instance) = std::move(value);
  }

  // Detaches T, moving the entity to the archetype without it.
  template <typename T>
  void remove(Entity instance)
  {
    require_valid(instance, "remove<T>");
    Archetype* const source = m_locations[instance.index].archetype;
    const ComponentId id = component_id<T>();
    if (!source->contains(id))
    {
      detail::fatal_invalid("remove<T>: entity does not have the component", instance.index,
                            instance.generation);
    }
    Signature destination_signature = source->signature();
    destination_signature.erase(std::lower_bound(destination_signature.begin(),
                                                 destination_signature.end(), id));
    transition(instance, archetype_for(destination_signature));
  }

  template <typename T>
  [[nodiscard]] T* try_get(Entity instance) noexcept
  {
    if (!valid(instance))
    {
      return nullptr;
    }
    const EntityLocation location = m_locations[instance.index];
    const std::size_t column_index = location.archetype->column_of(component_id<T>());
    if (column_index == kColumnNotFound)
    {
      return nullptr;
    }
    return static_cast<T*>(
        static_cast<void*>(location.archetype->column_row_bytes(column_index, location.row)));
  }

  template <typename T>
  [[nodiscard]] T& get(Entity instance)
  {
    T* const value = try_get<T>(instance);
    if (value == nullptr)
    {
      detail::fatal_invalid("get<T>: entity is invalid or lacks the component", instance.index,
                            instance.generation);
    }
    return *value;
  }

  template <typename T>
  [[nodiscard]] bool has(Entity instance) const noexcept
  {
    if (!valid(instance))
    {
      return false;
    }
    return m_locations[instance.index].archetype->contains(component_id<T>());
  }

  // -------------------------------------------------------------------------
  // Queries
  // -------------------------------------------------------------------------

  // Appends every non-empty chunk of every archetype that contains all of
  // `required`. `out` is reused by callers so that a hot dispatch loop can run
  // without touching the heap after the first call.
  void collect(const Signature& required, std::vector<Chunk*>& out) const
  {
    out.clear();
    for (const auto& entry : m_archetypes)
    {
      Archetype& archetype_instance = *entry.second;
      if (!signature_contains_all(archetype_instance.signature(), required))
      {
        continue;
      }
      for (std::size_t chunk_index = 0; chunk_index < archetype_instance.chunk_count();
           ++chunk_index)
      {
        Chunk* const candidate = archetype_instance.chunk(chunk_index);
        if (candidate->count() > 0)
        {
          out.push_back(candidate);
        }
      }
    }
  }

  template <typename... Cs>
  [[nodiscard]] std::vector<Chunk*> query() const
  {
    std::vector<Chunk*> matches;
    collect(make_signature({component_id<Cs>()...}), matches);
    return matches;
  }

  template <typename Fn>
  void each(Fn&& fn) const
  {
    for (std::size_t index = 0; index < m_locations.size(); ++index)
    {
      if (m_locations[index].valid())
      {
        fn(Entity{static_cast<std::uint32_t>(index), m_generations[index]});
      }
    }
  }

  template <typename Fn>
  void each_archetype(Fn&& fn) const
  {
    for (const auto& entry : m_archetypes)
    {
      fn(entry.first, *entry.second);
    }
  }

  // -------------------------------------------------------------------------
  // Introspection
  // -------------------------------------------------------------------------

  [[nodiscard]] std::size_t alive() const noexcept { return m_alive; }
  [[nodiscard]] std::size_t archetype_count() const noexcept { return m_archetypes.size(); }
  [[nodiscard]] std::size_t entity_slots() const noexcept { return m_locations.size(); }

  // Cumulative counters: chunks are carved from a bump arena that never frees,
  // so these only ever grow (they count chunks ever built, not rows in use).
  [[nodiscard]] const ChunkStorageStats& storage_stats() const noexcept
  {
    return m_allocator.stats();
  }

  [[nodiscard]] std::size_t target_chunk_bytes() const noexcept { return m_target_chunk_bytes; }

  // Exposed for the dispatcher: look up (or mint) the archetype for a
  // signature. Minting is the only heap traffic a structural change performs.
  [[nodiscard]] Archetype* archetype_for(const Signature& signature)
  {
    const auto existing = m_archetypes.find(signature);
    if (existing != m_archetypes.end())
    {
      return existing->second.get();
    }
    auto created = std::make_unique<Archetype>(signature, m_allocator, m_target_chunk_bytes);
    Archetype* const raw = created.get();
    m_archetypes.emplace(signature, std::move(created));
    return raw;
  }

private:
  static std::uint32_t next_generation(std::uint32_t generation) noexcept
  {
    ++generation;
    return generation == 0 ? 1u : generation;
  }

  [[nodiscard]] std::uint32_t acquire_index()
  {
    if (!m_free.empty())
    {
      const std::uint32_t index = m_free.back();
      m_free.pop_back();
      return index;
    }
    const std::uint32_t index = static_cast<std::uint32_t>(m_locations.size());
    m_locations.emplace_back();
    m_generations.push_back(0);
    return index;
  }

  void require_valid(Entity instance, const char* operation) const
  {
    if (!valid(instance))
    {
      detail::fatal_invalid(operation, instance.index, instance.generation);
    }
  }

  // Moves an entity to `destination`, carrying every shared column across and
  // repairing whichever entity swap-and-pop displaced.
  void transition(Entity instance, Archetype* destination)
  {
    const EntityLocation source_location = m_locations[instance.index];
    Archetype* const source = source_location.archetype;
    const std::uint32_t source_row = source_location.row;
    const std::uint32_t destination_row = destination->append(instance);

    destination->copy_row_from(*source, source_row, destination_row);

    if (source->remove_at(source_row))
    {
      const Entity moved = source->entity_at(source_row);
      m_locations[moved.index].row = source_row;
    }
    m_locations[instance.index] = EntityLocation{destination, destination_row};
  }

  template <typename T>
  static void emplace_default(Archetype* destination, std::uint32_t row)
  {
    void* const storage = destination->row_bytes(component_id<T>(), row);
    ::new (storage) T();
  }

  template <typename T>
  static void emplace_value(Archetype* destination, std::uint32_t row, T&& value)
  {
    void* const storage = destination->row_bytes(component_id<T>(), row);
    ::new (storage) T(std::forward<T>(value));
  }

  std::size_t m_target_chunk_bytes;
  // Declared before m_archetypes: every Archetype points into this allocator,
  // so it must outlive them (members are destroyed in reverse declaration).
  ChunkAllocator m_allocator;
  std::map<Signature, std::unique_ptr<Archetype>> m_archetypes;
  std::vector<EntityLocation> m_locations;
  std::vector<std::uint32_t> m_generations;
  std::vector<std::uint32_t> m_free;
  std::size_t m_alive{0};
};

} // namespace fiberecs::ecs
