// Harness main (see fuzz.h). Cases are pulled in by the generated cases_all.inc.
#include "fuzz.h"

#include <chrono>

#include <rex/ppc/context.h>
#include <rex/ppc/intrinsics.h>

#include "natives_all.inc"

#include "me_hot_shadow.h"

#include "cases_all.inc"

using namespace fuzz;

namespace fuzz {
// The shadow build of a case's native (generated shadow_all.cpp), or nullptr.
using ShadowFn = bool (*)(PPCContext&, uint8_t*);
ShadowFn ShadowFor(const char* name);
}  // namespace fuzz

namespace {

const PPCRegister* Gpr(const PPCContext& c, int n) {
  switch (n) {
#define G(i) case i: return &c.r##i;
    G(0) G(1) G(2) G(3) G(4) G(5) G(6) G(7) G(8) G(9) G(10) G(11) G(12) G(13) G(14) G(15) G(16) G(17) G(18) G(19)
    G(20) G(21) G(22) G(23) G(24) G(25) G(26) G(27) G(28) G(29) G(30) G(31)
#undef G
  }
  return nullptr;
}
const PPCRegister* Fpr(const PPCContext& c, int n) {
  switch (n) {
#define F(i) case i: return &c.f##i;
    F(0) F(1) F(2) F(3) F(4) F(5) F(6) F(7) F(8) F(9) F(10) F(11) F(12) F(13) F(14) F(15) F(16) F(17) F(18) F(19)
    F(20) F(21) F(22) F(23) F(24) F(25) F(26) F(27) F(28) F(29) F(30) F(31)
#undef F
  }
  return nullptr;
}
const PPCVRegister* Vr(const PPCContext& c, int n) {
  switch (n) {
#define V(i) case i: return &c.v##i;
    V(0) V(1) V(2) V(3) V(4) V(5) V(6) V(7) V(8) V(9) V(10) V(11) V(12) V(13) V(14) V(15) V(16) V(17) V(18) V(19)
    V(20) V(21) V(22) V(23) V(24) V(25) V(26) V(27) V(28) V(29) V(30) V(31)
#undef V
  }
  return nullptr;
}

bool Covered(const me::hot::Writes& w, uint32_t addr) {
  for (int i = 0; i < w.n; ++i)
    if (addr >= w.r[i].addr && addr - w.r[i].addr < w.r[i].len) return true;
  return false;
}

struct Stats {
  uint64_t iters = 0, fails = 0, r3zero = 0, r3one = 0, r3other = 0;
  uint64_t scratch[3][32] = {};  // [gpr/fpr/vr][n]: volatile register differs but is not in kCmp
  uint64_t fpcr_diff = 0;
};

// Returns an empty string when identical (for the registers kCmp selects + the always-compared set).
std::string CompareRegs(const me::hot::Cmp& cmp, const PPCContext& n, const PPCContext& o, Stats& st) {
  char buf[256];
  for (int i = 0; i < 32; ++i) {
    const bool vol = (i == 0 || (i >= 3 && i <= 12));
    const bool want = vol ? ((cmp.gpr >> i) & 1) != 0 : (i == 1 || i >= 13);
    if (Gpr(n, i)->u64 != Gpr(o, i)->u64) {
      if (want) {
        std::snprintf(buf, sizeof buf, "r%d native=%#" PRIx64 " original=%#" PRIx64, i, Gpr(n, i)->u64, Gpr(o, i)->u64);
        return buf;
      }
      if (vol) st.scratch[0][i]++;
    }
  }
  for (int i = 0; i < 32; ++i) {
    const bool vol = i < 14;
    const bool want = vol ? ((cmp.fpr >> i) & 1) != 0 : true;
    if (Fpr(n, i)->u64 != Fpr(o, i)->u64) {
      if (want) {
        std::snprintf(buf, sizeof buf, "f%d native=%#" PRIx64 " original=%#" PRIx64, i, Fpr(n, i)->u64, Fpr(o, i)->u64);
        return buf;
      }
      if (vol) st.scratch[1][i]++;
    }
  }
  for (int i = 0; i < 32; ++i) {
    const bool vol = i < 14;
    const bool want = vol ? ((cmp.vr >> i) & 1) != 0 : true;
    if (std::memcmp(Vr(n, i)->u8, Vr(o, i)->u8, 16) != 0) {
      if (want) {
        const uint32_t* x = Vr(n, i)->u32;
        const uint32_t* y = Vr(o, i)->u32;
        std::snprintf(buf, sizeof buf, "v%d native=%08x%08x%08x%08x original=%08x%08x%08x%08x", i, x[0], x[1], x[2], x[3],
                      y[0], y[1], y[2], y[3]);
        return buf;
      }
      if (vol) st.scratch[2][i]++;
    }
  }
  if (n.fpscr.csr != o.fpscr.csr) {
    std::snprintf(buf, sizeof buf, "fpscr.csr native=%#x original=%#x", n.fpscr.csr, o.fpscr.csr);
    return buf;
  }
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t iters = 20000, seed = 1;
  bool bench = false;
  std::vector<std::string> filter;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--iters" && i + 1 < argc) iters = std::strtoull(argv[++i], nullptr, 0);
    else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 0);
    else if (a == "--bench") bench = true;
    else filter.push_back(a);
  }
  Arena A, B;
  int total_fail = 0;
  for (const Case& c : Registry()) {
    if (!filter.empty()) {
      bool ok = false;
      for (auto& f : filter) ok |= (std::string(c.name).find(f) != std::string::npos);
      if (!ok) continue;
    }
    Stats st;
    Rng rng(seed ^ std::hash<std::string>()(c.name));
    std::vector<uint8_t> init(c.win_len);
    int shown = 0;
    for (uint64_t it = 0; it < iters; ++it) {
      PPCContext* co = new PPCContext();
      // garbage in every register so any hidden dependence shows
      for (int i = 0; i < 32; ++i) const_cast<PPCRegister*>(Gpr(*co, i))->u64 = rng.u64();
      for (int i = 0; i < 32; ++i) const_cast<PPCRegister*>(Fpr(*co, i))->u64 = rng.u64();
      for (int i = 0; i < 32; ++i) {
        PPCVRegister* v = const_cast<PPCVRegister*>(Vr(*co, i));
        v->u64[0] = rng.u64();
        v->u64[1] = rng.u64();
      }
      co->r1.u64 = StackTop(c);
      co->lr = 0x82210000 + rng.below(0x1000) * 4;
      // memory: random garbage with random structure, then the generator
      for (uint32_t k = 0; k < c.win_len; k += 8) {
        uint64_t x = rng.u64();
        std::memcpy(A.base + c.win_addr + k, &x, 8);
      }
      c.gen(rng, A.base, *co);
      co->r1.u64 = StackTop(c);
      std::memcpy(init.data(), A.base + c.win_addr, c.win_len);
      std::memcpy(B.base + c.win_addr, A.base + c.win_addr, c.win_len);
      // FPU mode: random start state (the original and the native version both start from it)
      co->fpscr.csr = co->fpscr.getcsr();
      if (rng.chance(0.5)) co->fpscr.enableFlushModeUnconditional();
      else co->fpscr.disableFlushModeUnconditional();
      const uint32_t hw0 = co->fpscr.getcsr();
      me::hot::Writes w;
      c.writes(*co, A.base, w);
      const uint64_t in_r[6] = {co->r3.u64, co->r4.u64, co->r5.u64, co->r6.u64, co->r7.u64, co->r8.u64};
      PPCContext* cn = new PPCContext(*co);
      PPCContext* c0 = new PPCContext(*co);  // the inputs, for the guard replay
      c.orig(*co, A.base);
      const uint32_t hw_o = co->fpscr.getcsr();
      co->fpscr.setcsr(hw0);
      c.nat(*cn, B.base);
      const uint32_t hw_n = cn->fpscr.getcsr();
      cn->fpscr.setcsr(hw0);
      std::string diff = CompareRegs(c.cmp, *cn, *co, st);
      if (diff.empty() && hw_n != hw_o) {
        diff = "hardware FPCR differs";
        st.fpcr_diff++;
      }
      const uint32_t sp = StackTop(c);
      auto scratch = [&](uint32_t a) { return a >= sp - 0x400 && a < sp + 0x80; };  // callee-owned stack scratch
      if (diff.empty()) {
        for (uint32_t k = 0; k < c.win_len; ++k)
          if (A.base[c.win_addr + k] != B.base[c.win_addr + k] && !scratch(c.win_addr + k)) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "memory %#x: native=%02x original=%02x", c.win_addr + k, B.base[c.win_addr + k],
                          A.base[c.win_addr + k]);
            diff = buf;
            break;
          }
      }
      if (diff.empty()) {  // declared write set must cover every changed byte
        if (w.overflow) diff = "Writes() overflow in the fuzzer";
        for (uint32_t k = 0; k < c.win_len && diff.empty(); ++k) {
          const uint32_t a = c.win_addr + k;
          if ((A.base[a] != init[k] || B.base[a] != init[k]) && !Covered(w, a)) {
            if (scratch(a)) continue;  // the callee-owned stack scratch (below r1, caller's parameter save area)
            char buf[160];
            std::snprintf(buf, sizeof buf, "byte %#x changed outside Writes() (orig %02x native %02x init %02x)", a,
                          A.base[a], B.base[a], init[k]);
            diff = buf;
          }
        }
      }
      // Guard replay: the console guard (me_hot_guest.cpp Check) on arena B from the same inputs, with the same shared
      // code (me_hot_shadow.h): the shadow build of the native on a private copy of the declared ranges (it must not
      // write arena B outside the stack scratch), the original for real, then the guard's comparison and the registers.
      // Catches Writes() ranges the guard cannot verify (2026-10-09: overlapping ranges gave a false DIFFERENCE) and
      // natives whose shadow build differs from the normal one.
      if (diff.empty() && !w.overflow) {
        std::memcpy(B.base + c.win_addr, init.data(), c.win_len);
        const ShadowFn sh = ShadowFor(c.name);
        // Slack pages: the console reads them through the shadow alias of committed pages (all of the arena here);
        // ME_GUARD_PAGES=declared models a console page that is not committed (slack only inside the declared pages).
        static const me::hot::ShadowPageFn page_fn =
            (getenv("ME_GUARD_PAGES") && std::string(getenv("ME_GUARD_PAGES")) == "declared") ? nullptr
                                                                                          : &me::hot::ShadowPageDirect;
        me::hot::ShadowView view;
        std::vector<uint8_t> storage;
        me::hot::Writes gw;  // the console guard's write set
        if (c.guard_writes) c.guard_writes(*c0, B.base, gw);
        else gw = w;
        if (!sh) {
          diff = "guard replay: no shadow build of the native (shadow_all.cpp)";
        } else if (!gw.overflow && me::hot::ShadowBuild(view, gw, B.base, c0->r1.u32, storage, page_fn)) {
          PPCContext* g = new PPCContext(*c0);
          g->fpscr.setcsr(hw0);
          me::hot::t_shadow = &view;
          const bool handled = sh(*g, B.base);
          me::hot::t_shadow = nullptr;
          const uint32_t hw_s = g->fpscr.getcsr();
          for (uint32_t k = 0; k < c.win_len && diff.empty(); ++k) {
            const uint32_t a = c.win_addr + k;
            // Cells only the case's widened write set declares are written by its host stubs for guest callees (guest
            // code a native calls runs on real memory), not by the native itself: put back for the original's run (on the
            // console such callees are read-only getters, docs/hot-guard.md).
            if (Covered(w, a) && !Covered(gw, a)) {
              B.base[a] = init[k];
              continue;
            }
            if (B.base[a] != init[k] && !scratch(a)) {
              char buf[160];
              std::snprintf(buf, sizeof buf, "guard replay: the shadow run wrote guest memory at %#x (%02x -> %02x)", a,
                            init[k], B.base[a]);
              diff = buf;
            }
          }
          PPCContext* o = new PPCContext(*c0);
          o->fpscr.setcsr(hw0);
          c.orig(*o, B.base);
          const uint32_t hw_g = o->fpscr.getcsr();
          o->fpscr.setcsr(hw0);
          if (handled && diff.empty()) {
            Stats dummy;
            diff = CompareRegs(c.cmp, *g, *o, dummy);
            if (!diff.empty()) diff = "guard replay registers: " + diff;
            else if (hw_s != hw_g) diff = "guard replay: hardware FPCR differs";
          }
          if (handled && diff.empty()) {
            // The guard's memory comparison, except two tolerances of the main comparison above: the callee-owned stack
            // scratch / bytes outside the window, and a 4-byte word that is a NaN in both results (the cases' NaN payload
            // tolerance, see case_826545D0.inc: the payload depends on the compiled operand order).
            char buf[200];
            auto is_nan_be = [](const uint8_t* p) {
              const uint32_t x = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
              return (x & 0x7F800000u) == 0x7F800000u && (x & 0x007FFFFFu) != 0;
            };
            for (int i = 0; i < gw.n && diff.empty(); ++i) {
              const me::hot::ShadowView::Win* x = me::hot::ShadowDeclared(view, gw.r[i].addr, gw.r[i].len);
              for (uint32_t k = 0; x && k < gw.r[i].len; ++k) {
                const uint32_t a = gw.r[i].addr + k;
                const uint8_t mine = x->buf[a - x->lo], now = *me::hot::Raw(B.base, a);
                if (mine == now || scratch(a) || a - c.win_addr >= c.win_len) continue;
                const uint32_t wa = a & ~3u;
                if (wa >= x->lo && wa + 4 <= x->lo + x->len && is_nan_be(x->buf + (wa - x->lo)) &&
                    is_nan_be(me::hot::Raw(B.base, wa)))
                  continue;
                std::snprintf(buf, sizeof buf, "guard replay: memory %#x (+%u of range %#x+%u) native=%02x original=%02x",
                              a, k, gw.r[i].addr, gw.r[i].len, mine, now);
                diff = buf;
                break;
              }
            }
          }
          if (handled && diff.empty()) {
            const me::hot::ShadowDiff d = me::hot::ShadowCompareStores(view);
            char buf[200];
            if (d.kind == me::hot::ShadowDiff::kStore) {
              std::snprintf(buf, sizeof buf, "guard replay: native stored to %#x outside its declared ranges", d.addr);
              diff = buf;
            } else if (d.kind == me::hot::ShadowDiff::kSlack) {
              std::snprintf(buf, sizeof buf, "guard replay: native wrote %#x next to its declared ranges", d.addr);
              diff = buf;
            }
          }
          delete g;
          delete o;
        }
      }
      delete c0;
      st.iters++;
      (co->r3.u64 == 0 ? st.r3zero : co->r3.u64 == 1 ? st.r3one : st.r3other)++;
      if (!diff.empty()) {
        st.fails++;
        if (shown++ < 5)
          std::printf("  FAIL %s iter %" PRIu64 ": %s\n    inputs r3=%#" PRIx64 " r4=%#" PRIx64 " r5=%#" PRIx64 " r6=%#" PRIx64 " r7=%#" PRIx64 " r8=%#" PRIx64 "\n",
                      c.name, it, diff.c_str(), in_r[0], in_r[1], in_r[2], in_r[3], in_r[4], in_r[5]);
      }
      delete co;
      delete cn;
    }
    std::printf("%-22s %s: %" PRIu64 " iterations, %" PRIu64 " failures", c.name, st.fails ? "FAIL" : "ok", st.iters, st.fails);
    // scratch information
    std::string info;
    const char* kind = "rfv";
    for (int k = 0; k < 3; ++k)
      for (int i = 0; i < 32; ++i)
        if (st.scratch[k][i]) info += " " + std::string(1, kind[k]) + std::to_string(i);
    if (!info.empty()) std::printf("  [scratch differs:%s]", info.c_str());
    std::printf("  [original r3: 0=%" PRIu64 " 1=%" PRIu64 " other=%" PRIu64 "]", st.r3zero, st.r3one, st.r3other);
    std::printf("\n");
    total_fail += st.fails ? 1 : 0;
    if (bench) {
      // Indicative only (clang -O2 on the host, not GCC -O3 on the Cortex-A57): ns per call of the original and of the
      // native version on 64 random inputs, 300 repeated calls each; only the argument registers are restored between
      // calls (state-changing functions repeat on the changed state).
      using clk = std::chrono::steady_clock;
      double t_o = 0, t_n = 0;
      const int kInputs = 64, kReps = 300;
      auto restore = [](PPCContext& d, const PPCContext& s) {
        d.r1 = s.r1; d.r3 = s.r3; d.r4 = s.r4; d.r5 = s.r5; d.r6 = s.r6; d.r7 = s.r7; d.r8 = s.r8; d.r9 = s.r9; d.r10 = s.r10;
        d.f1 = s.f1; d.f2 = s.f2; d.f3 = s.f3; d.f4 = s.f4; d.f5 = s.f5; d.f6 = s.f6; d.f7 = s.f7; d.f8 = s.f8;
        d.v1 = s.v1; d.v2 = s.v2; d.v3 = s.v3; d.v4 = s.v4;
      };
      for (int k = 0; k < kInputs; ++k) {
        PPCContext* co = new PPCContext();
        for (int i = 0; i < 32; ++i) const_cast<PPCRegister*>(Gpr(*co, i))->u64 = rng.u64();
        for (int i = 0; i < 32; ++i) const_cast<PPCRegister*>(Fpr(*co, i))->u64 = rng.u64();
        for (uint32_t m = 0; m < c.win_len; m += 8) {
          uint64_t x = rng.u64();
          std::memcpy(A.base + c.win_addr + m, &x, 8);
        }
        c.gen(rng, A.base, *co);
        co->r1.u64 = StackTop(c);
        co->fpscr.csr = co->fpscr.getcsr();
        std::memcpy(B.base + c.win_addr, A.base + c.win_addr, c.win_len);
        PPCContext* tmp = new PPCContext(*co);
        auto t0 = clk::now();
        for (int r = 0; r < kReps; ++r) {
          restore(*tmp, *co);
          c.orig(*tmp, A.base);
        }
        auto t1 = clk::now();
        for (int r = 0; r < kReps; ++r) {
          restore(*tmp, *co);
          c.nat(*tmp, B.base);
        }
        auto t2 = clk::now();
        t_o += std::chrono::duration<double, std::nano>(t1 - t0).count();
        t_n += std::chrono::duration<double, std::nano>(t2 - t1).count();
        delete co;
        delete tmp;
      }
      const double no = t_o / (kInputs * kReps), nn = t_n / (kInputs * kReps);
      std::printf("   bench %s: original %.1f ns/call, native %.1f ns/call (x%.2f)\n", c.name, no, nn, no / nn);
    }
  }
  return total_fail ? 1 : 0;
}
