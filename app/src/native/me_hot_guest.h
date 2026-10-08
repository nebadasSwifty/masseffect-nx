// Mass Effect - hot guest function replacements: hook dispatch with self-check guard (see me_hot_common.h).
//
// Hot-path cost matters here (a hooked function can be called 10^5 times per frame): the Hook object is constant
// initialised (no function-local-static guard: that compiles to an LDAR acquire load per call, measured at ~1.3 % of the
// main thread on the existing sub_82AC4AF0 hook), the only per-call shared state is one plain byte load, and the call
// counter that decides which calls are checked is thread-local (a shared counter would bounce its cache line between
// cores when two threads call the same function).
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "me_hot_common.h"

namespace me::hot {

enum : uint8_t { kUninit = 0, kOff = 1, kOn = 2 };

struct Hook {
  const char* name;
  PPCFunc* orig;
  bool (*native)(PPCContext&, uint8_t*);  // false = declined: nothing was modified, the original must run
  void (*writes)(const PPCContext&, const uint8_t*, Writes&);
  Cmp cmp;
  // Runtime state. Until the first call initialises it the zeros are harmless: kUninit goes to the slow path, and a
  // stale guard_calls/period_mask of 0 on another core only means "check this call".
  std::atomic<uint8_t> state{kUninit};
  uint32_t guard_calls = 0;         // the first N calls of each thread are always checked ...
  uint32_t period_mask = 0;         // ... afterwards every (mask+1)-th call
  std::atomic<bool> failed{false};  // a check found a difference: the original runs from now on
  std::atomic<uint64_t> checks{0};
};

// First call of a hook (any thread): read the cvars. Defined in me_hot_guest.cpp. umbrella = false: the hook is not
// switched on by masseffect_hot_guest, only by its own cvar (hooks added after the production toml enabled the umbrella).
void InitHook(Hook& h, bool own, bool umbrella = true);

// One guarded call: native on a copy of the registers with the written ranges snapshotted, rollback, original,
// compare. Leaves the state exactly as the original would. Defined in me_hot_guest.cpp.
void Check(Hook& h, PPCContext& ctx, uint8_t* base);

// Natives return void (always handle the call) or bool (false = decline this call, e.g. a rare input the native version
// does not reproduce: it must not have modified anything; the original then runs).
template <auto Native>
inline bool NativeB(PPCContext& ctx, uint8_t* base) {
  if constexpr (std::is_void_v<decltype(Native(ctx, base))>) {
    Native(ctx, base);
    return true;
  } else {
    return Native(ctx, base);
  }
}

template <auto Native>
inline void Dispatch(Hook& h, uint32_t& n, PPCContext& ctx, uint8_t* base) {
  if (h.failed.load(std::memory_order_relaxed)) [[unlikely]] {
    h.orig(ctx, base);
    return;
  }
  const uint32_t c = n++;
  if (c < h.guard_calls || (c & h.period_mask) == 0) [[unlikely]] {
    Check(h, ctx, base);
    return;
  }
  if (!NativeB<Native>(ctx, base)) h.orig(ctx, base);
}

}  // namespace me::hot

// CVAR: the hook's own cvar (declared by ME_HOT_CVAR); NS: namespace of the n_*.h header; ADDR: hex address without 0x.
#define ME_HOT_CVAR(CVAR, DESC) \
  REXCVAR_DEFINE_BOOL(CVAR, false, "Mass Effect", DESC).lifecycle(rex::cvar::Lifecycle::kInitOnly)

#define ME_HOT_HOOK(ADDR, CVAR, NS) ME_HOT_HOOK_IMPL(ADDR, CVAR, NS, true)
// Same, but masseffect_hot_guest does not switch it on: only its own cvar does.
#define ME_HOT_HOOK_OWN(ADDR, CVAR, NS) ME_HOT_HOOK_IMPL(ADDR, CVAR, NS, false)

#define ME_HOT_HOOK_IMPL(ADDR, CVAR, NS, UMBRELLA)                                                          \
  REX_EXTERN(__imp__sub_##ADDR);                                                                            \
  REX_HOOK_RAW(sub_##ADDR) {                                                                                \
    static thread_local uint32_t n_calls;                                                                   \
    static me::hot::Hook h{"sub_" #ADDR, &__imp__sub_##ADDR, &me::hot::NativeB<&NS::Native>, &NS::Writes,  \
                           NS::kCmp};                                                                       \
    const uint8_t st = h.state.load(std::memory_order_relaxed);                                             \
    if (st == me::hot::kOn) [[likely]] {                                                                    \
      me::hot::Dispatch<&NS::Native>(h, n_calls, ctx, base);                                                \
      return;                                                                                               \
    }                                                                                                       \
    if (st == me::hot::kUninit) [[unlikely]] {                                                              \
      me::hot::InitHook(h, REXCVAR_GET(CVAR), UMBRELLA);                                                    \
      if (h.state.load(std::memory_order_relaxed) == me::hot::kOn) {                                        \
        me::hot::Dispatch<&NS::Native>(h, n_calls, ctx, base);                                              \
        return;                                                                                             \
      }                                                                                                     \
    }                                                                                                       \
    __imp__sub_##ADDR(ctx, base);                                                                           \
  }
