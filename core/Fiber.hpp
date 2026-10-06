#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>

namespace fiberecs
{

// Opaque register file for a suspended execution context. Only the
// platform-specific backend in Fiber.cpp may interpret these fields.
struct FiberContext;

class Fiber
{
public:
  using Entry = std::function<void()>;

  // `stack` must reference at least `stack_size` writable, properly aligned
  // bytes -- typically carved out of a LinearArena. `stack_size` must be a
  // multiple of 16 and at least 32 KiB. Returns nullptr on failure.
  static Fiber* create(void* stack, std::size_t stack_size, Entry entry);
  static void destroy(Fiber* fiber);

  Fiber(const Fiber&) = delete;
  Fiber& operator=(const Fiber&) = delete;
  Fiber(Fiber&&) = delete;
  Fiber& operator=(Fiber&&) = delete;

  // Transfer control into the fiber. Returns when the fiber yields or finishes.
  void resume();

  // Transfer control back to the fiber that resumed this one. Only valid while
  // the fiber is running.
  void yield();

  [[nodiscard]] bool finished() const noexcept
  {
    return m_finished.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t stack_size() const noexcept { return m_stack_size; }
  [[nodiscard]] std::uint64_t switch_count() const noexcept
  {
    return m_switch_count.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t yield_count() const noexcept
  {
    return m_yield_count.load(std::memory_order_relaxed);
  }

  [[nodiscard]] void* stack() const noexcept { return m_stack; }

private:
  friend const char* fiber_backend_name() noexcept;
  Fiber() = default;
  ~Fiber() = default;

  // Installed as the entry point of a freshly built stack frame.
  static void bootstrap(Fiber* self) noexcept;

#if defined(_MSC_VER) && !defined(__clang__)
  // Windows Fiber API start routine (see the OS backend in Fiber.cpp). Runs
  // bootstrap() for the Fiber* passed to CreateFiber. __stdcall matches
  // LPFIBER_START_ROUTINE; on the supported x86_64 target it is ignored.
  static void __stdcall os_trampoline(void* param) noexcept;
#endif

  void* m_stack{nullptr};
  std::size_t m_stack_size{0};
  Entry m_entry;

  // Context of whoever resumed us; where yield() goes.
  FiberContext* m_resumer_ctx{nullptr};

  alignas(64) std::atomic<std::uint64_t> m_switch_count{0};
  alignas(64) std::atomic<std::uint64_t> m_yield_count{0};
  std::atomic<bool> m_finished{false};

  FiberContext* m_ctx{nullptr};
};

// Identifies the active context-switch backend ("x86_64 SysV context
// assembly" or "Windows Fiber API"), which determines stack ownership.
[[nodiscard]] const char* fiber_backend_name() noexcept;

} // namespace fiberecs