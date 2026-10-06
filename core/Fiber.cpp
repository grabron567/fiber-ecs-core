#include "Fiber.hpp"

#include <cassert>
#include <cstdio>

#if defined(_MSC_VER) && !defined(__clang__)
#define FIBERECS_BACKEND_OS 1
#define FIBERECS_BACKEND_NAME "Windows Fiber API (OS-managed stacks)"
#elif defined(__x86_64__) && defined(FIBERECS_ASM_FIBER) && \
    (defined(__GNUC__) || defined(__clang__))
#define FIBERECS_BACKEND_ASM 1
#define FIBERECS_BACKEND_NAME "x86_64 SysV context assembly (arena-owned stacks)"
#else
#error "Fiber: unsupported platform/compiler combination"
#endif

#if defined(FIBERECS_BACKEND_OS)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fiberecs
{

namespace
{
constexpr std::size_t kMinStackSize = 32u * 1024u;
}

const char* fiber_backend_name() noexcept
{
  return FIBERECS_BACKEND_NAME;
}

// ---------------------------------------------------------------------------
// Backend: x86_64 SysV inline assembly. The fiber stack comes from the caller
// (typically a LinearArena), so switching costs no allocator traffic.
// ---------------------------------------------------------------------------
#if defined(FIBERECS_BACKEND_ASM)

// Layout is mirrored by fiber_context.S. rdi/rsi are only carried on ABIs where
// they are callee-saved (Microsoft x64); on SysV they are caller-saved, so the
// switch is free to clobber them.
struct FiberContext
{
  std::uint64_t rsp{0}; // stack pointer at the resumption point
  std::uint64_t rip{0}; // instruction to resume at
  std::uint64_t rbx{0};
  std::uint64_t rbp{0};
  std::uint64_t r12{0};
  std::uint64_t r13{0};
  std::uint64_t r14{0};
  std::uint64_t r15{0};
  std::uint64_t rdi{0};
  std::uint64_t rsi{0};
};

// Defined in core/fiber_context.S. See that file for the ABI notes.
extern "C" void fiberecs_switch_asm(FiberContext* to, FiberContext* from) noexcept;
extern "C" void fiberecs_bootstrap_asm() noexcept;

namespace
{
// Root context of a thread: the target of the first switch when no fiber has
// been resumed here yet.
FiberContext& root_context()
{
  thread_local FiberContext ctx{};
  return ctx;
}

FiberContext*& current_context()
{
  thread_local FiberContext* ctx = &root_context();
  return ctx;
}
} // namespace

void Fiber::bootstrap(Fiber* self) noexcept
{
  self->m_entry();
  self->m_finished.store(true, std::memory_order_release);

  // Hand control back to the resumer. resume() asserts against resuming a
  // finished fiber, so this loop is only defensive.
  FiberContext* back = self->m_resumer_ctx;
  if (back == nullptr)
  {
    return;
  }
  for (;;)
  {
    fiberecs_switch_asm(back, self->m_ctx);
  }
}

Fiber* Fiber::create(void* stack, std::size_t stack_size, Entry entry)
{
  assert(stack != nullptr && "Fiber::create requires caller-provided storage");
  assert(stack_size >= kMinStackSize && "fiber stack too small");
  assert((stack_size % 16) == 0 && "fiber stack size must be a multiple of 16");
  assert(entry != nullptr && "Fiber::create requires a callable entry point");

  void* mem = ::operator new(sizeof(Fiber), std::align_val_t{alignof(Fiber)});
  Fiber* fiber = ::new (mem) Fiber();
  fiber->m_stack = stack;
  fiber->m_stack_size = stack_size;
  fiber->m_entry = std::move(entry);

  auto* ctx = static_cast<FiberContext*>(::operator new(sizeof(FiberContext)));
  fiber->m_ctx = ctx;

  // Build the initial frame. fiberecs_switch_asm restores rsp, pushes rip and
  // rets, so the trampoline is entered with rsp == 0 (mod 16) -- exactly the
  // state a `call` instruction leaves behind. That matters: the trampoline
  // immediately issues a `call` into Fiber::bootstrap, and an 8-byte skew here
  // would hand the C++ code a misaligned stack and fault on any 16-byte move.
  const std::uintptr_t top = reinterpret_cast<std::uintptr_t>(stack) + stack_size;
  const std::uintptr_t stack_top = top & ~static_cast<std::uintptr_t>(0xF);

  ctx->rsp = stack_top;
  ctx->rip = reinterpret_cast<std::uint64_t>(&fiberecs_bootstrap_asm);
  ctx->rbx = 0;
  ctx->rbp = 0;
  ctx->r12 = reinterpret_cast<std::uint64_t>(fiber);              // trampoline argument
  ctx->r13 = reinterpret_cast<std::uint64_t>(&Fiber::bootstrap);  // indirect entry
  ctx->r14 = 0;
  ctx->r15 = 0;
  ctx->rdi = 0;
  ctx->rsi = 0;

  return fiber;
}

void Fiber::destroy(Fiber* fiber)
{
  if (fiber == nullptr)
  {
    return;
  }
  ::operator delete(fiber->m_ctx);
  ::operator delete(static_cast<void*>(fiber), std::align_val_t{alignof(Fiber)});
}

void Fiber::resume()
{
  assert(!finished() && "resume() on a finished fiber");
  assert(m_resumer_ctx == nullptr && "fiber already resumed (no re-entrancy)");

  FiberContext* const from = current_context();
  m_resumer_ctx = from;
  m_switch_count.fetch_add(1, std::memory_order_relaxed);
  fiberecs_switch_asm(m_ctx, from);
  m_resumer_ctx = nullptr;
}

void Fiber::yield()
{
  assert(m_resumer_ctx != nullptr && "yield() outside of a resumed fiber");
  m_yield_count.fetch_add(1, std::memory_order_relaxed);
  fiberecs_switch_asm(m_resumer_ctx, m_ctx);
}

// ---------------------------------------------------------------------------
// Backend: Windows Fiber API (MSVC, where x64 inline asm is unavailable).
// The OS owns the fiber stacks; the arena pointer is retained for diagnostics.
// ---------------------------------------------------------------------------
#else

struct FiberContext
{
  void* os_fiber{nullptr};
  void* resumer_os_fiber{nullptr};
};

namespace
{
struct TrampolineArg
{
  Fiber* fiber;
};

thread_local TrampolineArg t_trampoline_arg{};

VOID WINAPI os_trampoline(PVOID param)
{
  auto* arg = static_cast<TrampolineArg*>(param);
  Fiber* self = arg->fiber;
  self->m_entry();
  self->m_finished.store(true, std::memory_order_release);
  return VOID();
}

// Converts the calling thread to a fiber once; every subsequent switch on this
// thread requires it.
FiberContext& root_context()
{
  thread_local FiberContext ctx{};
  if (ctx.os_fiber == nullptr)
  {
    void* main_fiber = ConvertThreadToFiber(nullptr);
    if (main_fiber == nullptr)
    {
      std::fprintf(stderr, "[fiberecs] ConvertThreadToFiber failed (error=%lu)\n", GetLastError());
      std::abort();
    }
    ctx.os_fiber = main_fiber;
  }
  return ctx;
}
} // namespace

void Fiber::bootstrap(Fiber* self) noexcept
{
  self->m_entry();
  self->m_finished.store(true, std::memory_order_release);
}

Fiber* Fiber::create(void* stack, std::size_t stack_size, Entry entry)
{
  assert(stack != nullptr && "Fiber::create requires caller-provided storage");
  assert(stack_size >= kMinStackSize && "fiber stack too small");
  assert((stack_size % 16) == 0 && "fiber stack size must be a multiple of 16");
  assert(entry != nullptr && "Fiber::create requires a callable entry point");

  void* mem = ::operator new(sizeof(Fiber), std::align_val_t{alignof(Fiber)});
  Fiber* fiber = ::new (mem) Fiber();
  fiber->m_stack = stack;
  fiber->m_stack_size = stack_size;
  fiber->m_entry = std::move(entry);

  auto* ctx = static_cast<FiberContext*>(::operator new(sizeof(FiberContext)));
  fiber->m_ctx = ctx;

  root_context(); // the resumer must be a fiber before this fiber can run

  t_trampoline_arg.fiber = fiber;
  ctx->os_fiber = CreateFiber(stack_size, &os_trampoline, &t_trampoline_arg);
  if (ctx->os_fiber == nullptr)
  {
    std::fprintf(stderr, "[fiberecs] CreateFiber failed (error=%lu)\n", GetLastError());
    ::operator delete(ctx);
    ::operator delete(mem, std::align_val_t{alignof(Fiber)});
    return nullptr;
  }
  return fiber;
}

void Fiber::destroy(Fiber* fiber)
{
  if (fiber == nullptr)
  {
    return;
  }
  if (GetCurrentFiber() == fiber->m_ctx->os_fiber)
  {
    ConvertFiberToThread(); // leave fiber mode before freeing the context
  }
  if (fiber->m_ctx != nullptr)
  {
    if (fiber->m_ctx->os_fiber != nullptr)
    {
      DeleteFiber(fiber->m_ctx->os_fiber);
    }
    ::operator delete(fiber->m_ctx);
  }
  ::operator delete(static_cast<void*>(fiber), std::align_val_t{alignof(Fiber)});
}

void Fiber::resume()
{
  assert(!finished() && "resume() on a finished fiber");
  assert(m_resumer_ctx == nullptr && "fiber already resumed (no re-entrancy)");

  FiberContext& root = root_context();
  m_resumer_ctx = &root;
  m_switch_count.fetch_add(1, std::memory_order_relaxed);
  SwitchToFiber(m_ctx->os_fiber);
  m_resumer_ctx = nullptr;
}

void Fiber::yield()
{
  assert(m_resumer_ctx != nullptr && "yield() outside of a resumed fiber");
  m_yield_count.fetch_add(1, std::memory_order_relaxed);
  SwitchToFiber(m_resumer_ctx->os_fiber);
}

#endif // backend selection

} // namespace fiberecs