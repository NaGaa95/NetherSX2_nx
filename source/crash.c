/* crash.c -- libnx CPU exception handler.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stddef.h>
#include <string.h>

#include "libc_shim.h"
#if defined(USE_VULKAN) && defined(NETHERSX2_VK_DIAGNOSTIC)
#include "hooks/vk.h"
#endif

#define EXCEPTION_SLOT_COUNT 8
#define EXCEPTION_SLOT_SIZE  0x400
#define EXCEPTION_STACK_SIZE 0x10000

typedef struct {
  ThreadExceptionDump dump;
  u8 padding[0x3d0 - sizeof(ThreadExceptionDump)];
  u64 original_x0;
  u64 original_pc;
  u32 deferred;
  u32 result;
  void *frame;
  u32 index;
  u8 tail[EXCEPTION_SLOT_SIZE - 0x3f4];
} ExceptionSlot;

_Static_assert(sizeof(ExceptionSlot) == EXCEPTION_SLOT_SIZE, "invalid exception slot size");
_Static_assert(offsetof(ExceptionSlot, original_x0) == 0x3d0, "invalid original_x0 offset");
_Static_assert(offsetof(ExceptionSlot, original_pc) == 0x3d8, "invalid original_pc offset");
_Static_assert(offsetof(ExceptionSlot, deferred) == 0x3e0, "invalid deferred offset");
_Static_assert(offsetof(ExceptionSlot, result) == 0x3e4, "invalid result offset");
_Static_assert(offsetof(ExceptionSlot, frame) == 0x3e8, "invalid frame offset");
_Static_assert(offsetof(ExceptionSlot, index) == 0x3f0, "invalid index offset");

__attribute__((aligned(0x1000)))
u8 __nx_exception_stack[EXCEPTION_SLOT_COUNT][EXCEPTION_STACK_SIZE];
u64 __nx_exception_stack_size = EXCEPTION_STACK_SIZE;
ExceptionSlot g_exc_slots[EXCEPTION_SLOT_COUNT];
volatile u32 g_exc_slot_mask;
static volatile int g_crashing;

extern void fastmem_fault_trampoline(void);
extern void fastmem_fault_resume_marker(void);

static ExceptionSlot *exception_slot_from_dump(ThreadExceptionDump *dump) {
  const uintptr_t address = (uintptr_t)dump;
  const uintptr_t base = (uintptr_t)g_exc_slots;
  if (address < base || address - base >= sizeof(g_exc_slots) ||
      ((address - base) & (EXCEPTION_SLOT_SIZE - 1)))
    return NULL;
  return (ExceptionSlot *)dump;
}

static void exception_slot_release(ExceptionSlot *slot) {
  if (slot && slot->index < EXCEPTION_SLOT_COUNT)
    __atomic_fetch_and(&g_exc_slot_mask, ~(1u << slot->index), __ATOMIC_RELEASE);
}

int fastmem_run_deferred_fault(ThreadExceptionDump *dump) {
  ExceptionSlot *slot = exception_slot_from_dump(dump);
  if (!slot)
    return 0;
  const int handled = fastmem_dispatch_fault((uintptr_t)slot->original_pc,
                                             (uintptr_t)slot->dump.far.x);
  slot->dump.cpu_gprs[0].x = slot->original_x0;
  slot->dump.pc.x = slot->original_pc;
  return handled;
}

int crash_in_progress(void) {
  return __atomic_load_n(&g_crashing, __ATOMIC_ACQUIRE);
}

// Redirect JIT stores from the RX view to its RW alias.
static int jit_emulate_store(ThreadExceptionDump *ctx, u8 *dst) {
  const u32 insn = *(const volatile u32 *)(uintptr_t)ctx->pc.x;
  const unsigned Rt = insn & 0x1f;
  const unsigned Rn = (insn >> 5) & 0x1f;
  #define GPR(r)     ((r) == 31 ? 0ULL : ctx->cpu_gprs[r].x)          // Rt==31 -> XZR
  #define SETBASE(v) do { if (Rn == 31) ctx->sp.x = (u64)(v); else ctx->cpu_gprs[Rn].x = (u64)(v); } while (0)

  if (((insn >> 27) & 0x7) == 0x5 && ((insn >> 22) & 1) == 0) {
    const unsigned opc = (insn >> 30) & 3, V = (insn >> 26) & 1, Rt2 = (insn >> 10) & 0x1f;
    const unsigned kind = (insn >> 23) & 3;               // 1=post,2=offset,3=pre
    const unsigned scale = V ? (2u + opc) : (opc == 2 ? 3u : 2u);
    const size_t esz = (size_t)1 << scale;
    if (V) { memcpy(dst, &ctx->fpu_gprs[Rt], esz); memcpy(dst + esz, &ctx->fpu_gprs[Rt2], esz); }
    else   { const u64 a = GPR(Rt), b = GPR(Rt2); memcpy(dst, &a, esz); memcpy(dst + esz, &b, esz); }
    if (kind == 3) SETBASE(ctx->far.x);                   // pre-index: Rn = fault addr
    else if (kind == 1) { int i7 = (int)((insn >> 15) & 0x7f); if (i7 & 0x40) i7 -= 0x80;
                          SETBASE((int64_t)ctx->far.x + (int64_t)i7 * (int64_t)esz); } // post
    ctx->pc.x += 4; return 1;
  }
  if (((insn >> 27) & 0x7) == 0x7) {
    const unsigned size = (insn >> 30) & 3, V = (insn >> 26) & 1, opc = (insn >> 22) & 3;
    unsigned scale = size; int is_store;
    if (!V) is_store = (opc == 0);
    else { is_store = (opc == 0 || opc == 2); if (opc == 2) scale = 4; } // 128-bit SIMD
    if (!is_store) return 0;
    const size_t esz = (size_t)1 << scale;
    if (V) memcpy(dst, &ctx->fpu_gprs[Rt], esz);
    else { const u64 v = GPR(Rt); memcpy(dst, &v, esz); }
    if (((insn >> 24) & 3) == 0 && ((insn >> 21) & 1) == 0) { // unscaled/pre/post form
      const unsigned idx = (insn >> 10) & 3;                  // 0=unscaled,1=post,3=pre
      if (idx == 3) SETBASE(ctx->far.x);
      else if (idx == 1) { int i9 = (int)((insn >> 12) & 0x1ff); if (i9 & 0x100) i9 -= 0x200;
                           SETBASE((int64_t)ctx->far.x + i9); }
    }
    ctx->pc.x += 4; return 1;
  }
  return 0;
  #undef GPR
  #undef SETBASE
}

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  if ((uintptr_t)ctx->pc.x >= (uintptr_t)fastmem_fault_resume_marker &&
      (uintptr_t)ctx->pc.x < (uintptr_t)fastmem_fault_resume_marker + 8) {
    ExceptionSlot *original = exception_slot_from_dump(
        (ThreadExceptionDump *)(uintptr_t)ctx->cpu_gprs[19].x);
    if (original && original->deferred) {
      const int handled = original->result != 0;
      memcpy(ctx, &original->dump, sizeof(*ctx));
      exception_slot_release(original);
      if (handled)
        return;
      goto real_crash;
    }
  }

  {
    const unsigned ec = (ctx->esr >> 26) & 0x3f; // 0x24/0x25 = data abort
    if (ec == 0x24 || ec == 0x25) {
      const uintptr_t far = ctx->far.x;
      uintptr_t redirected;
      if (jit_redirect_lookup(far, &redirected) &&
          jit_emulate_store(ctx, (u8 *)redirected))
        return;
    }
  }

  {
    const unsigned ec = (ctx->esr >> 26) & 0x3f;
    if ((ec == 0x24 || ec == 0x25) &&
        fastmem_fault_can_dispatch((uintptr_t)ctx->pc.x, (uintptr_t)ctx->far.x)) {
      ExceptionSlot *slot = exception_slot_from_dump(ctx);
      if (slot) {
        slot->original_x0 = ctx->cpu_gprs[0].x;
        slot->original_pc = ctx->pc.x;
        slot->deferred = 1;
        slot->result = 0;
        ctx->cpu_gprs[0].x = (uintptr_t)ctx;
        ctx->pc.x = (uintptr_t)fastmem_fault_trampoline;
        return;
      }
    }
  }

real_crash:
  if (__atomic_exchange_n(&g_crashing, 1, __ATOMIC_ACQ_REL)) {
    for (;;) svcSleepThread(1000000000ULL);
  }
#if defined(USE_VULKAN) && defined(NETHERSX2_VK_DIAGNOSTIC)
  vk_diag_exception(ctx->pc.x, ctx->far.x, ctx->esr, ctx->sp.x,
                    ctx->fp.x, ctx->lr.x);
#endif
  (void)ctx;
  svcExitProcess();
}
