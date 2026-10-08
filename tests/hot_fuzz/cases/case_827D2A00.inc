// ORIG: sub_827D2A00
// NATIVE: n_827D2A00.h
// LZO1X decompressor. The generator writes random LZO1X streams that follow the grammar of the original (literal runs,
// M1/M2/M3/M4 matches with extended lengths, trailing literals, the variant's end marker token 17), including the
// corner cases of this variant: M4 tokens 16/18-23 and the 0x8000 group with distance 0 (copies, not end markers),
// matches that reach in front of the output buffer (into the random window prefix), short-period overlapping copies,
// streams with bytes after the end marker (-8) and in_len shorter than the stream (-4).
// Window: [win, win + 0x10000) random prefix, output at win + 0x10000 (up to 192 KB), input at win + 0x40000.
extern "C" void __imp__sub_827D2A00(PPCContext&, uint8_t*);
namespace {

struct LzoGen {
  fuzz::Rng& r;
  std::vector<uint8_t> s;  // the stream
  uint32_t produced = 0;   // output bytes so far

  void lit_bytes(uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) s.push_back(uint8_t(r.u32()));
    produced += n;
  }
  // extended length field: value v >= 1 encoded as zeros (255 each) and a final non-zero byte
  void ext(uint32_t v) {
    while (v > 255) {
      s.push_back(0);
      v -= 255;
    }
    s.push_back(uint8_t(v));
  }
  uint32_t pick_len(uint32_t lo, uint32_t hi_common) {
    switch (r.below(8)) {
      case 0: return lo + r.below(300);
      case 1: return lo + r.below(1200);
      default: return lo + r.below(hi_common - lo + 1);
    }
  }
  // total distance (op - src) for a match; the window prefix gives 64 KB in front of the output
  uint32_t pick_dist(uint32_t lo, uint32_t hi) {
    uint32_t d;
    switch (r.below(6)) {
      case 0: d = lo + r.below(8); break;  // short periods (overlap)
      case 1: d = hi - r.below(8); break;
      default: d = lo + r.below(hi - lo + 1); break;
    }
    return d < lo ? lo : (d > hi ? hi : d);
  }
};

void gen_827D2A00(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x40000000;
  const uint32_t out = win + 0x10000 + r.below(64);
  const uint32_t in = win + 0x40000 + r.below(64);
  LzoGen g{r};
  // states: 0x100 top, 0x200 after a literal run >= 4, 0 after 1-3 trailing literals
  uint32_t state = 0x100;
  if (r.chance(0.5)) {
    const uint32_t n = r.chance(0.3) ? 1 + r.below(3) : 4 + r.below(200);
    g.s.push_back(uint8_t(17 + n));
    g.lit_bytes(n);
    state = n < 4 ? 0 : 0x200;
  }
  const uint32_t tokens = r.chance(0.1) ? r.below(4) : r.chance(0.5) ? 1 + r.below(60) : 1 + r.below(1500);
  for (uint32_t k = 0; k < tokens && g.produced < 150000; ++k) {
    uint32_t trailing = r.chance(0.5) ? 0 : 1 + r.below(3);
    // a first byte > 17 is always read as a literal header: without one the stream starts with a literal run
    const uint32_t kind = (k == 0 && g.s.empty()) ? 0 : r.below(state == 0x100 ? 5 : 6);
    if (state == 0x100 && kind == 0) {  // literal run
      const uint32_t n = r.chance(0.7) ? 4 + r.below(15) : g.pick_len(4, 300);
      if (n - 3 <= 15) {
        g.s.push_back(uint8_t(n - 3));
      } else {
        g.s.push_back(0);  // extended: 18 + ext (n >= 19 here)
        g.ext(n - 18);
      }
      g.lit_bytes(n);
      state = 0x200;
      continue;
    }
    if (state != 0x100 && kind == 5) {  // short match after literals
      const uint32_t t = r.below(4) << 2 | trailing;
      const uint32_t b = r.below(256);
      g.s.push_back(uint8_t(t));
      g.s.push_back(uint8_t(b));
      g.produced += state == 0x200 ? 3 : 2;
    } else if (kind == 1) {  // M2: len 3..8, dist 1..2048
      const uint32_t n = 3 + r.below(6);
      const uint32_t d = g.pick_dist(1, 2048) - 1;
      g.s.push_back(uint8_t(((n - 1) << 5) | ((d & 7) << 2) | trailing));
      g.s.push_back(uint8_t(d >> 3));
      g.produced += n;
    } else if (kind == 2) {  // M3: dist 1..16384
      const uint32_t n = r.chance(0.6) ? 3 + r.below(31) : g.pick_len(34, 600);
      const uint32_t d = g.pick_dist(1, 16384) - 1;
      if (n - 2 <= 31) {
        g.s.push_back(uint8_t(32 | (n - 2)));
      } else {
        g.s.push_back(32);
        g.ext(n - 33);
      }
      const uint32_t le = (d << 2) | trailing;
      g.s.push_back(uint8_t(le));
      g.s.push_back(uint8_t(le >> 8));
      g.produced += n;
    } else {  // M4: total distance 16384..49151 (bit 8 selects the 0x8000 group)
      const uint32_t n = r.chance(0.6) ? 3 + r.below(7) : g.pick_len(10, 600);
      const bool hi = r.chance(0.4);
      uint32_t d = r.chance(0.1) ? 0 : g.pick_dist(0, 16383);
      uint32_t t = 16 | (hi ? 8 : 0);
      if (n - 2 <= 7) t |= n - 2;
      if (t == 17 && d == 0) d = 1 + r.below(100);  // would be the end marker
      g.s.push_back(uint8_t(t));
      if (n - 2 > 7) g.ext(n - 9);
      const uint32_t le = (d << 2) | trailing;
      g.s.push_back(uint8_t(le));
      g.s.push_back(uint8_t(le >> 8));
      g.produced += n;
    }
    if (trailing) {
      g.lit_bytes(trailing);
      state = 0;
    } else {
      state = 0x100;
    }
  }
  // end marker: token 17 with distance 0 (low 2 bits of the distance word = 0)
  g.s.push_back(17);
  g.s.push_back(0);
  g.s.push_back(0);
  uint32_t in_len = uint32_t(g.s.size());
  if (r.chance(0.1)) in_len += 1 + r.below(20);           // input not consumed: -8
  else if (r.chance(0.1)) in_len -= 1 + r.below(in_len > 3 ? 3 : in_len);  // read past the end: -4
  for (size_t i = 0; i < g.s.size(); ++i) me::hot::St8(base, in + uint32_t(i), g.s[i]);
  const uint32_t out_len_p = r.chance(0.5) ? win + 0x3F000 + r.below(256) * 4 : win + 0x7E000 + r.below(64) * 4;
  ctx.r3.u64 = in | (r.chance(0.3) ? (r.u64() << 32) : 0);
  ctx.r4.u64 = in_len | (r.chance(0.3) ? (r.u64() << 32) : 0);  // only the low half of in + in_len is compared
  ctx.r5.u64 = out | (r.chance(0.3) ? (r.u64() << 32) : 0);
  ctx.r6.u64 = out_len_p | (r.chance(0.3) ? (r.u64() << 32) : 0);
}
fuzz::Registrar reg_827D2A00({"sub_827D2A00", __imp__sub_827D2A00, me::hot::n_827D2A00::Native,
                              me::hot::n_827D2A00::Writes, me::hot::n_827D2A00::kCmp, 0x40000000, 0x80000,
                              gen_827D2A00});
}  // namespace
