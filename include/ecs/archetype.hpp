#pragma once

// ---------------------------------------------------------------------------
// Archetypal ECS storage: component registry, archetype chunks, chunk allocator.
//
// An Archetype is one *exact* component set. Every entity in it lives in the
// same chunk layout, so component columns are plain contiguous arrays with no
// per-entity indirection -- iterating Position means walking one pointer
// forward, which is what makes the movement loop vectorisable and prefetchable.
//
// Memory layout of one chunk (every section starts on its own cache line):
//
//   [Chunk header][Entity handles][col0][col1] ... [colN]
//                        ^                  ^
//                        |                  each column is capacity * sizeof(T)
//                        Entity[] is capacity * sizeof(Entity)
//
// Chunks are carved from a growing slab of LinearArena blocks, so a system
// running over existing chunks never calls the heap: structural change is the
// only operation that can mint a new chunk.
// ---------------------------------------------------------------------------

#include "core/LinearArena.hpp"
#include "core/MemoryUtils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace fiberecs::ecs
{

// ---------------------------------------------------------------------------
// Entity handle: a stable (index, generation) pair.
//
// The generation is bumped when the slot is recycled, so a handle to a
// destroyed entity can never alias a later entity that reused its index.
// ---------------------------------------------------------------------------
struct Entity
{
  static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

  std::uint32_t index{kInvalidIndex};
  std::uint32_t generation{0};

  [[nodiscard]] bool valid() const noexcept { return index != kInvalidIndex; }

  bool operator==(const Entity&) const = default;
};

// ---------------------------------------------------------------------------
// Component registry
//
// Type ids are process-wide and assigned once per type on first use, so two
// Worlds share the same numbering and no World has to register anything up
// front. Metadata is only read while an archetype is being built, never from
// the per-entity hot path.
// ---------------------------------------------------------------------------
using ComponentId = std::uint32_t;

inline constexpr ComponentId kInvalidComponentId = 0xFFFFFFFFu;
inline constexpr std::size_t kMaxComponentTypes = 256;
inline constexpr std::size_t kMaxComponentsPerArchetype = 32;

struct ComponentType
{
  std::size_t size{0};
  std::size_t alignment{0};
  const char* name{nullptr};
};

namespace detail
{

inline std::mutex& registry_mutex() noexcept
{
  static std::mutex mutex;
  return mutex;
}

// deque: pushing a new type never invalidates an entry already handed out.
inline std::deque<ComponentType>& registry_storage() noexcept
{
  static std::deque<ComponentType> storage;
  return storage;
}

} // namespace detail

inline ComponentId register_component(std::size_t bytes, std::size_t alignment, const char* name)
{
  const std::lock_guard<std::mutex> lock(detail::registry_mutex());
  std::deque<ComponentType>& storage = detail::registry_storage();
  if (storage.size() >= kMaxComponentTypes)
  {
    std::fprintf(stderr, "[ecs] FATAL: component type limit (%zu) exceeded registering '%s'\n",
                 kMaxComponentTypes, name);
    std::abort();
  }
  storage.push_back(ComponentType{bytes, alignment, name});
  return static_cast<ComponentId>(storage.size() - 1);
}

// Returned by value: the registry may grow concurrently with another thread
// building an archetype, and a reference into a deque would not be safe.
[[nodiscard]] inline ComponentType component_type(ComponentId id)
{
  const std::lock_guard<std::mutex> lock(detail::registry_mutex());
  const std::deque<ComponentType>& storage = detail::registry_storage();
  if (static_cast<std::size_t>(id) >= storage.size())
  {
    std::fprintf(stderr, "[ecs] FATAL: unregistered component id %u\n", static_cast<unsigned int>(id));
    std::abort();
  }
  return storage[static_cast<std::size_t>(id)];
}

template <typename T>
[[nodiscard]] ComponentId component_id()
{
  static_assert(std::is_same_v<T, std::remove_cvref_t<T>>,
                "component types must be cv-unqualified; declare Position, not const Position");
  static_assert(std::is_trivially_destructible_v<T>,
                "components live in arena storage and are never destructed, so their type must be "
                "trivially destructible");
  static const ComponentId id = register_component(sizeof(T), alignof(T), typeid(T).name());
  return id;
}

// A signature is a sorted, de-duplicated component set. Ordering makes
// signature equality a plain lexicographic compare and lets column lookup use
// a binary search.
using Signature = std::vector<ComponentId>;

inline void sort_signature(Signature& signature)
{
  std::sort(signature.begin(), signature.end());
  signature.erase(std::unique(signature.begin(), signature.end()), signature.end());
}

[[nodiscard]] inline Signature make_signature(std::initializer_list<ComponentId> ids)
{
  Signature signature(ids);
  sort_signature(signature);
  return signature;
}

[[nodiscard]] inline bool signature_contains_all(const Signature& haystack, const Signature& needles)
{
  std::size_t matched = 0;
  std::size_t cursor = 0;
  for (const ComponentId id : haystack)
  {
    while (cursor < needles.size() && needles[cursor] < id)
    {
      ++cursor;
    }
    if (cursor == needles.size())
    {
      break;
    }
    if (needles[cursor] == id)
    {
      ++matched;
      ++cursor;
    }
  }
  return matched == needles.size();
}

inline constexpr std::size_t kColumnNotFound = static_cast<std::size_t>(~std::size_t{0});

// Chunk sizing: aim for this many bytes per chunk so a chunk still fits in L1/L2
// and a single system job stays at a sensible grain.
inline constexpr std::size_t kDefaultTargetChunkBytes = 16u * 1024u;
inline constexpr std::size_t kMaxChunkEntities = 4096u;

// ---------------------------------------------------------------------------
// Chunk layout: where every column sits inside a chunk block.
//
// Offsets are computed once per archetype. Each column is pushed to the next
// multiple of 64 bytes (or of the component's own alignment, whichever is
// larger) so a column never straddles a cache line and an AVX-512 load that
// starts at element 0 stays inside one aligned span.
// ---------------------------------------------------------------------------
struct ChunkLayout
{
  std::size_t capacity{0};
  std::size_t entity_offset{0};
  std::size_t bytes{0};
  std::array<std::size_t, kMaxComponentsPerArchetype> column_offsets{};
};

class Archetype;

class alignas(kCacheLineSize) Chunk
{
public:
  Chunk(Archetype* archetype, std::uint32_t chunk_index, std::uint32_t capacity, Entity* entities,
        const std::array<std::byte*, kMaxComponentsPerArchetype>& columns) noexcept
    : m_archetype(archetype),
      m_entities(entities),
      m_columns(columns),
      m_chunk_index(chunk_index),
      m_capacity(capacity),
      m_count(0)
  {
  }

  Chunk(const Chunk&) = delete;
  Chunk& operator=(const Chunk&) = delete;
  Chunk(Chunk&&) = delete;
  Chunk& operator=(Chunk&&) = delete;

  [[nodiscard]] std::uint32_t count() const noexcept { return m_count; }
  void set_count(std::uint32_t rows) noexcept { m_count = rows; }
  [[nodiscard]] std::uint32_t capacity() const noexcept { return m_capacity; }
  [[nodiscard]] std::uint32_t chunk_index() const noexcept { return m_chunk_index; }
  [[nodiscard]] Archetype* archetype() const noexcept { return m_archetype; }

  [[nodiscard]] Entity* entities() const noexcept { return m_entities; }

  [[nodiscard]] Entity entity(std::uint32_t local_row) const noexcept { return m_entities[local_row]; }
  void set_entity(std::uint32_t local_row, Entity value) const noexcept { m_entities[local_row] = value; }

  [[nodiscard]] std::byte* column_bytes(std::uint32_t column_index) const noexcept
  {
    return m_columns[column_index];
  }

  template <typename T>
  [[nodiscard]] T* column(std::uint32_t column_index) const noexcept
  {
    return static_cast<T*>(static_cast<void*>(m_columns[column_index]));
  }

private:
  Archetype* m_archetype{nullptr};
  Entity* m_entities{nullptr};
  std::array<std::byte*, kMaxComponentsPerArchetype> m_columns{};
  std::uint32_t m_chunk_index{0};
  std::uint32_t m_capacity{0};
  std::uint32_t m_count{0};
};

static_assert(sizeof(Chunk) % kCacheLineSize == 0, "the chunk header must not disturb column alignment");

// ---------------------------------------------------------------------------
// ChunkAllocator: a growing slab of 64-byte-aligned LinearArena blocks.
//
// Slabs only ever grow, and only when a structural change needs a brand new
// chunk. Because every request is pre-checked against the live slab's
// remaining() the arena's fatal overflow path is unreachable, which matters:
// LinearArena treats overflow as a hard abort rather than returning null.
// ---------------------------------------------------------------------------
struct ChunkStorageStats
{
  std::size_t chunks{0};
  std::size_t slabs{0};
  std::size_t reserved_bytes{0};
  std::size_t used_bytes{0};
};

class ChunkAllocator
{
public:
  explicit ChunkAllocator(std::size_t slab_bytes = kDefaultSlabBytes) noexcept
    : m_slab_bytes(std::max(slab_bytes, kCacheLineSize))
  {
  }

  ChunkAllocator(const ChunkAllocator&) = delete;
  ChunkAllocator& operator=(const ChunkAllocator&) = delete;
  ChunkAllocator(ChunkAllocator&&) = delete;
  ChunkAllocator& operator=(ChunkAllocator&&) = delete;

  static constexpr std::size_t kDefaultSlabBytes = 4u * 1024u * 1024u;

  // Always succeeds: a slab is added when the live one cannot serve the request.
  [[nodiscard]] void* allocate(std::size_t bytes)
  {
    const std::size_t needed = align_up(bytes, kCacheLineSize);
    // +kCacheLineSize covers the worst-case inter-block padding that
    // LinearArena::allocate applies on top of `needed`.
    if (m_slabs.empty() || m_slabs.back()->remaining() < needed + kCacheLineSize)
    {
      m_slabs.push_back(std::make_unique<LinearArena>(std::max(m_slab_bytes, needed), kCacheLineSize));
      ++m_stats.slabs;
      m_stats.reserved_bytes += m_slabs.back()->capacity();
    }

    void* const memory = m_slabs.back()->allocate(needed, kCacheLineSize);
    if (memory == nullptr)
    {
      // Unreachable: the pre-check above guarantees the slab can serve `needed`.
      std::fprintf(stderr, "[ecs] FATAL: chunk slab refused %zuB (slab has %zuB free)\n", needed,
                   m_slabs.back()->remaining());
      std::abort();
    }
    m_stats.used_bytes += needed;
    ++m_stats.chunks;
    return memory;
  }

  [[nodiscard]] const ChunkStorageStats& stats() const noexcept { return m_stats; }

  void reset_stats() noexcept { m_stats = ChunkStorageStats{}; }

private:
  std::size_t m_slab_bytes;
  std::vector<std::unique_ptr<LinearArena>> m_slabs;
  ChunkStorageStats m_stats;
};

// ---------------------------------------------------------------------------
// Archetype: one exact component set plus the chunks that hold its rows.
//
// Rows are addressed *flatly* across chunks (row = chunk_index * capacity +
// local_row), so the archetype is a logical contiguous array. Removal is
// swap-and-pop: the final row slides into the hole, which keeps every chunk
// dense and means a chunk-level parallel job never walks padding.
// ---------------------------------------------------------------------------
// Per-column size/alignment, resolved once per archetype from the component
// registry so the hot path never takes the registry mutex.
struct ColumnSpec
{
  std::size_t size{0};
  std::size_t alignment{0};
};

using ColumnSpecs = std::array<ColumnSpec, kMaxComponentsPerArchetype>;

class Archetype
{
public:
  Archetype(Signature signature, ChunkAllocator& allocator, std::size_t target_chunk_bytes)
    : m_signature(std::move(signature)),
      m_allocator(&allocator),
      m_column_specs(compute_column_specs(m_signature)),
      m_chunk_capacity(solve_chunk_capacity(m_signature.size(), m_column_specs, target_chunk_bytes)),
      m_layout(build_chunk_layout(m_signature.size(), m_column_specs, m_chunk_capacity))
  {
    if (m_signature.size() > kMaxComponentsPerArchetype)
    {
      std::fprintf(stderr, "[ecs] FATAL: archetype carries %zu components, limit is %zu\n",
                   m_signature.size(), kMaxComponentsPerArchetype);
      std::abort();
    }
  }

  Archetype(const Archetype&) = delete;
  Archetype& operator=(const Archetype&) = delete;
  Archetype(Archetype&&) = delete;
  Archetype& operator=(Archetype&&) = delete;

  [[nodiscard]] const Signature& signature() const noexcept { return m_signature; }
  [[nodiscard]] std::size_t component_count() const noexcept { return m_signature.size(); }
  [[nodiscard]] std::uint32_t chunk_capacity() const noexcept { return m_chunk_capacity; }
  [[nodiscard]] const ChunkLayout& layout() const noexcept { return m_layout; }

  [[nodiscard]] std::size_t column_of(ComponentId id) const noexcept
  {
    const auto it = std::lower_bound(m_signature.begin(), m_signature.end(), id);
    if (it == m_signature.end() || *it != id)
    {
      return kColumnNotFound;
    }
    return static_cast<std::size_t>(it - m_signature.begin());
  }

  [[nodiscard]] bool contains(ComponentId id) const noexcept { return column_of(id) != kColumnNotFound; }

  [[nodiscard]] std::size_t size() const noexcept { return m_size; }
  [[nodiscard]] std::size_t chunk_count() const noexcept { return m_chunks.size(); }

  [[nodiscard]] Chunk* chunk(std::size_t index) const noexcept { return m_chunks[index]; }

  [[nodiscard]] std::size_t occupied_bytes() const noexcept
  {
    return m_chunks.size() * m_layout.bytes;
  }

  [[nodiscard]] Entity entity_at(std::uint32_t row) const noexcept
  {
    const std::size_t local = static_cast<std::size_t>(row) / m_chunk_capacity;
    return m_chunks[local]->entity(static_cast<std::uint32_t>(static_cast<std::size_t>(row) %
                                                             m_chunk_capacity));
  }

  // Appends an entity and returns its row. Existing rows never move, so a
  // column pointer taken for row R stays valid until R itself is removed.
  [[nodiscard]] std::uint32_t append(Entity entity)
  {
    if (m_chunks.empty() || m_size == m_chunks.size() * m_chunk_capacity)
    {
      create_chunk();
    }
    const std::size_t row = m_size;
    Chunk* const target = m_chunks[row / m_chunk_capacity];
    target->set_entity(static_cast<std::uint32_t>(row % m_chunk_capacity), entity);
    ++m_size;
    refresh_tail();
    return static_cast<std::uint32_t>(row);
  }

  // Removes `row` by sliding the current last row into the hole. Returns true
  // when a different entity actually moved, in which case the caller must fix
  // up that entity's recorded location.
  [[nodiscard]] bool remove_at(std::uint32_t row)
  {
    if (m_size == 0)
    {
      return false;
    }
    const std::uint32_t last = static_cast<std::uint32_t>(m_size - 1);
    const bool moved = row != last;
    if (moved)
    {
      move_row(last, row);
    }
    --m_size;
    compact_tail();
    return moved;
  }

  // Copies every component shared by `src` and this archetype from one row to
  // another. Components that exist only here (the one being added) are skipped,
  // so the caller may write them afterwards.
  void copy_row_from(const Archetype& src, std::uint32_t src_row, std::uint32_t dst_row)
  {
    const std::size_t src_capacity = src.chunk_capacity();
    const std::size_t src_chunk_index = static_cast<std::size_t>(src_row) / src_capacity;
    const std::size_t src_local = static_cast<std::size_t>(src_row) % src_capacity;
    const std::size_t dst_chunk_index = static_cast<std::size_t>(dst_row) / m_chunk_capacity;
    const std::size_t dst_local = static_cast<std::size_t>(dst_row) % m_chunk_capacity;

    Chunk* const dst_chunk = m_chunks[dst_chunk_index];
    const Chunk* const src_chunk = src.chunk(src_chunk_index);

    for (const ComponentId id : m_signature)
    {
      const std::size_t dst_column = column_of(id);
      const std::size_t src_column = src.column_of(id);
      if (src_column == kColumnNotFound)
      {
        continue;
      }
      // Component size is a property of the type, so this archetype's cached
      // column size is valid for both sides; no registry lookup per row.
      const std::size_t bytes = m_column_specs[dst_column].size;
      std::byte* const dst = dst_chunk->column_bytes(static_cast<std::uint32_t>(dst_column)) +
                             dst_local * bytes;
      const std::byte* const src_bytes =
          src_chunk->column_bytes(static_cast<std::uint32_t>(src_column)) + src_local * bytes;
      std::memcpy(dst, src_bytes, bytes);
    }
  }

  void move_row(std::uint32_t from, std::uint32_t to)
  {
    copy_row_from(*this, from, to);
    set_entity(to, entity_at(from));
  }

  void set_entity(std::uint32_t row, Entity entity)
  {
    const std::size_t chunk_index = static_cast<std::size_t>(row) / m_chunk_capacity;
    m_chunks[chunk_index]->set_entity(static_cast<std::uint32_t>(static_cast<std::size_t>(row) %
                                                                m_chunk_capacity),
                                      entity);
  }

  // Column pointer for `row` of component `id`, pre-offset to that row.
  [[nodiscard]] std::byte* row_bytes(ComponentId id, std::uint32_t row)
  {
    const std::size_t column = column_of(id);
    if (column == kColumnNotFound)
    {
      std::fprintf(stderr, "[ecs] FATAL: archetype has no component %u\n", static_cast<unsigned int>(id));
      std::abort();
    }
    return column_row_bytes(column, row);
  }

  // Same, with the column index already resolved by the caller.
  [[nodiscard]] std::byte* column_row_bytes(std::size_t column, std::uint32_t row)
  {
    const std::size_t chunk_index = static_cast<std::size_t>(row) / m_chunk_capacity;
    const std::size_t local = static_cast<std::size_t>(row) % m_chunk_capacity;
    return m_chunks[chunk_index]->column_bytes(static_cast<std::uint32_t>(column)) +
           local * m_column_specs[column].size;
  }

  [[nodiscard]] std::size_t column_size(std::size_t column) const noexcept
  {
    return m_column_specs[column].size;
  }

  [[nodiscard]] static ColumnSpecs compute_column_specs(const Signature& signature)
  {
    ColumnSpecs specs{};
    for (std::size_t i = 0; i < signature.size() && i < kMaxComponentsPerArchetype; ++i)
    {
      const ComponentType type = component_type(signature[i]);
      specs[i] = ColumnSpec{type.size, type.alignment};
    }
    return specs;
  }

  [[nodiscard]] static ChunkLayout build_chunk_layout(std::size_t component_count,
                                                      const ColumnSpecs& specs,
                                                      std::size_t capacity)
  {
    ChunkLayout layout;
    layout.capacity = capacity;
    layout.entity_offset = align_up(sizeof(Chunk), kCacheLineSize);

    std::size_t cursor = layout.entity_offset + capacity * sizeof(Entity);
    const std::size_t columns = std::min(component_count, kMaxComponentsPerArchetype);
    for (std::size_t i = 0; i < columns; ++i)
    {
      const std::size_t alignment = std::max(kCacheLineSize, specs[i].alignment);
      cursor = align_up(cursor, alignment);
      layout.column_offsets[i] = cursor;
      cursor += capacity * specs[i].size;
    }
    layout.bytes = align_up(cursor, kCacheLineSize);
    return layout;
  }

  // Largest capacity whose layout fits the target chunk size. Layout bytes grow
  // monotonically with capacity, so a binary search is exact. Everything it
  // needs is already materialised in `specs`, so a capacity probe never touches
  // the (mutex-protected) component registry.
  [[nodiscard]] static std::uint32_t solve_chunk_capacity(std::size_t component_count,
                                                          const ColumnSpecs& specs,
                                                          std::size_t target_bytes)
  {
    std::size_t low = 1;
    std::size_t high = kMaxChunkEntities;
    std::size_t best = 1;
    while (low <= high)
    {
      const std::size_t probe = low + (high - low) / 2;
      const ChunkLayout layout = build_chunk_layout(component_count, specs, probe);
      if (layout.bytes <= target_bytes)
      {
        best = probe;
        low = probe + 1;
      }
      else
      {
        high = probe - 1;
      }
    }
    return static_cast<std::uint32_t>(best);
  }

private:
  void create_chunk()
  {
    const std::uint32_t index = static_cast<std::uint32_t>(m_chunks.size());
    auto* const block = static_cast<std::byte*>(m_allocator->allocate(m_layout.bytes));

    std::array<std::byte*, kMaxComponentsPerArchetype> columns{};
    for (std::size_t i = 0; i < m_signature.size(); ++i)
    {
      columns[i] = block + m_layout.column_offsets[i];
    }
    auto* const entity_memory = static_cast<void*>(block + m_layout.entity_offset);
    auto* const entities = static_cast<Entity*>(entity_memory);

    m_chunks.push_back(::new (block) Chunk(this, index, m_chunk_capacity, entities, columns));
  }

  // The tail chunk's live row count is a pure function of m_size, which keeps
  // append/remove and the parallel job walk in agreement without bookkeeping.
  void refresh_tail()
  {
    if (m_chunks.empty())
    {
      return;
    }
    const std::size_t tail_index = m_chunks.size() - 1;
    const std::size_t rows = m_size - tail_index * m_chunk_capacity;
    m_chunks[tail_index]->set_count(static_cast<std::uint32_t>(rows));
  }

  // Removal can leave the final chunk with no rows at all (m_size dropped to
  // exactly (n-1) * capacity). Such a chunk would break the flat row mapping --
  // the next append computes row / capacity and would target a chunk that is
  // already full -- so it is dropped here. Its slab storage is not reclaimed
  // (arenas never free), which is the usual space-for-simplicity trade: only
  // ever one chunk per removal can go empty, and only while the tail drains.
  void compact_tail()
  {
    while (m_chunks.size() > 1 && m_size <= (m_chunks.size() - 1) * m_chunk_capacity)
    {
      m_chunks.pop_back();
    }
    refresh_tail();
  }

  Signature m_signature;
  ChunkAllocator* m_allocator;
  ColumnSpecs m_column_specs;
  std::uint32_t m_chunk_capacity;
  ChunkLayout m_layout;
  std::vector<Chunk*> m_chunks;
  std::size_t m_size{0};
};

} // namespace fiberecs::ecs
