/**
 * @file        ppc/indirect_dispatch.h
 * @brief       Compact indirect-call dispatch (REX_INDIRECT_DISPATCH = 1 or 2)
 *
 * @remarks     The legacy dispatch (REX_INDIRECT_DISPATCH = 0, the default) reads one 8-byte slot per 4-byte guest
 *              instruction from the per-module table in guest memory at IMAGE_BASE + IMAGE_SIZE (~24 MB for Mass
 *              Effect): every distinct target is a different cache line and, mostly, a different page.
 *
 *              This header provides the two alternatives the generated code can use instead:
 *
 *              1. A compact host-side open-addressing hash table of registered function entry points
 *                 (kTableSize = 2^17 entries x 8 bytes = 1 MB, load ~0.37 for 48K functions). Each entry packs
 *                 the guest address (low 32 bits) and the host function as a signed 32-bit byte offset from
 *                 the anchor function TargetZeroAnchor (high 32 bits), so one aligned 64-bit load gives both
 *                 the key and the value (no torn reads between threads).
 *
 *              2. A per-call-site inline cache (one static uint64_t per bctrl, same packing), checked before
 *                 anything else. Its address does not depend on the target, so the load issues in parallel with
 *                 the vtable load chain. On a miss the site calls the out-of-line miss path, which probes the
 *                 compact table and refills the site.
 *
 *              An all-zero entry decodes to {guest 0, TargetZeroAnchor}: TargetZeroAnchor performs exactly
 *              what the legacy path does for guest target 0 (ResolveIndirectFunction(0), a fatal trap), so empty
 *              table slots and empty inline caches need no extra check in the hit path.
 *
 *              Anything not found in the compact table (mid-function targets, unregistered addresses, imports
 *              resolved through ResolveIndirectFunction, targets whose host offset does not fit in 32 bits) takes
 *              the legacy path in the miss function, so the semantics are unchanged. The table is built lazily
 *              on the first miss from FunctionDispatcher's function map and kept in step by
 *              FunctionDispatcher::SetFunction / UnregisterModule.
 */

#pragma once

#include <cstdint>

#include <rex/ppc/func.h>

namespace rex::runtime::indirect {

inline constexpr uint32_t kTableBits = 17;
inline constexpr uint32_t kTableSize = 1u << kTableBits;
inline constexpr uint32_t kTableMask = kTableSize - 1;
inline constexpr uint32_t kHashMultiplier = 0x9E3779B1u;  // odd; the top bits of the product are the slot

// The NRO is one static PIE: hidden visibility lets the generated code form these addresses with adrp+add
// instead of a GOT load. Desktop builds link the SDK as a shared library and keep default visibility.
#if defined(__SWITCH__)
#define REX_INDIRECT_DISPATCH_VISIBILITY __attribute__((visibility("hidden")))
#else
#define REX_INDIRECT_DISPATCH_VISIBILITY
#endif

// The compact table (host .bss, 1 MB). Written only under the FunctionDispatcher mutex, read lock-free.
REX_INDIRECT_DISPATCH_VISIBILITY extern uint64_t g_table[kTableSize];

// Decoding anchor and target-0 handler: legacy behaviour for a call to guest address 0.
REX_INDIRECT_DISPATCH_VISIBILITY void TargetZeroAnchor(PPCContext& ctx, uint8_t* base);

inline uint32_t Slot(uint32_t guest) {
  return (guest * kHashMultiplier) >> (32 - kTableBits);
}

inline uint64_t LoadEntry(const uint64_t* p) {
  return __atomic_load_n(p, __ATOMIC_RELAXED);
}

inline PPCFunc* Decode(uint64_t entry) {
  return reinterpret_cast<PPCFunc*>(reinterpret_cast<intptr_t>(&TargetZeroAnchor) +
                                    static_cast<intptr_t>(static_cast<int32_t>(entry >> 32)));
}

/**
 * Out-of-line probe of the compact table (builds it on the first call). Returns the host function of a registered
 * entry point, or nullptr (not registered, not representable, or cvar indirect_dispatch_fast = false): the caller
 * then takes the legacy path. When `inline_cache` is not null and the target was found, the call site's cache is
 * filled (cvar indirect_dispatch_ic_refill decides whether a filled site is overwritten).
 */
PPCFunc* Find(uint32_t guest, uint64_t* inline_cache);

/**
 * Hit-path lookup without an inline cache, for host code that calls guest code indirectly (natives).
 * Returns nullptr when the legacy path must be used.
 */
inline PPCFunc* Lookup(uint32_t guest) {
  uint64_t e = LoadEntry(&g_table[Slot(guest)]);
  if (static_cast<uint32_t>(e) == guest) [[likely]] {
    return Decode(e);
  }
  return Find(guest, nullptr);
}

/**
 * Verification (REX_INDIRECT_DISPATCH_VERIFY builds): compares the function the fast path chose with the legacy
 * one. For the first indirect_dispatch_verify_calls calls it returns `legacy_fn` and logs every difference
 * ("indirect_dispatch DIFFERENCE", first 64 only) plus a summary every 2^20 checks; after that it returns `fast_fn`.
 */
PPCFunc* Verify(uint32_t guest, PPCFunc* fast_fn, PPCFunc* legacy_fn);

}  // namespace rex::runtime::indirect

// Table maintenance, called by FunctionDispatcher with its dispatch mutex held (function_dispatcher.cpp).
namespace rex::runtime::indirect::detail {
bool Built();
void BeginRebuild();                      // empties the table (readers then miss and take the legacy path)
void Insert(uint32_t guest, PPCFunc* func);
void EndRebuild(size_t function_count);   // publishes the table, logs a summary
bool Update(uint32_t guest, PPCFunc* func);  // after the build; false = rebuild needed (removal / not representable)
void InvalidateInlineCaches();
PPCFunc* Probe(uint32_t guest);            // table probe only (no build, no cvar); nullptr if absent
}  // namespace rex::runtime::indirect::detail
