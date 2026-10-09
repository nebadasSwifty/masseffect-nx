// Mass Effect - hot guest hooks: the self-check guard's private shadow of the declared write ranges.
//
// Why: the guard (me_hot_guest.cpp Check) used to run the native version into REAL guest memory, copy its result, write
// the old bytes back and then run the original. Another guest thread touching those bytes inside that window saw the
// native's bytes, then the rolled-back old bytes, and could lose its own stores (suspected cause of the 2026-10-09 EN
// start crash: garbage read from a package buffer while the async IO thread's memcpy was being checked). Now the guard
// writes nothing to guest memory except what the original itself writes:
//
//   1. Writes() gives the ranges the call may write (computed from the inputs, as before).
//   2. ShadowBuild copies them (overlapping / touching ranges merged) into a private per-call buffer, together with some
//      slack around each: kShadowLead bytes (or Writes::read_before) in front, clamp(L, kShadowLead, kShadowTrailMax)
//      behind, the slack outside the declared pages only where ShadowPageFn says it is readable. Guest memory is only
//      read.
//   3. The native version runs compiled a second time with "shadow accessors" (me_hot_shadow.cpp: every n_*.h header is
//      expanded again inside a wrapper namespace with ME_HOT_SHADOW_ACCESSORS, see me_hot_common.h), with
//      t_shadow pointing at the view: Raw() and the scalar stores of a declared address use the private copy; the calling
//      thread's own stack scratch ([r1 - kShadowStackBelow, r1 + kShadowStackAbove)) is used for real (no other thread
//      reads it, the original overwrites it anyway); any other scalar store is dropped into a sink and reported. Reads of
//      everything else see guest memory, still in the pre-call state because the native runs first.
//   4. The original runs for real: the only writer of guest memory, so other threads see exactly an unhooked call.
//   5. Registers / FPCR as before; ShadowCompare: every declared byte of the private copy against guest memory, then the
//      dropped stores and the slack bytes (a native writing next to its declared ranges) as DIFFERENCEs too.
//
// Limits (docs/hot-guard.md has the per-hook table): a store through a raw pointer (Raw(...) + memcpy / NEON) to an
// address outside every window is not redirected (no current native does this: every raw store targets a declared
// range); a raw read run that starts in front of a window by more than its leading slack, or runs past the trailing
// slack, mixes guest memory with the private copy and can give a false DIFFERENCE (never a wrong guest state; the hook is
// then switched off). The CRT memcpy has its own shadow version ShadowMemcpy (its source may lie anywhere inside the
// destination range). Guest code called by a native (the virtual getters of n_8245FF18 / n_82B5F0E8) runs on real memory
// as before; it is read-only in the game. Every path of a native that calls other guest code (allocator, DrawRichMesh,
// lazy class construction) makes Writes() report overflow, so the guard runs only the original for those calls.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include "me_hot_common.h"

namespace me::hot {

// Private bytes kept around a merged declared range of length L. Leading: kShadowLead, so a pointer taken a little below
// the range (a source just below an overlapping destination, an input record next to the output) reads one coherent copy
// across the start. Trailing: clamp(L, kShadowLead, kShadowTrailMax), so a pointer taken inside the range can run past its
// end (a source just above an overlapping destination) and still read the pre-call bytes that follow.
inline constexpr uint32_t kShadowLead = 256;
inline constexpr uint32_t kShadowTrailMax = 4096;
inline uint64_t ShadowTrail(uint64_t len) {
  return len < kShadowLead ? kShadowLead : len > kShadowTrailMax ? kShadowTrailMax : len;
}
// Slack bytes outside the 4 KB guest pages of the declared range are read only through `ShadowPageFn`, which returns a
// pointer that cannot fault (on the Switch the always-mapped shadow alias of a committed page, RexGmShadowFor), or nullptr:
// the slack then stops at that page. Reading the window itself there could fault on an uncommitted page, commit a 4 MB
// chunk of physical memory, or take an emulated read fault on a GPU-watched page. The declared pages are read directly,
// as the original is about to write them.
inline constexpr uint32_t kShadowPage = 0x1000;
using ShadowPageFn = const uint8_t* (*)(const uint8_t* base, uint32_t page);
// For a fully mapped arena (the host fuzzer).
inline const uint8_t* ShadowPageDirect(const uint8_t* base, uint32_t page) { return Raw(base, page); }
inline constexpr uint32_t kShadowStackBelow = 0x1000;  // callee stack scratch the natives may store to for real
inline constexpr uint32_t kShadowStackAbove = 0x80;    // ... and the caller's parameter save area

struct ShadowView {
  struct Win {
    uint32_t lo;               // window start (guest address) = declared start - leading slack
    uint32_t len;              // window length (declared part + both slacks)
    uint32_t dlo;              // merged declared part [dlo, dlo + dlen)
    uint32_t dlen;
    uint8_t* buf;              // private copy of the window (what the native sees and writes)
    const uint8_t* pristine;   // the window as it was before the call (slack check)
  };
  Win win[Writes::kMax];
  int n = 0;
  uint32_t stack_lo = 0, stack_hi = 0;  // [stack_lo, stack_hi): the calling thread's own stack scratch
  bool bad_store = false;               // a scalar store outside every declared range and the stack scratch
  uint32_t bad_addr = 0;
  alignas(16) uint8_t sink[16];         // where such a store goes instead of guest memory
};

// The view the shadow accessors use; set by the guard around one shadow native call (nullptr: plain guest memory).
inline thread_local ShadowView* t_shadow = nullptr;

// Guest memory <-> host buffer, contiguous except across 0xE0000000 on hosts with the +0x1000 physical offset.
inline void GuestCopyOut(const uint8_t* base, uint32_t a, uint32_t n, uint8_t* out) {
  while (n) {
    uint32_t chunk = n;
    if (a < 0xE0000000u && uint64_t(a) + n > 0xE0000000u) chunk = 0xE0000000u - a;
    std::memcpy(out, Raw(base, a), chunk);
    a += chunk;
    out += chunk;
    n -= chunk;
  }
}
// First offset where guest memory [a, a + n) differs from ref, or n.
inline uint32_t GuestFirstDiff(const uint8_t* base, uint32_t a, uint32_t n, const uint8_t* ref) {
  uint32_t done = 0;
  while (done < n) {
    uint32_t chunk = n - done;
    const uint32_t x = a + done;
    if (x < 0xE0000000u && uint64_t(x) + chunk > 0xE0000000u) chunk = 0xE0000000u - x;
    const uint8_t* now = Raw(base, x);
    if (std::memcmp(now, ref + done, chunk) != 0) {
      uint32_t k = 0;
      while (now[k] == ref[done + k]) ++k;
      return done + k;
    }
    done += chunk;
  }
  return n;
}

// Builds the private copy of the declared ranges of w. storage holds the buffers (resized here; the view points into
// it). sp = r1 at the call. false: a range wraps around 4 GB (the guard then runs only the original).
inline bool ShadowBuild(ShadowView& v, const Writes& w, const uint8_t* base, uint32_t sp, std::vector<uint8_t>& storage,
                        ShadowPageFn page_fn) {
  constexpr uint64_t kPage = kShadowPage;
  struct M {
    uint64_t lo, hi;
  } m[Writes::kMax];
  int k = 0;
  for (int i = 0; i < w.n; ++i) {
    const uint64_t lo = w.r[i].addr, hi = lo + w.r[i].len;
    if (hi > 0x100000000ull) return false;
    int j = k++;
    while (j > 0 && m[j - 1].lo > lo) {
      m[j] = m[j - 1];
      --j;
    }
    m[j] = {lo, hi};
  }
  int nm = 0;  // merge overlapping and touching ranges: one contiguous private copy each
  for (int i = 0; i < k; ++i) {
    if (nm > 0 && m[i].lo <= m[nm - 1].hi) {
      if (m[i].hi > m[nm - 1].hi) m[nm - 1].hi = m[i].hi;
    } else {
      m[nm++] = m[i];
    }
  }
  // Pages outside [first declared page, last declared page] are readable only through page_fn.
  auto readable = [&](uint64_t page) { return page_fn != nullptr && page_fn(base, uint32_t(page)) != nullptr; };
  uint64_t lead[Writes::kMax], trail[Writes::kMax];
  for (int i = 0; i < nm; ++i) {
    const uint64_t page_lo = m[i].lo & ~(kPage - 1), page_hi = (m[i].hi + kPage - 1) & ~(kPage - 1);
    uint64_t lo = m[i].lo - std::min<uint64_t>(std::max<uint64_t>(kShadowLead, w.read_before), m[i].lo);
    for (uint64_t p = page_lo; p > lo;) {  // walk down the pages in front of the declared ones
      p -= kPage;
      if (!readable(p)) {
        lo = std::max(lo, p + kPage);
        break;
      }
    }
    uint64_t hi = std::min<uint64_t>(m[i].hi + ShadowTrail(m[i].hi - m[i].lo), 0x100000000ull);
    for (uint64_t p = page_hi; p < hi; p += kPage) {  // and up the pages behind them
      if (!readable(p)) {
        hi = std::min(hi, p);
        break;
      }
    }
    lead[i] = m[i].lo - lo;
    trail[i] = hi - m[i].hi;
  }
  for (int i = 1; i < nm; ++i) {  // two windows never overlap: a gap smaller than both slacks is split
    const uint64_t gap = m[i].lo - m[i - 1].hi;
    if (trail[i - 1] + lead[i] > gap) {
      const uint64_t keep = lead[i] < gap ? gap - lead[i] : 0;  // what the next window's slack leaves free
      trail[i - 1] = std::min<uint64_t>(trail[i - 1], std::max<uint64_t>(gap / 2, keep));
      lead[i] = gap - trail[i - 1];
    }
  }
  size_t total = 0;
  for (int i = 0; i < nm; ++i) total += size_t(lead[i] + (m[i].hi - m[i].lo) + trail[i]);
  storage.resize(2 * total);
  uint8_t* p = storage.data();
  uint8_t* q = storage.data() + total;
  v.n = nm;
  for (int i = 0; i < nm; ++i) {
    ShadowView::Win& x = v.win[i];
    x.lo = uint32_t(m[i].lo - lead[i]);
    x.len = uint32_t(lead[i] + (m[i].hi - m[i].lo) + trail[i]);
    x.dlo = uint32_t(m[i].lo);
    x.dlen = uint32_t(m[i].hi - m[i].lo);
    // Page by page: the declared pages directly, the slack pages through page_fn (checked readable above).
    const uint64_t dpage_lo = m[i].lo & ~(kPage - 1), dpage_hi = (m[i].hi + kPage - 1) & ~(kPage - 1);
    for (uint64_t a = x.lo, end = uint64_t(x.lo) + x.len; a < end;) {
      const uint64_t page = a & ~(kPage - 1);
      const uint32_t n = uint32_t(std::min<uint64_t>(end, page + kPage) - a);
      uint8_t* out = p + (a - x.lo);
      if (page >= dpage_lo && page < dpage_hi) {
        GuestCopyOut(base, uint32_t(a), n, out);
      } else if (const uint8_t* src = page_fn ? page_fn(base, uint32_t(page)) : nullptr) {
        std::memcpy(out, src + (a - page), n);
      } else {
        std::memset(out, 0, n);  // decommitted since the check above (not expected): never read by the comparison
      }
      a += n;
    }
    std::memcpy(q, p, x.len);
    x.buf = p;
    x.pristine = q;
    p += x.len;
    q += x.len;
  }
  v.stack_lo = sp >= kShadowStackBelow ? sp - kShadowStackBelow : 0;
  v.stack_hi = uint32_t(std::min<uint64_t>(uint64_t(sp) + kShadowStackAbove, 0xFFFFFFFFull));
  v.bad_store = false;
  v.bad_addr = 0;
  return true;
}

// Shadow accessors (used by the shadow build of me_hot_common.h only).
// Reads: an address in [window start, end of the declared part) gets the private copy (the leading slack too, so a pointer
// taken just below a range stays in one contiguous copy, and the trailing slack follows it for pointers running past
// the end); anything else, the trailing slack included, is guest memory (unchanged before the original runs).
// Stores: the same part of a window -> private copy; the thread's stack scratch -> real (thread-private; temporaries a
// native keeps there must read back through Raw); the trailing slack -> private copy (reported afterwards); anything
// else -> dropped into the sink and reported. Every store into the slack outside the stack scratch is a DIFFERENCE.
inline uint8_t* ShadowRaw(uint8_t* base, uint32_t a) {
  if (ShadowView* v = t_shadow) {
    for (int i = 0; i < v->n; ++i)
      if (a - v->win[i].lo < (v->win[i].dlo - v->win[i].lo) + v->win[i].dlen) return v->win[i].buf + (a - v->win[i].lo);
  }
  return base + a + PhysOff(a);
}
inline const uint8_t* ShadowRaw(const uint8_t* base, uint32_t a) { return ShadowRaw(const_cast<uint8_t*>(base), a); }

inline uint8_t* ShadowStore(uint8_t* base, uint32_t a, uint32_t n) {
  ShadowView* v = t_shadow;
  if (!v) return base + a + PhysOff(a);
  const uint64_t end = uint64_t(a) + n;
  for (int i = 0; i < v->n; ++i) {
    const ShadowView::Win& x = v->win[i];
    if (a - x.lo < (x.dlo - x.lo) + x.dlen && end <= uint64_t(x.lo) + x.len) return x.buf + (a - x.lo);
  }
  if (a >= v->stack_lo && end <= v->stack_hi) return base + a + PhysOff(a);
  for (int i = 0; i < v->n; ++i)
    if (a - v->win[i].lo < v->win[i].len && end <= uint64_t(v->win[i].lo) + v->win[i].len)
      return v->win[i].buf + (a - v->win[i].lo);
  if (!v->bad_store) {
    v->bad_store = true;
    v->bad_addr = a;
  }
  return v->sink;
}

// The window whose declared part contains [a, a + n), or nullptr.
inline ShadowView::Win* ShadowDeclared(ShadowView& v, uint32_t a, uint32_t n) {
  for (int i = 0; i < v.n; ++i)
    if (a - v.win[i].dlo < v.win[i].dlen && uint64_t(a) + n <= uint64_t(v.win[i].dlo) + v.win[i].dlen) return &v.win[i];
  return nullptr;
}

// Result of the memory comparison after the original ran.
struct ShadowDiff {
  enum Kind { kNone, kMemory, kStore, kSlack } kind = kNone;
  int range = 0;        // kMemory: index in Writes
  uint32_t offset = 0;  // kMemory: offset in that range
  uint32_t addr = 0;    // guest address of the first difference / bad store
  uint8_t native = 0, original = 0;
};

// Native result (private copy) against guest memory after the original: first differing declared byte.
inline ShadowDiff ShadowCompareMemory(ShadowView& v, const Writes& w, const uint8_t* base) {
  ShadowDiff d;
  for (int i = 0; i < w.n; ++i) {
    ShadowView::Win* x = ShadowDeclared(v, w.r[i].addr, w.r[i].len);
    if (!x) continue;  // cannot happen: every declared range is inside its merged window
    const uint8_t* mine = x->buf + (w.r[i].addr - x->lo);
    const uint32_t k = GuestFirstDiff(base, w.r[i].addr, w.r[i].len, mine);
    if (k < w.r[i].len) {
      d.kind = ShadowDiff::kMemory;
      d.range = i;
      d.offset = k;
      d.addr = w.r[i].addr + k;
      d.native = mine[k];
      d.original = *Raw(base, d.addr);
      return d;
    }
  }
  return d;
}

// The native's stores that did not go to a declared range: dropped ones, then changed slack bytes.
inline ShadowDiff ShadowCompareStores(const ShadowView& v) {
  ShadowDiff d;
  if (v.bad_store) {
    d.kind = ShadowDiff::kStore;
    d.addr = v.bad_addr;
    return d;
  }
  for (int i = 0; i < v.n; ++i) {
    const ShadowView::Win& x = v.win[i];
    const uint32_t lead = x.dlo - x.lo, tail0 = lead + x.dlen;
    for (uint32_t k = 0; k < x.len; ++k) {
      if (k == lead) k = tail0;
      if (k >= x.len) break;
      if (x.buf[k] != x.pristine[k] && x.lo + k - v.stack_lo >= v.stack_hi - v.stack_lo) {
        d.kind = ShadowDiff::kSlack;
        d.addr = x.lo + k;
        d.native = x.buf[k];
        d.original = x.pristine[k];
        return d;
      }
    }
  }
  return d;
}

// Everything the guard checks in memory (me_hot_guest.cpp Check).
inline ShadowDiff ShadowCompare(ShadowView& v, const Writes& w, const uint8_t* base) {
  const ShadowDiff d = ShadowCompareMemory(v, w, base);
  return d.kind != ShadowDiff::kNone ? d : ShadowCompareStores(v);
}

// Shadow version of a native: same contract as NativeB (me_hot_guest.h): false = declined.
template <auto Native>
inline bool ShadowCall(PPCContext& ctx, uint8_t* base) {
  if constexpr (std::is_void_v<decltype(Native(ctx, base))>) {
    Native(ctx, base);
    return true;
  } else {
    return Native(ctx, base);
  }
}

// CRT memcpy (n_82AC4AF0) in the shadow: the result of the original (forward copy) for every call the native version
// accepts is the pre-call content of [src, src + n) at [dst, dst + n) (dst <= src overlap: forward copy == memmove; no
// overlap: plain copy). Guest memory is still in the pre-call state when this runs, so the source is read from it
// directly; the generic shadow accessors cannot do this case (a source inside the destination range would be read past
// the end of the private copy). Declines exactly where n_82AC4AF0::Native declines, and also for wrapping ranges of
// n <= 64 (only means that call is not checked).
inline bool ShadowMemcpy(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (n == 0) return true;
  if (uint32_t(dst - src - 1u) < n - 1u) return false;  // dst > src && dst - src < n: the original smears
  if (uint64_t(dst) + n > 0x100000000ull || uint64_t(src) + n > 0x100000000ull) return false;
  ShadowView* v = t_shadow;
  ShadowView::Win* x = v ? ShadowDeclared(*v, dst, n) : nullptr;
  if (!x) return false;  // not declared: never write guest memory from the guard
  GuestCopyOut(base, src, n, x->buf + (dst - x->lo));
  return true;
}

}  // namespace me::hot

// Defines me::hot::NAME::ShadowNative (declared by ME_HOT_HOOK in me_hot_guest.h) as the shadow build of NAME::Native.
// Used in the shadow translation unit after the natives were expanded inside namespace me_hot_shadow_copy.
#define ME_HOT_SHADOW_NATIVE(NAME)                                                         \
  namespace me::hot::NAME {                                                                \
  bool ShadowNative(PPCContext& c, uint8_t* b) {                                           \
    return ::me::hot::ShadowCall<&::me_hot_shadow_copy::me::hot::NAME::Native>(c, b);      \
  }                                                                                        \
  }
