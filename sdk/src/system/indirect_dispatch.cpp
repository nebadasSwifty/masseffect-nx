/**
 * @file        system/indirect_dispatch.cpp
 * @brief       Compact indirect-call dispatch table and inline-cache support (see rex/ppc/indirect_dispatch.h)
 *
 * Only used by code generated with REX_INDIRECT_DISPATCH = 1 or 2 (masseffect: CMake MASSEFFECT_INDIRECT_DISPATCH).
 * With the default (0) nothing here runs: the table stays empty and is never built.
 */

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/indirect_dispatch.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

REXCVAR_DEFINE_BOOL(indirect_dispatch_fast, true, "CPU",
                    "Builds with MASSEFFECT_INDIRECT_DISPATCH 1/2 only: use the compact 1 MB dispatch table and the "
                    "per-call-site inline caches. false = every guest indirect call takes the legacy table lookup "
                    "(through the out-of-line miss path), for a run-time A/B in the same build. Read when the table "
                    "is first needed: set it in the toml, not at run time");
REXCVAR_DEFINE_BOOL(indirect_dispatch_ic_refill, true, "CPU",
                    "MASSEFFECT_INDIRECT_DISPATCH 2: an inline-cache miss overwrites the call site's cached target "
                    "(last target wins). false = a site keeps its first target and polymorphic sites never store "
                    "again (no stores in steady state, misses always go through the table)");
REXCVAR_DEFINE_BOOL(indirect_dispatch_hot_cache, false, "CPU",
                    "MASSEFFECT_INDIRECT_DISPATCH 1/2: a 4 KB cache of the targets that missed their call site's inline "
                    "cache (or the compact table's home slot), checked before the 1 MB table. Polymorphic call sites "
                    "(virtual Tick loops) miss all the time and every miss used to load a random line of the 1 MB "
                    "table. Same results (entries are the table's own {guest, host} words). Read when the table is "
                    "built: set it in the toml");
REXCVAR_DEFINE_BOOL(indirect_dispatch_hot_cache_verify, false, "CPU",
                    "With indirect_dispatch_hot_cache: also probe the compact table on every hot-cache hit and log "
                    "\"indirect_dispatch hot cache DIFFERENCE\" (first 16) plus a summary every 2^22 hits. Expected: 0");
REXCVAR_DEFINE_INT32(indirect_dispatch_verify_calls, 1000000, "CPU",
                     "Builds with MASSEFFECT_INDIRECT_DISPATCH_VERIFY only: compare the fast and the legacy result "
                     "for this many indirect calls (the legacy one is used), log every DIFFERENCE (first 64)");

namespace rex::runtime::indirect {

alignas(64) uint64_t g_table[kTableSize];

// Hot-miss cache: direct-mapped, kHotSize entries in the table's packing (one 64-bit word = key + value), shared by all
// threads. 4 KB stays in the L1 data cache; the hot set of the 1 MB table (~1,100 lines over ~250 pages) does not.
constexpr uint32_t kHotBits = 9;
constexpr uint32_t kHotSize = 1u << kHotBits;
alignas(64) uint64_t g_hot[kHotSize];

namespace {

// Load above this fraction: further inserts are refused (the target then takes the legacy path).
constexpr uint32_t kMaxEntries = kTableSize / 4 * 3;

std::atomic<bool> g_built{false};
uint32_t g_entries = 0;           // under the dispatcher mutex
uint32_t g_unrepresentable = 0;   // under the dispatcher mutex
uint32_t g_refused = 0;           // under the dispatcher mutex

std::mutex g_ic_mutex;
std::vector<uint64_t*> g_ic_sites;  // every inline cache that was ever filled (for invalidation)

// Latched when the table is published (EndRebuild): the hot path then reads plain flags instead of the cvar accessors.
std::atomic<bool> g_hot_on{false};
bool g_hot_verify = false;
bool g_ic_refill = true;
std::atomic<uint64_t> g_hot_checks{0};
std::atomic<uint64_t> g_hot_differences{0};

inline uint32_t HotSlot(uint32_t guest) {
  return (guest * kHashMultiplier) >> (32 - kHotBits);
}

void ClearHot() {
  for (uint32_t i = 0; i < kHotSize; ++i) {
    __atomic_store_n(&g_hot[i], uint64_t{0}, __ATOMIC_RELAXED);
  }
}

std::atomic<uint64_t> g_verify_checks{0};
std::atomic<uint64_t> g_verify_differences{0};

inline void StoreEntry(uint64_t* p, uint64_t e) {
  __atomic_store_n(p, e, __ATOMIC_RELAXED);
}

// 0 if not representable (offset 0 is the anchor itself, which is never a registered function).
uint64_t Encode(uint32_t guest, PPCFunc* func) {
  intptr_t delta = reinterpret_cast<intptr_t>(func) - reinterpret_cast<intptr_t>(&TargetZeroAnchor);
  if (delta == 0 || delta != static_cast<intptr_t>(static_cast<int32_t>(delta))) {
    return 0;
  }
  return (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(delta))) << 32) | guest;
}

// Slot holding `guest`, or the first empty slot of its probe sequence (kTableSize if the table is full).
uint32_t FindSlot(uint32_t guest, bool* found) {
  uint32_t s = Slot(guest);
  for (uint32_t i = 0; i < kTableSize; ++i, s = (s + 1) & kTableMask) {
    uint64_t e = LoadEntry(&g_table[s]);
    if (e == 0) {
      *found = false;
      return s;
    }
    if (static_cast<uint32_t>(e) == guest) {
      *found = true;
      return s;
    }
  }
  *found = false;
  return kTableSize;
}

void FillInlineCache(uint64_t* ic, uint64_t e) {
  uint64_t old = LoadEntry(ic);
  if (old == e) {
    return;
  }
  if (old != 0 && !(g_hot_on.load(std::memory_order_relaxed) ? g_ic_refill : REXCVAR_GET(indirect_dispatch_ic_refill))) {
    return;
  }
  if (old == 0) {
    // First fill of this site: remember it so a later remapping can clear it.
    std::lock_guard lock(g_ic_mutex);
    g_ic_sites.push_back(ic);
  }
  StoreEntry(ic, e);
}

void EnsureBuilt() {
  Runtime* runtime = Runtime::instance();
  FunctionDispatcher* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  if (dispatcher) {
    dispatcher->BuildIndirectDispatchTable();
  }
}

}  // namespace

void TargetZeroAnchor(PPCContext& ctx, uint8_t* base) {
  // Exactly the legacy REX_CALL_INDIRECT_FUNC for target 0 (outside every code range).
  ctx.last_indirect_target = 0;
  rex::runtime::ResolveIndirectFunction(0)(ctx, base);
}

namespace {

// Table probe for the hot-cache path (the table is built and indirect_dispatch_fast was true when it was published).
[[gnu::noinline]] PPCFunc* FindViaTable(uint32_t guest, uint64_t* inline_cache, uint64_t* hot) {
  bool found = false;
  uint32_t s = guest != 0 ? FindSlot(guest, &found) : kTableSize;
  if (!found) {
    return nullptr;
  }
  uint64_t e = LoadEntry(&g_table[s]);
  if (static_cast<uint32_t>(e) != guest || e == 0) {
    return nullptr;
  }
  __atomic_store_n(hot, e, __ATOMIC_RELAXED);
  if (inline_cache) {
    FillInlineCache(inline_cache, e);
  }
  return Decode(e);
}

[[gnu::noinline]] void VerifyHot(uint32_t guest, uint64_t e) {
  PPCFunc* table = detail::Probe(guest);
  uint64_t n = g_hot_checks.fetch_add(1, std::memory_order_relaxed) + 1;
  if (table != Decode(e)) {
    uint64_t d = g_hot_differences.fetch_add(1, std::memory_order_relaxed);
    if (d < 16) {
      REXLOG_ERROR("indirect_dispatch hot cache DIFFERENCE: guest {:08X} hot {} table {}", guest,
                   reinterpret_cast<void*>(Decode(e)), reinterpret_cast<void*>(table));
    }
  }
  if ((n & ((1u << 22) - 1)) == 0) {
    REXLOG_INFO("indirect_dispatch hot cache verify: {} hits checked, {} differences", n,
                g_hot_differences.load(std::memory_order_relaxed));
  }
}

}  // namespace

PPCFunc* Find(uint32_t guest, uint64_t* inline_cache) {
  if (g_hot_on.load(std::memory_order_relaxed)) [[likely]] {
    // Entries are self-validating (key and value in one aligned word): a stale or empty slot only misses.
    uint64_t* hot = &g_hot[HotSlot(guest)];
    uint64_t e = LoadEntry(hot);
    if (static_cast<uint32_t>(e) == guest && e != 0) [[likely]] {
      if (g_hot_verify) [[unlikely]] {
        VerifyHot(guest, e);
      }
      if (inline_cache) {
        FillInlineCache(inline_cache, e);
      }
      return Decode(e);
    }
    return FindViaTable(guest, inline_cache, hot);
  }
  if (!REXCVAR_GET(indirect_dispatch_fast)) [[unlikely]] {
    return nullptr;
  }
  if (!g_built.load(std::memory_order_acquire)) [[unlikely]] {
    EnsureBuilt();
    if (!g_built.load(std::memory_order_acquire)) {
      return nullptr;
    }
  }
  bool found = false;
  uint32_t s = guest != 0 ? FindSlot(guest, &found) : kTableSize;
  if (!found) {
    return nullptr;
  }
  uint64_t e = LoadEntry(&g_table[s]);
  if (static_cast<uint32_t>(e) != guest || e == 0) {  // changed under us (rebuild): use the legacy path
    return nullptr;
  }
  if (inline_cache) {
    FillInlineCache(inline_cache, e);
  }
  return Decode(e);
}

PPCFunc* Verify(uint32_t guest, PPCFunc* fast_fn, PPCFunc* legacy_fn) {
  uint64_t limit = static_cast<uint64_t>(std::max<int32_t>(0, REXCVAR_GET(indirect_dispatch_verify_calls)));
  uint64_t n = g_verify_checks.fetch_add(1, std::memory_order_relaxed);
  if (n >= limit) {
    return fast_fn;
  }
  // Target 0: the fast path calls TargetZeroAnchor, which calls the legacy result itself.
  bool same = fast_fn == legacy_fn || (guest == 0 && fast_fn == &TargetZeroAnchor);
  if (!same) {
    uint64_t d = g_verify_differences.fetch_add(1, std::memory_order_relaxed);
    if (d < 64) {
      REXLOG_ERROR("indirect_dispatch DIFFERENCE: guest {:08X} fast {} legacy {} (check {})", guest,
                   reinterpret_cast<void*>(fast_fn), reinterpret_cast<void*>(legacy_fn), n);
    }
  }
  if (((n + 1) & ((1u << 20) - 1)) == 0 || n + 1 == limit) {
    REXLOG_INFO("indirect_dispatch verify: {} checks, {} differences{}", n + 1,
                g_verify_differences.load(std::memory_order_relaxed),
                n + 1 == limit ? " (limit reached, fast path only from now on)" : "");
  }
  return legacy_fn;
}

namespace detail {

bool Built() {
  return g_built.load(std::memory_order_relaxed);
}

void BeginRebuild() {
  g_hot_on.store(false, std::memory_order_relaxed);
  g_built.store(false, std::memory_order_relaxed);
  ClearHot();
  for (uint32_t i = 0; i < kTableSize; ++i) {
    StoreEntry(&g_table[i], 0);
  }
  g_entries = 0;
  g_unrepresentable = 0;
  g_refused = 0;
}

void Insert(uint32_t guest, PPCFunc* func) {
  if (!func || guest == 0) {
    return;
  }
  uint64_t e = Encode(guest, func);
  if (!e) {
    ++g_unrepresentable;
    return;
  }
  bool found = false;
  uint32_t s = FindSlot(guest, &found);
  if (!found && (s == kTableSize || g_entries >= kMaxEntries)) {
    ++g_refused;
    return;
  }
  if (!found) {
    ++g_entries;
  }
  StoreEntry(&g_table[s], e);
}

void EndRebuild(size_t function_count) {
  uint32_t home = 0;
  uint32_t max_probe = 0;
  for (uint32_t i = 0; i < kTableSize; ++i) {
    uint64_t e = g_table[i];
    if (!e) {
      continue;
    }
    uint32_t dist = (i - Slot(static_cast<uint32_t>(e))) & kTableMask;
    home += dist == 0;
    max_probe = std::max(max_probe, dist);
  }
  g_ic_refill = REXCVAR_GET(indirect_dispatch_ic_refill);
  g_hot_verify = REXCVAR_GET(indirect_dispatch_hot_cache_verify);
  ClearHot();
  g_built.store(true, std::memory_order_release);
  const bool hot = REXCVAR_GET(indirect_dispatch_hot_cache) && REXCVAR_GET(indirect_dispatch_fast);
  g_hot_on.store(hot, std::memory_order_release);
  if (hot) {
    REXLOG_INFO("indirect_dispatch: hot-miss cache on ({} entries, {} KB){}", kHotSize, sizeof(g_hot) / 1024,
                g_hot_verify ? ", verifying every hit against the table" : "");
  }
  REXLOG_INFO(
      "indirect_dispatch: table built: {} functions, {} entries in {} slots ({} KB), {:.1f} % in their home "
      "slot, longest probe {}, {} not representable, {} refused",
      function_count, g_entries, kTableSize, sizeof(g_table) / 1024,
      g_entries ? 100.0 * home / g_entries : 0.0, max_probe, g_unrepresentable, g_refused);
}

bool Update(uint32_t guest, PPCFunc* func) {
  if (guest == 0) {
    return true;
  }
  bool found = false;
  uint32_t s = FindSlot(guest, &found);
  uint64_t e = func ? Encode(guest, func) : 0;
  if (!e) {
    if (func) {
      ++g_unrepresentable;
    }
    return !found;  // a stale entry must go: rebuild
  }
  if (found) {
    if (LoadEntry(&g_table[s]) != e) {
      StoreEntry(&g_table[s], e);
      InvalidateInlineCaches();  // also clears the hot-miss cache
    }
    return true;
  }
  if (s == kTableSize || g_entries >= kMaxEntries) {
    ++g_refused;
    return true;
  }
  ++g_entries;
  StoreEntry(&g_table[s], e);
  return true;
}

void InvalidateInlineCaches() {
  ClearHot();
  std::lock_guard lock(g_ic_mutex);
  if (!g_ic_sites.empty()) {
    REXLOG_WARN("indirect_dispatch: function mapping changed, clearing {} inline caches", g_ic_sites.size());
  }
  for (uint64_t* ic : g_ic_sites) {
    StoreEntry(ic, 0);
  }
  g_ic_sites.clear();
}

PPCFunc* Probe(uint32_t guest) {
  bool found = false;
  uint32_t s = guest != 0 ? FindSlot(guest, &found) : kTableSize;
  return found ? Decode(LoadEntry(&g_table[s])) : nullptr;
}

}  // namespace detail

}  // namespace rex::runtime::indirect
