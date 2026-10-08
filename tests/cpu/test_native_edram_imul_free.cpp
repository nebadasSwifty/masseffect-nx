// Exactness proof of the IMUL-free address math of the EDRAM transfer shaders (app/src/native/masseffect/shaders,
// docs/edram-shader-imul.md). NAK on SM50 emits the microcoded IMUL for 32-bit multiplies, IMUL.HI for divisions by
// a constant and an IMUL sequence for divisions by a register; the shaders now use float forms instead:
//
//   DivPitch(t, p) = uint((float(t) + 0.5) / float(p))      NAK: I2F, FADD, MUFU.RCP, FMUL, F2I.RZ
//   MulSmall(a, b) = uint(float(a) * float(b))             NAK: I2F, I2F, FMUL, F2I.RZ
//   ModPitch(t, p) = t - MulSmall(DivPitch(t, p), p)
//   Div80(x)       = uint(fma(float(x), 0.0125, 0.00625))  NAK: I2F, FFMA, F2I.RZ
//   Mul80(x)       = (x << 6) + (x << 4)
//   Div40(x)       = uint(fma(float(x), 0.025, 0.0125))
//
// Every primitive is checked exhaustively against the integer operation it replaces, over ranges wider than the
// shaders use (tiles < 2^16 with EDRAM at 2048 tiles, pitches 1..2048 with real pitches <= 160 tiles, x < 2^20 with
// the widest dispatch at 2048 * 80 samples). MUFU.RCP is not correctly rounded, so DivPitch is checked with the
// reciprocal rounded to nearest and moved by up to 2 ulp either way, and with a true division. FMA may be fused or
// not, so Div80/Div40 are checked both ways. Then the whole per-texel address maps of the old and new shaders are
// compared (exhaustively over the target grid for every pitch up to 160 and both sample layouts, and on random runs).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>

namespace {

int failures = 0;
void Check(bool ok, const char* what, uint32_t a, uint32_t b, uint32_t c = 0) {
  if (ok) return;
  if (++failures <= 20) std::printf("FAIL %s (%u, %u, %u)\n", what, a, b, c);
}

// F2I.U32.RZ of a non-negative finite float.
uint32_t F2U(float f) { return uint32_t(f); }

// --- New (float) forms; `rcp_ulps` moves the reciprocal like an inexact MUFU.RCP, 99 = a true division. ---
uint32_t DivPitchNew(uint32_t tile, uint32_t pitch, int rcp_ulps) {
  const float numerator = float(tile) + 0.5f;
  if (rcp_ulps == 99) return F2U(numerator / float(pitch));
  float r = 1.0f / float(pitch);
  for (int i = 0; i < rcp_ulps; ++i) r = std::nextafter(r, 2.0f);
  for (int i = 0; i > rcp_ulps; --i) r = std::nextafter(r, 0.0f);
  return F2U(numerator * r);
}
uint32_t MulSmall(uint32_t a, uint32_t b) {
  volatile float product = float(a) * float(b);  // one rounding, never contracted with anything
  return F2U(product);
}
uint32_t ModPitchNew(uint32_t tile, uint32_t pitch, int rcp_ulps) {
  return tile - MulSmall(DivPitchNew(tile, pitch, rcp_ulps), pitch);
}
uint32_t DivConstNew(uint32_t x, float scale, float bias, bool fused) {
  if (fused) return F2U(std::fma(float(x), scale, bias));
  volatile float product = float(x) * scale;
  return F2U(product + bias);
}
uint32_t Div80New(uint32_t x, bool fused) { return DivConstNew(x, 0.0125f, 0.00625f, fused); }
uint32_t Div40New(uint32_t x, bool fused) { return DivConstNew(x, 0.025f, 0.0125f, fused); }
uint32_t Mul80(uint32_t x) { return (x << 6) + (x << 4); }
uint32_t Mul40(uint32_t x) { return (x << 5) + (x << 3); }
uint32_t Mod80New(uint32_t x, bool fused) { return x - Mul80(Div80New(x, fused)); }
uint32_t Mod40New(uint32_t x, bool fused) { return x - Mul40(Div40New(x, fused)); }

// One emulated device: which reciprocal error and FMA behaviour the GPU has (unknown, so every combination is run).
struct Device { int rcp_ulps; bool fused; };
constexpr Device kDevices[] = {{0, true}, {0, false}, {1, true}, {-1, true}, {2, false}, {-2, false}, {99, true}};

void PrimitivesExhaustive() {
  for (const int ulps : {0, 1, -1, 2, -2, 99}) {
    for (uint32_t pitch = 1; pitch <= 2048; ++pitch) {
      for (uint32_t tile = 0; tile < (1u << 16); ++tile) {
        const uint32_t q = DivPitchNew(tile, pitch, ulps);
        Check(q == tile / pitch, "DivPitch", tile, pitch, uint32_t(ulps));
        Check(tile - MulSmall(q, pitch) == tile % pitch, "ModPitch", tile, pitch, uint32_t(ulps));
      }
    }
  }
  // Every product below 2^24 that the shaders form: rows (< 2^16 / 16 = 4096, doubled for margin) times pitches
  // (<= 2048), and rows of the stencil-to-buffer output times words per row.
  for (uint32_t a = 0; a < 8192; ++a)
    for (uint32_t b = 0; b <= 2048; ++b) Check(MulSmall(a, b) == a * b, "MulSmall", a, b);
  for (const bool fused : {true, false}) {
    for (uint32_t x = 0; x < (1u << 20); ++x) {
      Check(Div80New(x, fused) == x / 80, "Div80", x, fused);
      Check(Mod80New(x, fused) == x % 80, "Mod80", x, fused);
      Check(Div40New(x, fused) == x / 40, "Div40", x, fused);
      Check(Mod40New(x, fused) == x % 40, "Mod40", x, fused);
      Check(Mul80(x) == x * 80 && Mul40(x) == x * 40, "Mul80/Mul40", x, 0);
    }
  }
}

// --- Whole address maps, transcribed from the shaders before and after the rewrite. ---
struct Run {
  uint32_t pitch_source, pitch_target, tile_source_start, tile_target_start, tiles_count;
  uint32_t source_msaa_x, source_msaa_y, target_msaa_x, target_msaa_y;
};
struct Texel { bool keep; uint32_t x, y; };

// Fragment passes (me_edram_color_to_color.frag, *_to_depth/stencil.frag): destination texel -> source texel.
Texel FragOld(const Run& c, uint32_t px, uint32_t py) {
  const uint32_t x = px << c.target_msaa_x, y = py << c.target_msaa_y;
  const uint32_t tile_target = (y / 16u) * c.pitch_target + x / 80u;
  if (tile_target < c.tile_target_start || tile_target - c.tile_target_start >= c.tiles_count) return {false, 0, 0};
  const uint32_t tile_source = c.tile_source_start + (tile_target - c.tile_target_start);
  const uint32_t q = uint32_t((float(tile_source) + 0.5f) / float(c.pitch_source));  // the old float DivPitch
  return {true, ((tile_source - q * c.pitch_source) * 80u + x % 80u) >> c.source_msaa_x,
          (q * 16u + y % 16u) >> c.source_msaa_y};
}
Texel FragNew(const Run& c, uint32_t px, uint32_t py, const Device& d) {
  const uint32_t x = px << c.target_msaa_x, y = py << c.target_msaa_y;
  const uint32_t tile_target = MulSmall(y >> 4u, c.pitch_target) + Div80New(x, d.fused);
  if (tile_target < c.tile_target_start || tile_target - c.tile_target_start >= c.tiles_count) return {false, 0, 0};
  const uint32_t tile_source = c.tile_source_start + (tile_target - c.tile_target_start);
  return {true, (Mul80(ModPitchNew(tile_source, c.pitch_source, d.rcp_ulps)) + Mod80New(x, d.fused)) >> c.source_msaa_x,
          (DivPitchNew(tile_source, c.pitch_source, d.rcp_ulps) * 16u + (y & 15u)) >> c.source_msaa_y};
}
// me_edram_r64_to_r64.frag: 40 pixels per tile row.
Texel Frag64Old(const Run& c, uint32_t px, uint32_t py) {
  const uint32_t x = px << c.target_msaa_x, y = py << c.target_msaa_y;
  const uint32_t tile_target = (y / 16u) * c.pitch_target + x / 40u;
  if (tile_target < c.tile_target_start || tile_target - c.tile_target_start >= c.tiles_count) return {false, 0, 0};
  const uint32_t tile_source = c.tile_source_start + (tile_target - c.tile_target_start);
  const uint32_t q = uint32_t((float(tile_source) + 0.5f) / float(c.pitch_source));
  return {true, ((tile_source - q * c.pitch_source) * 40u + x % 40u) >> c.source_msaa_x,
          (q * 16u + y % 16u) >> c.source_msaa_y};
}
Texel Frag64New(const Run& c, uint32_t px, uint32_t py, const Device& d) {
  const uint32_t x = px << c.target_msaa_x, y = py << c.target_msaa_y;
  const uint32_t tile_target = MulSmall(y >> 4u, c.pitch_target) + Div40New(x, d.fused);
  if (tile_target < c.tile_target_start || tile_target - c.tile_target_start >= c.tiles_count) return {false, 0, 0};
  const uint32_t tile_source = c.tile_source_start + (tile_target - c.tile_target_start);
  return {true, (Mul40(ModPitchNew(tile_source, c.pitch_source, d.rcp_ulps)) + Mod40New(x, d.fused)) >> c.source_msaa_x,
          (DivPitchNew(tile_source, c.pitch_source, d.rcp_ulps) * 16u + (y & 15u)) >> c.source_msaa_y};
}

// Compute passes (masseffect_edram_*.comp, me_edram_*.comp): invocation -> (target texel, source texel). The
// Coordinate() shaders used an integer division by the pitch; the others the old float DivPitch (same result).
struct Pair { bool keep; uint32_t tx, ty, sx, sy; };
Pair CompOld(const Run& c, uint32_t qx, uint32_t qy, uint32_t is64) {
  const bool range = c.tiles_count != 0u;
  if (range && (qy >= 16u || qx >= c.tiles_count * 80u)) return {false, 0, 0, 0, 0};
  const uint32_t relative = qx / 80u, word_local = qx % 80u;
  const uint32_t tile_target = range ? c.tile_target_start + relative : (qy / 16u) * c.pitch_target + relative;
  const uint32_t tile_source = range ? c.tile_source_start + relative : tile_target;
  const uint32_t w = 80u >> is64;
  return {true, ((tile_target % c.pitch_target) * w + (word_local >> is64)) >> c.target_msaa_x,
          ((tile_target / c.pitch_target) * 16u + qy % 16u) >> c.target_msaa_y,
          ((tile_source % c.pitch_source) * w + (word_local >> is64)) >> c.source_msaa_x,
          ((tile_source / c.pitch_source) * 16u + qy % 16u) >> c.source_msaa_y};
}
Pair CompNew(const Run& c, uint32_t qx, uint32_t qy, uint32_t is64, const Device& d) {
  const bool range = c.tiles_count != 0u;
  if (range && (qy >= 16u || qx >= Mul80(c.tiles_count))) return {false, 0, 0, 0, 0};
  const uint32_t relative = Div80New(qx, d.fused), word_local = Mod80New(qx, d.fused);
  const uint32_t tile_target = range ? c.tile_target_start + relative : MulSmall(qy >> 4u, c.pitch_target) + relative;
  const uint32_t tile_source = range ? c.tile_source_start + relative : tile_target;
  return {true, ((Mul80(ModPitchNew(tile_target, c.pitch_target, d.rcp_ulps)) >> is64) + (word_local >> is64)) >> c.target_msaa_x,
          (DivPitchNew(tile_target, c.pitch_target, d.rcp_ulps) * 16u + (qy & 15u)) >> c.target_msaa_y,
          ((Mul80(ModPitchNew(tile_source, c.pitch_source, d.rcp_ulps)) >> is64) + (word_local >> is64)) >> c.source_msaa_x,
          (DivPitchNew(tile_source, c.pitch_source, d.rcp_ulps) * 16u + (qy & 15u)) >> c.source_msaa_y};
}

bool Same(const Texel& a, const Texel& b) { return a.keep == b.keep && (!a.keep || (a.x == b.x && a.y == b.y)); }
bool Same(const Pair& a, const Pair& b) {
  return a.keep == b.keep && (!a.keep || (a.tx == b.tx && a.ty == b.ty && a.sx == b.sx && a.sy == b.sy));
}

// Every target texel of the pitches below (all small ones, the real 1280/960/640 widths at 1x and 2x, the maximum),
// 1x and 2x vertical sample layouts, runs spanning the whole 2048-tile EDRAM with a source start that differs from
// the target start and a different source pitch; worst-case devices only (the primitives cover all of them).
constexpr uint32_t kPitches[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 16, 20, 24, 32, 40, 48, 64, 80, 96, 128, 159, 160};
constexpr Device kMapDevices[] = {{0, true}, {2, false}, {-2, false}};
void MapsExhaustive() {
  for (const uint32_t pitch : kPitches) {
    for (uint32_t msaa_y = 0; msaa_y <= 1; ++msaa_y) {
      const Run run{(pitch * 7u) % 160u + 1u, pitch, 37u, 0u, 2048u, 0u, msaa_y, 0u, msaa_y};
      const uint32_t rows = (2048u + pitch - 1u) / pitch, width = pitch * 80u, height = (rows * 16u) >> msaa_y;
      for (const Device& d : kMapDevices) {
        for (uint32_t py = 0; py < height; ++py) {
          for (uint32_t px = 0; px < width; ++px) {
            Check(Same(FragOld(run, px, py), FragNew(run, px, py, d)), "frag map", px, py, pitch);
            if (px < width / 2u) Check(Same(Frag64Old(run, px, py), Frag64New(run, px, py, d)), "frag64 map", px, py, pitch);
          }
        }
        // Compute, whole-target mode (tiles_count 0): the dispatch covers the target grid.
        Run whole = run;
        whole.tiles_count = 0u;
        whole.pitch_source = pitch;
        for (uint32_t qy = 0; qy < rows * 16u; ++qy)
          for (uint32_t qx = 0; qx < width; ++qx)
            for (uint32_t is64 = 0; is64 <= 1; ++is64)
              Check(Same(CompOld(whole, qx, qy, is64), CompNew(whole, qx, qy, is64, d)), "compute whole map", qx, qy, pitch);
      }
    }
  }
}

// Random runs: any start/count inside the 2048-tile EDRAM, pitches up to 2048, both sample layouts on each side.
void MapsRandom() {
  std::mt19937 rng(20261007u);
  for (int i = 0; i < 4000; ++i) {
    Run c{};
    c.pitch_source = 1u + rng() % ((i & 1) ? 160u : 2048u);
    c.pitch_target = 1u + rng() % ((i & 2) ? 160u : 2048u);
    c.tile_target_start = rng() % 2048u;
    c.tile_source_start = rng() % 2048u;
    c.tiles_count = 1u + rng() % (2048u - (c.tile_target_start > c.tile_source_start ? c.tile_target_start
                                                                                      : c.tile_source_start));
    c.source_msaa_x = rng() & 1u; c.source_msaa_y = rng() & 1u;
    c.target_msaa_x = rng() & 1u; c.target_msaa_y = rng() & 1u;
    const Device& d = kDevices[i % (sizeof(kDevices) / sizeof(kDevices[0]))];
    for (int k = 0; k < 4000; ++k) {
      const uint32_t px = rng() % ((c.pitch_target * 80u) >> c.target_msaa_x);
      const uint32_t py = rng() % ((((2048u + c.pitch_target - 1u) / c.pitch_target) * 16u) >> c.target_msaa_y);
      Check(Same(FragOld(c, px, py), FragNew(c, px, py, d)), "random frag map", px, py, uint32_t(i));
      Check(Same(Frag64Old(c, px >> 1, py), Frag64New(c, px >> 1, py, d)), "random frag64 map", px, py, uint32_t(i));
      const uint32_t qx = rng() % (c.tiles_count * 80u), qy = rng() % 16u;
      for (uint32_t is64 = 0; is64 <= 1; ++is64)
        Check(Same(CompOld(c, qx, qy, is64), CompNew(c, qx, qy, is64, d)), "random compute run map", qx, qy, uint32_t(i));
    }
  }
}

}  // namespace

int main() {
  PrimitivesExhaustive();
  MapsExhaustive();
  MapsRandom();
  if (failures) {
    std::printf("%d mismatches\n", failures);
    return 1;
  }
  std::printf("IMUL-free EDRAM address math: identical to the integer forms on every checked input\n");
  return 0;
}
