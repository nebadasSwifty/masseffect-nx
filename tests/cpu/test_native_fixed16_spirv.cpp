// Xenos k_16_16 emulation (me_fixed16_spirv.h, masseffect_native_velocity_16_16): the pixel shader epilogue must
// make a UNORM16 attachment store exactly the Xenos EDRAM word (fixed point -32...32), whether the attachment
// rounds to nearest or toward zero; the resolve model must give back the value; and the SPIR-V transform must
// rewrite only the selected output stores. CPU models plus a synthetic DXC-shaped module. No game data.
#include "me_fixed16_spirv.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace me::native;
using Words = std::vector<uint32_t>;

static void Check(bool b, const char* s) {
  if (!b) throw std::runtime_error(s);
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

// Fragment module: outputs at Location 0 and 1, stored once each (optionally an access chain into output 0).
static Words Fixture(bool chain, bool store_second) {
  Words w{0x07230203, 0x00010000, 0, 100, 0};
  Emit(w, 17, {1});
  EmitString(w, 11, 1, "GLSL.std.450");
  Emit(w, 14, {0, 1});
  Emit(w, 15, {4, 30, 0x6E69616D, 0, 20, 21});
  Emit(w, 16, {30, 7});
  Emit(w, 71, {20, 30, 0});
  Emit(w, 71, {21, 30, 1});
  Emit(w, 19, {2});
  Emit(w, 33, {3, 2});
  Emit(w, 22, {6, 32});
  Emit(w, 23, {7, 6, 4});
  Emit(w, 32, {8, 3, 7});
  Emit(w, 32, {9, 3, 6});
  Emit(w, 21, {10, 32, 1});
  Emit(w, 43, {6, 11, 0x3F000000});
  Emit(w, 43, {10, 12, 3});
  Emit(w, 59, {8, 20, 3});
  Emit(w, 59, {8, 21, 3});
  Emit(w, 54, {2, 30, 0, 3});
  Emit(w, 248, {31});
  Emit(w, 80, {7, 40, 11, 11, 11, 11});
  if (chain) {
    Emit(w, 65, {9, 41, 20, 12});
    Emit(w, 62, {41, 11});
  }
  Emit(w, 62, {20, 40});
  if (store_second) Emit(w, 62, {21, 40});
  Emit(w, 253, {});
  Emit(w, 56, {});
  return w;
}

static uint32_t CountOp(const Words& w, uint32_t op) {
  uint32_t n = 0;
  for (size_t at = 5; at < w.size(); at += w[at] >> 16)
    if ((w[at] & 0xFFFF) == op) ++n;
  return n;
}

static void CheckStore(float x) {
  const uint16_t word = Fixed16EncodeModel(x);
  const float out = Fixed16EpilogueOutputModel(x);
  if (Fixed16UnormStoreModel(out) != word || Fixed16UnormStoreModel(out, true) != word) {
    std::fprintf(stderr, "x %a: word %04X, stored %04X / %04X\n", double(x), word, Fixed16UnormStoreModel(out),
                 Fixed16UnormStoreModel(out, true));
    Check(false, "the epilogue output does not store the EDRAM word");
  }
}

int main() {
  // 1. Every 16-bit pattern survives the epilogue output + UNORM16 store (both rounding modes): the value each
  //    pattern stands for, and both float neighbours of it.
  for (uint32_t word = 0; word < 65536; ++word) {
    if (word == 0x8000) continue;  // -32768 decodes to -32, which re-encodes as 0x8001 (as on xenia)
    const float v = Fixed16DecodeModel(uint16_t(word));
    Check(Fixed16EncodeModel(v) == word, "decode -> encode does not give the pattern back");
    for (int d = -2; d <= 2; ++d) CheckStore(Fixed16FloatOfBits(uint32_t(int64_t(Fixed16BitsOfFloat(v)) + d)));
  }
  // 2. Dense sweep of float bit patterns (NaN, infinities, denormals, out of range).
  uint64_t swept = 0;
  for (uint64_t bits = 0; bits <= 0xFFFFFFFFull; bits += 97) {
    CheckStore(Fixed16FloatOfBits(uint32_t(bits)));
    ++swept;
  }
  Check(Fixed16EncodeModel(std::nanf("")) == 0, "NaN must store 0");
  Check(Fixed16EncodeModel(100.0f) == 0x7FFF && Fixed16EncodeModel(-100.0f) == 0x8001, "clamp to -32...32");
  Check(Fixed16EncodeModel(0.5f) == 0x0200 && Fixed16EncodeModel(1.0f) == 0x0400, "0.5 -> 0x0200, 1 -> 0x0400");
  // Negative control: without the 0.25 offset, a toward-zero attachment loses patterns.
  {
    uint32_t lost = 0;
    for (uint32_t word = 1; word < 65535; ++word) {
      const float plain = float(word) * (1.0f / 65535.0f);
      if (Fixed16UnormStoreModel(plain, true) != word) ++lost;
    }
    Check(lost != 0, "negative control: the plain word / 65535 output was not caught");
  }

  // 3. Resolve (unsigned fraction, exp bias 0) of a written value v in [0, 1] gives v back within the EDRAM
  //    precision (32 / 32767 / 2 = 0.000488, i.e. <= 33 UNORM16 steps): what the game reads through the k_16_16
  //    fetch (signs 0, exp_adjust 0). Values below 0 read 0, above 1 read 1.
  for (uint32_t k = 0; k <= 65535; ++k) {
    const float v = float(k) / 65535.0f;
    const uint16_t texel = Fixed16ResolveModel(Fixed16EncodeModel(v), 0, 0);
    const int32_t old_texel = int32_t(k);  // the old path: UNORM16(v) copied as is
    Check(std::abs(int32_t(texel) - old_texel) <= 33, "resolve differs from the value by more than the EDRAM step");
  }
  Check(Fixed16ResolveModel(Fixed16EncodeModel(-0.25f), 0, 0) == 0, "negative value must resolve to 0");
  Check(Fixed16ResolveModel(Fixed16EncodeModel(3.0f), 0, 0) == 0xFFFF, "value > 1 must resolve to 1");
  Check(std::abs(int32_t(Fixed16ResolveModel(Fixed16EncodeModel(3.0f), -2, 0)) - 49151) <= 4,
        "exp bias -2 must scale by 1/4");
  Check(Fixed16ResolveModel(Fixed16EncodeModel(-0.5f), 0, 1) == uint16_t(-16383 & 0xFFFF) ||
            Fixed16ResolveModel(Fixed16EncodeModel(-0.5f), 0, 1) == uint16_t(-16384 & 0xFFFF),
        "signed fraction pack");
  // The raw-word reading the old docs assumed (resolve = copy, fetch = word / 65535) would read 0.5 as 0.0078.
  Check(Fixed16EncodeModel(0.5f) == 0x0200 && Fixed16ResolveModel(0x0200, 0, 0) > 0x7FF0,
        "0.5 must come back near 0x8000 through the converting resolve");

  // 4. The SPIR-V transform.
  {
    const Words in = Fixture(false, true);
    Words out;
    Fixed16Stats stats;
    uint32_t missing = 0;
    std::string reason;
    Check(TransformFixed16Encode(in, 1u, out, stats, missing, reason), "transform failed on the DXC-shaped module");
    Check(stats.stores == 1 && missing == 0, "one store expected");
    Check(out[3] > in[3], "id bound not raised");
    Check(CountOp(out, 156) == 2, "2 OpIsNan expected (x and y)");
    Check(CountOp(out, 12) == 4, "4 OpExtInst (FClamp + RoundEven per channel) expected");
    Check(CountOp(out, 169) == 4, "4 OpSelect expected");
    Check(CountOp(out, 62) == CountOp(in, 62), "store count changed");
    bool rewritten = false, untouched = false;
    for (size_t at = 5; at < out.size(); at += out[at] >> 16) {
      if ((out[at] & 0xFFFF) != 62) continue;
      if (out[at + 1] == 20) rewritten = out[at + 2] != 40;
      if (out[at + 1] == 21) untouched = out[at + 2] == 40;
    }
    Check(rewritten && untouched, "wrong store rewritten");
    Check(TransformFixed16Encode(in, 3u, out, stats, missing, reason) && stats.stores == 2, "two locations");
    Check(TransformFixed16Encode(in, 4u, out, stats, missing, reason) && stats.stores == 0 && missing == 4 &&
              out == in, "a missing Location is returned unchanged");
    Check(TransformFixed16Encode(Fixture(false, false), 2u, out, stats, missing, reason) && missing == 2 &&
              out == Fixture(false, false), "a never-stored output is returned unchanged");
    Check(!TransformFixed16Encode(Fixture(true, false), 1u, out, stats, missing, reason) && out.empty(),
          "an access chain into the output must be refused");
  }

  std::printf("k_16_16: 65535 patterns, %llu swept floats, resolve model and transform OK\n",
              (unsigned long long)swept);
  return 0;
}
