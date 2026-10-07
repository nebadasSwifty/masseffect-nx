// FoldTextureSigns (me_texture_signs_spirv.h) on a synthetic module: shifts by 24 of shared descriptor words
// the key carries become constants; other shifts are left alone. No game data.
#include "me_texture_signs_spirv.h"
#include <cstdio>
#include <stdexcept>
using Words = std::vector<uint32_t>;
static void Check(bool b, const char* s) { if (!b) throw std::runtime_error(s); }
static void Emit(Words& w, uint32_t op, std::initializer_list<uint32_t> a) {
  w.push_back((uint32_t(a.size() + 1) << 16) | op);
  w.insert(w.end(), a);
}
// The instruction that defines `id` (result at word 2), or nullptr.
static const uint32_t* Find(const Words& w, uint32_t id) {
  for (size_t at = 5; at < w.size(); at += w[at] >> 16)
    if ((w[at] >> 16) >= 3 && w[at + 2] == id && (w[at] & 0xFFFF) != 71) return &w[at];
  return nullptr;
}
static Words Fixture(bool with_shared_block) {
  Words w{0x07230203, 0x00010300, 0, 100, 0};
  Emit(w, 17, {1});                            // OpCapability Shader
  Emit(w, 14, {0, 1});                         // OpMemoryModel
  Emit(w, 15, {4, 30, 0x6E69616D, 0});         // OpEntryPoint Fragment %30 "main"
  Emit(w, 16, {30, 7});                        // OriginUpperLeft
  if (with_shared_block) {
    Emit(w, 71, {10, 34, 4});                  // %10: set 4, binding 2 = the shared constants
    Emit(w, 71, {10, 33, 2});
  }
  Emit(w, 71, {11, 34, 4});                    // %11: set 4, binding 1 = the pixel constants
  Emit(w, 71, {11, 33, 1});
  Emit(w, 19, {2});                            // void
  Emit(w, 33, {3, 2});                         // void()
  Emit(w, 21, {4, 32, 0});                     // uint
  Emit(w, 21, {5, 32, 1});                     // int
  Emit(w, 22, {6, 32});                        // float
  Emit(w, 23, {7, 6, 4});                      // float4
  Emit(w, 43, {4, 20, 31});
  Emit(w, 43, {5, 21, 0});
  Emit(w, 43, {5, 22, 1});
  Emit(w, 43, {5, 23, 2});
  Emit(w, 43, {4, 24, 24});                    // uint 24
  Emit(w, 43, {4, 25, 0});                     // uint 0 (reused for a folded 0)
  Emit(w, 43, {5, 27, 4});
  Emit(w, 43, {5, 28, 8});
  Emit(w, 28, {8, 7, 20});                     // float4[31]
  Emit(w, 30, {9, 8});                         // struct { float4 v[31]; }
  Emit(w, 32, {12, 2, 9});                     // Uniform pointers
  Emit(w, 32, {13, 2, 6});
  Emit(w, 59, {12, 10, 2});
  Emit(w, 59, {12, 11, 2});
  Emit(w, 54, {2, 30, 0, 3});                  // OpFunction
  Emit(w, 248, {31});
  // word 1 (v[0].y)
  Emit(w, 65, {13, 40, 10, 21, 21, 22}); Emit(w, 61, {6, 41, 40}); Emit(w, 124, {4, 42, 41});
  Emit(w, 194, {4, 43, 42, 24});
  // word 18 (v[4].z): the 3D heap of register 2
  Emit(w, 65, {13, 44, 10, 21, 27, 23}); Emit(w, 61, {6, 45, 44}); Emit(w, 124, {4, 46, 45});
  Emit(w, 194, {4, 47, 46, 24});
  // word 33 (v[8].y) through an OpCopyObject
  Emit(w, 65, {13, 50, 10, 21, 28, 22}); Emit(w, 61, {6, 51, 50}); Emit(w, 124, {4, 52, 51});
  Emit(w, 83, {4, 53, 52}); Emit(w, 194, {4, 54, 53, 24});
  // not the shared block: left alone
  Emit(w, 65, {13, 60, 11, 21, 21, 22}); Emit(w, 61, {6, 61, 60}); Emit(w, 124, {4, 62, 61});
  Emit(w, 194, {4, 63, 62, 24});
  // word 5 (v[1].y), not carried by the key: left alone
  Emit(w, 65, {13, 70, 10, 21, 22, 22}); Emit(w, 61, {6, 71, 70}); Emit(w, 124, {4, 72, 71});
  Emit(w, 194, {4, 73, 72, 24});
  Emit(w, 253, {});
  Emit(w, 56, {});
  return w;
}
int main() {
  try {
    uint8_t signs[48] = {};
    signs[1] = 0x3F;
    signs[18] = 0;
    signs[33] = 0xAA;
    const uint64_t known = (uint64_t(1) << 1) | (uint64_t(1) << 18) | (uint64_t(1) << 33);
    const Words in = Fixture(true);
    Words out;
    me::native::TextureSignsFold stats;
    std::string reason;
    Check(me::native::FoldTextureSigns(in, signs, known, out, stats, reason), "fold failed");
    Check(stats.folded == 3 && stats.unknown == 1 && stats.other == 1, "fold counts");
    Check(out[3] == 102, "two new constants expected (0x3F and 0xAA; 0 is reused)");
    const auto folded_to = [&](uint32_t id) -> uint32_t {
      const uint32_t* i = Find(out, id);
      Check(i && (i[0] & 0xFFFF) == 83 && i[1] == 4, "folded shift must be OpCopyObject %uint");
      const uint32_t* c = Find(out, i[3]);
      Check(c && (c[0] & 0xFFFF) == 43 && c[1] == 4, "folded value must be an OpConstant %uint");
      return c[3];
    };
    Check(folded_to(43) == 0x3F, "word 1");
    Check(folded_to(47) == 0 && Find(out, 47)[3] == 25, "word 18 must reuse the existing uint 0");
    Check(folded_to(54) == 0xAA, "word 33 through OpCopyObject");
    Check((Find(out, 63)[0] & 0xFFFF) == 194 && (Find(out, 73)[0] & 0xFFFF) == 194, "other shifts kept");
    // New constants precede the first function.
    size_t function = 0, last_constant = 0;
    for (size_t at = 5; at < out.size(); at += out[at] >> 16) {
      if ((out[at] & 0xFFFF) == 54 && !function) function = at;
      if ((out[at] & 0xFFFF) == 43) last_constant = at;
    }
    Check(function && last_constant < function, "constants must precede the function");
    // Nothing known, or no shared block: the module is returned unchanged.
    Check(me::native::FoldTextureSigns(in, signs, 0, out, stats, reason) && out == in && !stats.folded, "known 0");
    const Words plain = Fixture(false);
    Check(me::native::FoldTextureSigns(plain, signs, known, out, stats, reason) && out == plain, "no block");
    Words bad = in;
    bad[0] = 0;
    Check(!me::native::FoldTextureSigns(bad, signs, known, out, stats, reason) && out.empty(), "bad header");
    std::puts("texture signs fold: OK");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
