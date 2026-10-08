// Restore into 7e3 (me_restore_7e3_spirv.h, masseffect_native_restore_into_7e3): the fragment epilogue that
// lets the UNORM10 scene restore render straight into the 7e3 host image must give, bit for bit, what the f2
// attachment store plus the f2 -> f3 conversion give today. CPU models of both chains, plus the SPIR-V
// transform on a synthetic DXC-shaped module. No game data.
#include "me_restore_7e3_spirv.h"
#include <cstdio>
#include <stdexcept>

using namespace me::native;
using Words = std::vector<uint32_t>;

static void Check(bool b, const char* s) {
  if (!b) throw std::runtime_error(s);
}

static bool SameBits(const float a[4], const float b[4]) {
  for (int c = 0; c < 4; ++c)
    if (BitsOfFloat(a[c]) != BitsOfFloat(b[c])) return false;
  return true;
}

// The f3 image stores the float32 output as half; the word a later consumer re-packs from it.
static uint32_t WordFromF3(const float v[4]) {
  float h[4];
  for (int c = 0; c < 4; ++c) h[c] = F16ToF32(F32ToF16Rtne(v[c]));
  return Pack32Model(h, 3u);
}
// The word a consumer re-packs from today's f2 image (which holds f16(x)).
static uint32_t WordFromF2(const float x[4]) {
  float h[4];
  for (int c = 0; c < 4; ++c) h[c] = F16ToF32(F32ToF16Rtne(x[c]));
  return Pack32Model(h, 2u);
}

static void Emit(Words& w, uint32_t op, std::initializer_list<uint32_t> a) {
  w.push_back((uint32_t(a.size() + 1) << 16) | op);
  w.insert(w.end(), a);
}
static void EmitString(Words& w, uint32_t op, uint32_t id, const char* s) {
  Words text;
  uint32_t word = 0, n = 0;
  for (const char* p = s;; ++p) {
    word |= uint32_t(uint8_t(*p)) << (8 * (n & 3));
    if ((++n & 3) == 0) { text.push_back(word); word = 0; }
    if (!*p) break;
  }
  if (n & 3) text.push_back(word);
  w.push_back((uint32_t(text.size() + 2) << 16) | op);
  w.push_back(id);
  w.insert(w.end(), text.begin(), text.end());
}

// DXC-shaped fragment module: out.var.SV_Target0 (Location 0, vec4) stored once before OpReturn, optionally a
// second output at Location 1 and optionally an OpAccessChain into output 0 (unsupported shape).
static Words Fixture(bool chain, bool second_output) {
  Words w{0x07230203, 0x00010000, 0, 100, 0};
  Emit(w, 17, {1});                                    // OpCapability Shader
  EmitString(w, 11, 1, "GLSL.std.450");                // %1 = OpExtInstImport
  Emit(w, 14, {0, 1});                                 // OpMemoryModel Logical GLSL450
  Emit(w, 15, {4, 30, 0x6E69616D, 0, 20, 21});         // OpEntryPoint Fragment %30 "main" %20 %21
  Emit(w, 16, {30, 7});                                // OriginUpperLeft
  Emit(w, 71, {20, 30, 0});                            // %20 Location 0
  Emit(w, 71, {21, 30, 1});                            // %21 Location 1
  Emit(w, 19, {2});                                    // void
  Emit(w, 33, {3, 2});                                 // void()
  Emit(w, 22, {6, 32});                                // float
  Emit(w, 23, {7, 6, 4});                              // float4
  Emit(w, 32, {8, 3, 7});                              // Output float4*
  Emit(w, 32, {9, 3, 6});                              // Output float*
  Emit(w, 21, {10, 32, 1});                            // int (signed; the transform adds its own uint)
  Emit(w, 43, {6, 11, 0x3F000000});                    // 0.5
  Emit(w, 43, {10, 12, 3});                            // int 3
  Emit(w, 59, {8, 20, 3});                             // %20 Output
  Emit(w, 59, {8, 21, 3});                             // %21 Output
  Emit(w, 54, {2, 30, 0, 3});                          // OpFunction
  Emit(w, 248, {31});                                  // OpLabel
  Emit(w, 80, {7, 40, 11, 11, 11, 11});                // %40 = (0.5, 0.5, 0.5, 0.5)
  if (chain) {
    Emit(w, 65, {9, 41, 20, 12});                      // %41 = &%20[3]
    Emit(w, 62, {41, 11});
  }
  Emit(w, 62, {20, 40});                               // store %20
  if (second_output) Emit(w, 62, {21, 40});            // store %21
  Emit(w, 253, {});                                    // OpReturn
  Emit(w, 56, {});                                     // OpFunctionEnd
  return w;
}

static uint32_t CountOp(const Words& w, uint32_t op) {
  uint32_t n = 0;
  for (size_t at = 5; at < w.size(); at += w[at] >> 16)
    if ((w[at] & 0xFFFF) == op) ++n;
  return n;
}

int main() {
  // 1. The closed-form 7e3 decode the epilogue emits equals the shaders' From7e3 for every code, and every
  //    decoded value is exact in half (the f3 RGBA16F image keeps it) and re-encodes to the same code.
  for (uint32_t code = 0; code < 1024; ++code) {
    const float reference = From7e3Reference(code);
    Check(BitsOfFloat(Decode7e3Epilogue(code)) == BitsOfFloat(reference), "closed-form 7e3 decode differs");
    Check(BitsOfFloat(F16ToF32(F32ToF16Rtne(reference))) == BitsOfFloat(reference), "7e3 value not exact in half");
    Check(A7e3Reference(F16ToF32(F32ToF16Rtne(reference))) == code, "f3 -> word does not give the code back");
  }
  for (uint32_t a = 0; a < 4; ++a) {
    const float third = float(a) * (1.0f / 3.0f);
    Check(uint32_t(RoundEvenModel(Clamp01Model(F16ToF32(F32ToF16Rtne(third))) * 3.0f)) == a,
          "alpha 2-bit does not survive f3 storage");
  }

  // 2. Every value the restore draw really writes: a point-sampled UNORM10 texel, x = k / 1023 (and the 2-bit
  //    alpha k / 3). All 1024 codes on every channel, with the f2 attachment rounding to nearest-even AND toward
  //    zero: the epilogue equals today's chain either way.
  uint64_t compared = 0;
  for (uint32_t k = 0; k < 1024; ++k) {
    for (uint32_t a = 0; a < 4; ++a) {
      const float x[4] = {float(k) / 1023.0f, float(1023 - k) / 1023.0f, float((k * 7) & 1023) / 1023.0f,
                          float(a) / 3.0f};
      float epilogue[4], today[4], today_rtz[4];
      Restore7e3EpilogueModel(x, epilogue);
      Restore7e3TodayModel(x, today);
      Restore7e3TodayModel(x, today_rtz, true);
      Check(SameBits(epilogue, today), "code input: epilogue != f2 store (RTNE) + conversion");
      Check(SameBits(epilogue, today_rtz), "code input: epilogue != f2 store (RTZ) + conversion");
      Check(WordFromF3(epilogue) == WordFromF2(x), "code input: later consumers would see another word");
      ++compared;
    }
  }

  // 3. Dense sweep of float bit patterns (every 61st pattern, all signs, NaN, infinities, denormals) on every
  //    channel position: the epilogue equals today's chain with a round-to-nearest-even f2 attachment, and the
  //    EDRAM word any later 32-bit consumer re-packs is the same.
  uint64_t swept = 0;
  for (uint64_t bits = 0; bits <= 0xFFFFFFFFull; bits += 61) {
    const float v = FloatOfBits(uint32_t(bits));
    const float x[4] = {v, v, v, v};
    float epilogue[4], today[4];
    Restore7e3EpilogueModel(x, epilogue);
    Restore7e3TodayModel(x, today);
    if (!SameBits(epilogue, today)) {
      std::fprintf(stderr, "mismatch at bits %08X\n", uint32_t(bits));
      Check(false, "dense sweep: epilogue != f2 store + conversion");
    }
    Check(WordFromF3(epilogue) == WordFromF2(x), "dense sweep: later consumers would see another word");
    ++swept;
  }
  // Negative control: the same epilogue without the OpQuantizeToF16 step must disagree somewhere in the sweep
  // (the test can tell a wrong epilogue apart).
  {
    uint64_t differences = 0;
    for (uint64_t bits = 0; bits <= 0xFFFFFFFFull && !differences; bits += 61) {
      const float v = FloatOfBits(uint32_t(bits));
      const float x[4] = {v, v, v, v};
      float today[4];
      Restore7e3TodayModel(x, today);
      const uint32_t code = uint32_t(RoundEvenModel(Clamp01Model(v) * 1023.0f));
      if (BitsOfFloat(Decode7e3Epilogue(code)) != BitsOfFloat(today[0])) ++differences;
    }
    Check(differences != 0, "negative control: an epilogue without the half step was not caught");
  }
  // Every half value exactly (the inputs that survive the attachment unchanged), and both neighbours of every
  // code boundary (k + 0.5) / 1023.
  for (uint32_t h = 0; h < 65536; ++h) {
    const float v = F16ToF32(uint16_t(h));
    const float x[4] = {v, v, v, v};
    float epilogue[4], today[4];
    Restore7e3EpilogueModel(x, epilogue);
    Restore7e3TodayModel(x, today);
    Check(SameBits(epilogue, today), "half sweep: epilogue != f2 store + conversion");
  }
  for (uint32_t k = 0; k < 1024; ++k) {
    const float boundary = (float(k) + 0.5f) / 1023.0f;
    for (int d = -64; d <= 64; ++d) {
      const float v = FloatOfBits(uint32_t(int64_t(BitsOfFloat(boundary)) + d));
      const float x[4] = {v, v, v, v};
      float epilogue[4], today[4];
      Restore7e3EpilogueModel(x, epilogue);
      Restore7e3TodayModel(x, today);
      Check(SameBits(epilogue, today), "boundary sweep: epilogue != f2 store + conversion");
    }
  }

  // 4. The SPIR-V transform: one store rewritten, the epilogue's operations present, id bound raised.
  {
    const Words in = Fixture(false, true);
    Words out;
    Restore7e3Stats stats;
    std::string reason;
    Check(TransformRestore7e3(in, 1u, out, stats, reason), "transform failed on the DXC-shaped module");
    Check(stats.stores == 1, "one store expected");
    Check(out[3] > in[3], "id bound not raised");
    Check(CountOp(out, 116) == 4, "4 OpQuantizeToF16 expected");
    Check(CountOp(out, 12) == 8, "8 OpExtInst (FClamp + Round per channel) expected");
    Check(CountOp(out, 169) == 3, "3 OpSelect (7e3 decode) expected");
    Check(CountOp(out, 21) == 2, "one uint type added next to the signed int");
    Check(CountOp(out, 62) == CountOp(in, 62), "store count changed");
    // The Location 0 store now stores the new vector; the Location 1 store is untouched.
    bool rewritten = false, untouched = false;
    for (size_t at = 5; at < out.size(); at += out[at] >> 16) {
      if ((out[at] & 0xFFFF) != 62) continue;
      if (out[at + 1] == 20) rewritten = out[at + 2] != 40;
      if (out[at + 1] == 21) untouched = out[at + 2] == 40;
    }
    Check(rewritten && untouched, "wrong store rewritten");
    Check(TransformRestore7e3(in, 3u, out, stats, reason) && stats.stores == 2, "two locations");
  }
  {
    Words out;
    Restore7e3Stats stats;
    std::string reason;
    Check(!TransformRestore7e3(Fixture(true, false), 1u, out, stats, reason) && out.empty(),
          "an access chain into the output must be refused");
    Check(!TransformRestore7e3(Fixture(false, false), 4u, out, stats, reason), "missing Location 2 must fail");
    Check(!TransformRestore7e3(Fixture(false, false), 2u, out, stats, reason), "never-stored output must fail");
  }

  std::printf("restore 7e3: %llu code cases, %llu swept patterns, closed form and transform OK\n",
              (unsigned long long)compared, (unsigned long long)swept);
  return 0;
}
