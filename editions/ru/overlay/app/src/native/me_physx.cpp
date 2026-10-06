// Mass Effect - PhysX (2.5.1) warnings without formatting them.
//
// WHY (Switch profile)
//   ~13 % of the game's main thread was the CRT printf core (sub_82ACED20 / sub_82ACEBF0) under
//   sub_8299D160, the PhysX SDK's error-stream report: every frame NpScene.cpp line 2033 reports
//   "Scene::fetchResults: simulate() was not called or aborted" (sub_82BF8F78 via sub_82AF1298), and
//   sub_8299D160 vsnprintf's the message into a 160-byte buffer (growing it while it does not fit) before
//   handing it to the game's output stream (virtual call). The message only reaches the game's log.
//
// WHAT IT DOES (masseffect_physx_no_warnings)
//   sub_8299D160(r3 SDK object, r4 code, r5 file, r6 line, r7 out flag, r8 format, r9 va_list): code 107
//   (assertion: the stream decides whether to break) still goes to the original; any other code returns 0
//   without formatting or calling the stream, as the original does when no stream is installed (the
//   last/first error code fields +40/+36 are still updated as in the original).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include <switch.h>

REXCVAR_DEFINE_BOOL(masseffect_physx_no_warnings, false, "Mass Effect",
                    "Drop PhysX SDK warnings/info messages (sub_8299D160) instead of formatting them for the game "
                    "log: the scene reports one every frame (~13 % of the main thread on the Switch)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
std::atomic<uint64_t> g_warnings{0};

// Guest addresses map 1:1 onto base on the Switch (no 0xE0000000 physical offset).
uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}
}  // namespace

REX_EXTERN(__imp__sub_8299D160);
REX_HOOK_RAW(sub_8299D160) {
  static const bool no_warnings = REXCVAR_GET(masseffect_physx_no_warnings);
  if (no_warnings && ctx.r4.u32 != 107) {
    const uint32_t sdk = ctx.r3.u32, code = ctx.r4.u32;
    Store32(base, sdk + 40, code);                       // stw r25,40(r26)
    if (Load32(base, sdk + 36) == 0) Store32(base, sdk + 36, code);  // first error code
    if (g_warnings.fetch_add(1, std::memory_order_relaxed) == 0)
      REXLOG_INFO("[native] PhysX: warnings dropped without formatting (code {}, line {})", ctx.r4.u32,
                  ctx.r6.u32);
    // The every-frame message comes from a busy loop: sub_82320F80 polls fetchResults(NX_RIGID_BODY_FINISHED,
    // block = false) on both scenes until the PhysX threads have run the step. Give the core away on each
    // poll so those threads (same priority 0x3B; with masseffect_exclusive_core 10+K allowed on the main
    // thread's core) can run instead of the spin.
    // YieldType_WithCoreMigration: a plain yield only hands over to threads already queued on this core.
    svcSleepThread(YieldType_WithCoreMigration);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8299D160(ctx, base);
}

// (The native CRT memcpy/wcsstr hooks that lived here were removed: they gave no measurable gain, and even when
// disabled a hook on sub_82AC4AF0 cost ~2.6 % of the main thread's samples: the function-local `static const bool`
// guard is a load-acquire on every call of a function that is called millions of times. Keep hooks of hot
// functions free of function-local statics, and do not hook them at all unless they are enabled.)
