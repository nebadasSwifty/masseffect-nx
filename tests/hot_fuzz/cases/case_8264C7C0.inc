// ORIG: sub_8264C7C0
// NATIVE: n_8264C7C0.h
extern "C" void __imp__sub_8264C7C0(PPCContext&, uint8_t*);
namespace {
// Window 0x820B8000 .. 0x820C8000 contains the two float constants the function loads (0x820B90B8 and 0x820BCD64) and
//   +0x0100 object, +0x0200 pointer word, +0x0400 8-byte table entries, +0x0800 u16 index table, +0x1100.. particle records,
//   +0x5000 vertex buffer (352 B per particle), +0xB000 index buffer (12 B per particle).
// Aliasing modes move one of the buffers / the object / the table onto the data that is read (partial overlaps, so that
// the fast implementation must fall back to the ordered one at the right particle).
void gen_8264C7C0(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t W = 0x820B8000;
  auto put32 = [&](uint32_t a, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(base + a, &v, 4);
  };
  auto sane = [&](double scale) {
    float f = float(int32_t(r.u32() % 4001) - 2000) * float(scale);
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
  };
  // Every NaN is the canonical quiet NaN: with two different NaN operands the NaN the PPC op returns depends on the operand order
  // the C++ compiler chose for the commutative fmul/fadd (clang here, GCC on the Switch); with one NaN pattern it cannot matter.
  auto canon = [](uint32_t b) { return ((b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu)) ? 0x7FC00000u : b; };
  auto fval = [&]() { return r.chance(0.8) ? sane(0.125) : canon(r.float_bits()); };

  // the harness pre-fills the window with random bytes: canonicalise every NaN-looking word first (explicit fields are set below)
  for (uint32_t a = W; a < W + 0x10000; a += 4) {
    uint32_t v;
    std::memcpy(&v, base + a, 4);
    v = __builtin_bswap32(v);
    if ((v & 0x7F800000u) == 0x7F800000u && (v & 0x7FFFFFu)) put32(a, 0x7FC00000u);
  }
  const uint32_t n0 = r.chance(0.1) ? 0 : (r.chance(0.7) ? r.range(1, 6) : r.range(1, 40));
  uint32_t cnt = r.chance(0.5) ? n0 : (r.chance(0.5) ? n0 + r.below(10) : 0x7FFFFFFF);
  const int32_t lim = r.chance(0.5) ? -1 - int32_t(r.below(5)) : int32_t(r.chance(0.5) ? n0 : r.below(60));
  if (cnt == 0x7FFFFFFF && lim < 0) cnt = n0;  // keep the count bounded (the window is small)
  const uint32_t n = (lim >= 0 && int32_t(cnt) > lim) ? uint32_t(lim) : cnt;  // the count the function uses
  uint32_t obj = W + 0x100;
  uint32_t out = W + 0x5000 + (r.chance(0.5) ? 0 : r.below(64) * 4);
  uint32_t idx = r.chance(0.35) ? 0 : W + 0xB000 + (r.chance(0.5) ? 0 : r.below(64) * 2);
  uint32_t ptrword = W + 0x200, tabarr = W + 0x400, u16tab = W + 0x800 + (r.chance(0.9) ? 0 : 2 * r.below(8));
  uint32_t rec0 = W + 0x1100 + (r.chance(0.8) ? 0 : r.below(64) * 4);
  uint32_t stride = r.chance(0.7) ? 112 + 16 * r.below(3) : r.range(0, 200) * 4;
  uint32_t nrec = r.range(1, 40);
  const uint32_t off76 = r.chance(0.7) ? r.range(0, 24) * 4 + 0 : r.range(0, 200) * 4;

  // ---- records: structured data over the whole record area (plus an equal amount of random bytes below it)
  for (uint32_t a = W + 0x1100; a < W + 0x4C00; a += 4) put32(a, fval());
  const bool uvmode = r.chance(0.5);  // the UV record values look like texel coordinates (integers) most of the time
  if (uvmode)
    for (uint32_t a = W + 0x1100; a < W + 0x4C00; a += 4)
      if (r.chance(0.5)) {
        float f = float(int32_t(r.below(512)));
        uint32_t b;
        std::memcpy(&b, &f, 4);
        put32(a, b);
      }
  // ---- index table: u16 particle-record numbers
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t v = r.chance(0.9) ? r.below(nrec) : r.below(0x10000);
    uint16_t be = __builtin_bswap16(uint16_t(v));
    std::memcpy(base + u16tab + 2 * i, &be, 2);
  }
  // ---- 8-byte table entries {particle index, junk}
  for (uint32_t i = 0; i < 64; ++i) {
    put32(tabarr + 8 * i, r.chance(0.9) ? r.below(64) : canon(r.u32()));
    put32(tabarr + 8 * i + 4, canon(r.u32()));
  }
  put32(ptrword, tabarr);

  // ---- constants f9 / f10
  // (a signalling NaN in f9 / f10 is stored quieted by clang's build of the original but as a raw copy by GCC's: avoided,
  // real data is 0.0 / 1.0)
  auto quiet = [](uint32_t b) { return ((b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu)) ? (b | 0x400000u) : b; };
  auto cst = [&]() { return r.chance(0.8) ? (r.chance(0.5) ? 0x3F800000u : 0u) : (r.chance(0.5) ? canon(r.float_bits()) : sane(0.01)); };
  put32(0x820B90B8, cst());
  put32(0x820BCD64, r.chance(0.85) ? 0x3F800000u : cst());

  // ---- aliasing modes
  const int alias = r.chance(0.55) ? 0 : int(r.below(9));
  switch (alias) {
    case 1: out = rec0 + r.below(2000) * 4; break;                   // vertex buffer inside the records
    case 2: out = W + 0x0800 + r.below(100) * 4; break;              // ... over the index table / table entries / pointer word
    case 3: if (idx) idx = out + r.below(300 * n + 1); else out = obj - r.below(200 * n + 1); break;  // index buffer inside the vertex buffer / object
    case 4: out = obj - 4 * r.below(88 * n + 2); break;              // object inside the vertex buffer
    case 5: out = ptrword - 4 * r.below(88 * n + 2); break;          // pointer word / tables inside it
    case 6: if (idx) idx = rec0 + r.below(1500) * 2; break;          // index buffer inside the records
    case 7: out = rec0 + 4 * r.below(300) + off76; break;            // vertex buffer near the UV record
    case 8: out = tabarr + r.below(256) * 4; break;                  // vertex buffer over the 8-byte table entries
    default: break;
  }
  // keep every write inside the window and clear of the guest stack
  if (out < W + 0x10 || out + 352 * n + 16 > W + 0xF000) out = W + 0x5000;
  if (idx && (idx < W + 0x10 || idx + 12 * n + 16 > W + 0xF000)) idx = W + 0xB000;
  if (alias == 4 || alias == 5) {
    if (out + 352 * n + 16 > W + 0xF000 || out < W + 0x10) out = W + 0x5000;
  }

  // ---- the object
  put32(obj + 8, cnt);
  put32(obj + 68, uint32_t(lim));
  put32(obj + 16, stride);
  put32(obj + 20, rec0);
  put32(obj + 24, u16tab);
  put32(obj + 28, fval());
  put32(obj + 32, fval());
  put32(obj + 36, fval());
  base[obj + 52] = r.chance(0.5) ? 0 : uint8_t(1 + r.below(255));
  put32(obj + 76, off76);
  put32(obj + 80, r.chance(0.85) ? r.range(1, 4096) : (r.chance(0.5) ? 0 : r.u32()));
  put32(obj + 84, r.chance(0.85) ? r.range(1, 4096) : (r.chance(0.5) ? 0 : r.u32()));
  put32(obj + 88, r.chance(0.5) ? 0 : (r.chance(0.7) ? 1 : r.u32()));

  // records must not be read from the callee-owned stack scratch (the original spills there, the native does not)
  {
    const uint32_t sp = W + 0x10000 - 0x100, lo = sp - 0x400 - 0x80, hi = sp + 0x80;
    for (uint32_t i = 0; i < 64; ++i) {
      uint16_t be;
      std::memcpy(&be, base + u16tab + 2 * i, 2);
      const uint32_t t = __builtin_bswap16(be);
      const uint32_t p = uint32_t(int64_t(int32_t(t)) * int64_t(int32_t(stride))) + rec0;
      const uint64_t a0 = p, a1 = uint64_t(p) + 112, b0 = uint64_t(p) + off76, b1 = b0 + 24;
      if ((a0 < hi && a1 > lo) || (b0 < hi && b1 > lo)) {
        const uint16_t z = 0;
        std::memcpy(base + u16tab + 2 * i, &z, 2);
      }
    }
  }
  ctx.r3.u64 = (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0) | obj;
  ctx.r4.u64 = (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0) | out;
  ctx.r5.u64 = (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0) | idx;
  ctx.r6.u64 = r.chance(0.5) ? 0 : ((r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0) | ptrword);
}
fuzz::Registrar reg_8264C7C0({"sub_8264C7C0", __imp__sub_8264C7C0, me::hot::n_8264C7C0::Native, me::hot::n_8264C7C0::Writes,
                              me::hot::n_8264C7C0::kCmp, 0x820B8000, 0x10000, gen_8264C7C0});
}  // namespace
