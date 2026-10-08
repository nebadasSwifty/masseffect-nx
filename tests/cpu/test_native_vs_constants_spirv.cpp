// AnalyzeVertexConstants / FoldVertexConstants (me_vs_constants_spirv.h) on a synthetic module: compared
// components of the vertex constant block and identity input remap words become constants; the rest is left
// alone. No game data.
#include "me_vs_constants_spirv.h"
#include <cstdio>
#include <stdexcept>
using Words = std::vector<uint32_t>;
static void Check(bool b, const char* s) { if (!b) throw std::runtime_error(s); }
static void Emit(Words& w, uint32_t op, std::initializer_list<uint32_t> a) {
  w.push_back((uint32_t(a.size() + 1) << 16) | op);
  w.insert(w.end(), a);
}
static const uint32_t* Find(const Words& w, uint32_t id) {
  for (size_t at = 5; at < w.size(); at += w[at] >> 16)
    if ((w[at] >> 16) >= 3 && w[at + 2] == id && (w[at] & 0xFFFF) != 71) return &w[at];
  return nullptr;
}
static Words Fixture() {
  Words w{0x07230203, 0x00010300, 0, 200, 0};
  Emit(w, 17, {1});                            // OpCapability Shader
  Emit(w, 14, {0, 1});                         // OpMemoryModel
  Emit(w, 15, {0, 30, 0x6E69616D, 0});         // OpEntryPoint Vertex %30 "main"
  Emit(w, 71, {10, 34, 4});                    // %10: set 4, binding 0 = the vertex constants
  Emit(w, 71, {10, 33, 0});
  Emit(w, 71, {11, 34, 4});                    // %11: set 4, binding 2 = the shared constants
  Emit(w, 71, {11, 33, 2});
  Emit(w, 19, {2});                            // void
  Emit(w, 33, {3, 2});                         // void()
  Emit(w, 21, {4, 32, 0});                     // uint
  Emit(w, 21, {5, 32, 1});                     // int
  Emit(w, 22, {6, 32});                        // float
  Emit(w, 23, {7, 6, 4});                      // float4
  Emit(w, 20, {14});                           // bool
  Emit(w, 43, {4, 20, 256});
  Emit(w, 43, {4, 26, 31});
  Emit(w, 43, {5, 21, 0});
  Emit(w, 43, {5, 22, 3});
  Emit(w, 43, {5, 23, 8});
  Emit(w, 43, {5, 24, 18});
  Emit(w, 43, {5, 25, 2});
  Emit(w, 43, {5, 27, 3});
  Emit(w, 43, {6, 29, 0x3F800000});           // float 1.0 (reused for a folded 1.0)
  Emit(w, 28, {8, 7, 20});                     // float4[256]
  Emit(w, 30, {9, 8});                         // struct { float4 v[256]; }
  Emit(w, 28, {15, 7, 26});                    // float4[31]
  Emit(w, 30, {16, 15});                       // struct { float4 v[31]; }
  Emit(w, 32, {12, 2, 9});                     // Uniform pointers
  Emit(w, 32, {17, 2, 16});
  Emit(w, 32, {13, 2, 7});
  Emit(w, 32, {18, 2, 6});
  Emit(w, 59, {12, 10, 2});
  Emit(w, 59, {17, 11, 2});
  Emit(w, 54, {2, 30, 0, 3});                  // OpFunction
  Emit(w, 248, {31});
  // v[8].x compared through an OpCopyObject of the load: folded
  Emit(w, 65, {13, 40, 10, 21, 23}); Emit(w, 61, {7, 41, 40}); Emit(w, 83, {7, 42, 41});
  Emit(w, 81, {6, 43, 42, 0}); Emit(w, 186, {14, 44, 43, 29});      // OpFOrdGreaterThan
  // v[8].x again, not compared itself: folded too (same component)
  Emit(w, 81, {6, 45, 41, 0});
  // v[8].y and v[3].x, read but never compared: left alone
  Emit(w, 81, {6, 46, 41, 1});
  Emit(w, 65, {13, 47, 10, 21, 22}); Emit(w, 61, {7, 48, 47}); Emit(w, 81, {6, 49, 48, 0});
  // remap of location 0 (shared v[18].z = word 74) and location 1 (v[18].w = word 75)
  Emit(w, 65, {18, 50, 11, 21, 24, 25}); Emit(w, 61, {6, 51, 50}); Emit(w, 124, {4, 52, 51});
  Emit(w, 65, {18, 53, 11, 21, 24, 27}); Emit(w, 61, {6, 54, 53}); Emit(w, 124, {4, 55, 54});
  Emit(w, 253, {});
  Emit(w, 56, {});
  return w;
}
int main() {
  try {
    const Words in = Fixture();
    me::native::VertexConstantsInfo info;
    std::string reason;
    Check(me::native::AnalyzeVertexConstants(in, info, reason), "analysis failed");
    Check(info.n_components == 1 && info.components[0] == 8 * 4 && !info.more_components, "compared components");
    Check(info.remap_locations == 0x3, "remap locations");
    Words out;
    me::native::VertexConstantsFold stats;
    const uint32_t value = 0x3F800000;  // 1.0: the existing constant must be reused
    Check(me::native::FoldVertexConstants(in, info.components, &value, 1, 0x1, out, stats, reason), "fold failed");
    Check(stats.folded_components == 2 && stats.folded_remaps == 1, "fold counts");
    Check(out[3] == 201, "one new constant expected (uint 0xFFF; float 1.0 is reused)");
    const auto folded_to = [&](uint32_t id, uint32_t type) -> uint32_t {
      const uint32_t* i = Find(out, id);
      Check(i && (i[0] & 0xFFFF) == 83 && i[1] == type, "folded value must be an OpCopyObject");
      const uint32_t* c = Find(out, i[3]);
      Check(c && (c[0] & 0xFFFF) == 43 && c[1] == type, "folded value must be an OpConstant");
      return c[3];
    };
    Check(folded_to(43, 6) == value && Find(out, 43)[3] == 29, "compared extract");
    Check(folded_to(45, 6) == value, "other extract of the same component");
    Check((Find(out, 46)[0] & 0xFFFF) == 81 && (Find(out, 49)[0] & 0xFFFF) == 81, "other components kept");
    Check(folded_to(52, 4) == 0xFFF, "identity remap of location 0");
    Check((Find(out, 55)[0] & 0xFFFF) == 124, "location 1 is not in the identity mask: kept");
    size_t function = 0, last_constant = 0;
    for (size_t at = 5; at < out.size(); at += out[at] >> 16) {
      if ((out[at] & 0xFFFF) == 54 && !function) function = at;
      if ((out[at] & 0xFFFF) == 43) last_constant = at;
    }
    Check(function && last_constant < function, "constants must precede the function");
    // Nothing to fold: unchanged.
    Check(me::native::FoldVertexConstants(in, info.components, &value, 0, 0, out, stats, reason) && out == in &&
              !stats.folded_components && !stats.folded_remaps, "nothing folded");
    Words bad = in;
    bad[0] = 0;
    Check(!me::native::FoldVertexConstants(bad, info.components, &value, 1, 1, out, stats, reason) && out.empty(),
          "bad header");
    Check(!me::native::AnalyzeVertexConstants(bad, info, reason), "bad header analysis");
    std::puts("vertex shader constants fold: OK");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
