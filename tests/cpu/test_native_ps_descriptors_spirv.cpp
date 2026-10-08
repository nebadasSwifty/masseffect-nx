// me_ps_descriptors_spirv.h on a synthetic pixel shader shaped like the library's (DXC output of shader_common.h):
//   A  ReadFltBoundsFromShared: the clamp bounds become loads of shared words 122/123 (the renderer writes the same
//      bits there);
//   B  CombineTexture2DFetches: the 2D heap becomes the combined heap (set 5), the OpSampledImage whose sampler is
//      the one of the same register becomes the combined descriptor, the other keeps its separate sampler;
//   C  DropIndexMasks: after the texture signs are folded the `& 0xFFFFFF` of a cleared word goes away;
// plus the soundness refusals. When spirv-val is on the PATH every output is validated (Vulkan 1.2). With
// ME_PS_LIBRARY_DIR=<folder of library pixel shader .spv> the whole chain (signs fold, C, B, A) runs on each of them
// and each result must pass spirv-val. No game data in the repository.
#include "me_texture_signs_spirv.h"
#include "me_ps_descriptors_spirv.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
using Words = std::vector<uint32_t>;
static void Check(bool b, const char* s) {
  if (!b) throw std::runtime_error(s);
}
static void Emit(Words& w, uint32_t op, std::initializer_list<uint32_t> a) {
  w.push_back((uint32_t(a.size() + 1) << 16) | op);
  w.insert(w.end(), a);
}
// The instruction that defines `id` (result at word 2, or word 1 for types), or nullptr.
static const uint32_t* Find(const Words& w, uint32_t id) {
  for (size_t at = 5; at < w.size(); at += w[at] >> 16) {
    const uint32_t op = w[at] & 0xFFFF;
    if (op == 71 || op == 72 || op == 5 || op == 15) continue;
    if ((w[at] >> 16) >= 3 && w[at + 2] == id) return &w[at];
  }
  return nullptr;
}
static size_t Count(const Words& w, uint32_t op) {
  size_t n = 0;
  for (size_t at = 5; at < w.size(); at += w[at] >> 16) n += (w[at] & 0xFFFF) == op;
  return n;
}
static uint32_t DecorationValue(const Words& w, uint32_t id, uint32_t decoration) {
  for (size_t at = 5; at < w.size(); at += w[at] >> 16)
    if ((w[at] & 0xFFFF) == 71 && (w[at] >> 16) == 4 && w[at + 1] == id && w[at + 2] == decoration) return w[at + 3];
  return UINT32_MAX;
}

static bool g_validate = false;
static std::filesystem::path g_folder;
static void Validate(const Words& w, const char* name) {
  if (!g_validate) return;
  const auto path = g_folder / (std::string(name) + ".spv");
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(w.data()), w.size() * 4);
  const std::string command = "spirv-val --target-env vulkan1.2 '" + path.string() + "'";
  if (std::system(command.c_str()) != 0) throw std::runtime_error(std::string("spirv-val failed: ") + name);
}

// bad_use: a 2D descriptor word also feeds a float conversion (B must refuse).
static Words Fixture(bool bad_use) {
  Words w{0x07230203, 0x00010500, 0, 100, 0};
  Emit(w, 17, {1});                                  // OpCapability Shader
  Emit(w, 17, {5302});                               // RuntimeDescriptorArray
  Emit(w, 11, {1, 0x4C534C47, 0x6474732E, 0x3035342E, 0});  // %1 = "GLSL.std.450"
  Emit(w, 14, {0, 1});                               // Logical GLSL450
  Emit(w, 15, {4, 50, 0x6E69616D, 0, 43, 40, 41, 42});  // Fragment %50 "main" + interface
  Emit(w, 16, {50, 7});                              // OriginUpperLeft
  Emit(w, 71, {20, 6, 16});                          // ArrayStride 16
  Emit(w, 71, {21, 2});                              // Block
  Emit(w, 72, {21, 0, 35, 0});                       // member 0 Offset 0
  Emit(w, 71, {40, 34, 4}); Emit(w, 71, {40, 33, 2});  // shared block: set 4 binding 2
  Emit(w, 71, {41, 34, 0}); Emit(w, 71, {41, 33, 0});  // 2D heap: set 0 binding 0
  Emit(w, 71, {42, 34, 3}); Emit(w, 71, {42, 33, 0});  // sampler heap: set 3 binding 0
  Emit(w, 71, {43, 30, 0});                          // output Location 0
  Emit(w, 19, {2}); Emit(w, 33, {3, 2});             // void, void()
  Emit(w, 22, {4, 32});                              // float
  Emit(w, 21, {5, 32, 0});                           // uint
  Emit(w, 21, {6, 32, 1});                           // int
  Emit(w, 23, {7, 4, 4}); Emit(w, 23, {8, 4, 2});    // float4, float2
  Emit(w, 43, {5, 10, 31});
  Emit(w, 43, {6, 11, 0});
  Emit(w, 43, {6, 12, 12});
  Emit(w, 43, {5, 13, 2});
  Emit(w, 43, {5, 14, 24});
  Emit(w, 43, {5, 15, 0x00FFFFFF});
  Emit(w, 43, {4, 16, 0x3F000000});                  // 0.5
  Emit(w, 43, {4, 17, me::native::kFltMinBits});     // shader_common.h FLT_MIN
  Emit(w, 43, {4, 18, me::native::kFltMaxBits});     // FLT_MAX
  Emit(w, 43, {5, 19, 1});
  Emit(w, 28, {20, 7, 10});                          // float4[31]
  Emit(w, 30, {21, 20});                             // struct { float4 v[31]; }
  Emit(w, 32, {22, 2, 21}); Emit(w, 32, {23, 2, 4});  // Uniform pointers
  Emit(w, 25, {24, 4, 1, 0, 0, 0, 1, 0});            // image 2D sampled
  Emit(w, 29, {25, 24}); Emit(w, 32, {26, 0, 25}); Emit(w, 32, {27, 0, 24});
  Emit(w, 26, {28});                                 // sampler
  Emit(w, 29, {29, 28}); Emit(w, 32, {30, 0, 29}); Emit(w, 32, {31, 0, 28});
  Emit(w, 27, {32, 24});                             // sampled image
  Emit(w, 32, {33, 3, 7});                           // Output float4*
  Emit(w, 44, {8, 34, 16, 16});                      // float2(0.5, 0.5)
  Emit(w, 59, {22, 40, 2}); Emit(w, 59, {26, 41, 0}); Emit(w, 59, {30, 42, 0}); Emit(w, 59, {33, 43, 3});
  Emit(w, 54, {2, 50, 0, 3});
  Emit(w, 248, {51});
  // register 2: 2D word 2 (v[0].z), sampler word 50 (v[12].z)
  Emit(w, 65, {23, 60, 40, 11, 11, 13}); Emit(w, 61, {4, 61, 60}); Emit(w, 124, {5, 62, 61});
  Emit(w, 194, {5, 63, 62, 14});                     // sign byte
  Emit(w, 199, {5, 64, 62, 15});                     // index
  Emit(w, 65, {27, 65, 41, 64}); Emit(w, 61, {24, 66, 65});
  Emit(w, 65, {23, 67, 40, 11, 12, 13}); Emit(w, 61, {4, 68, 67}); Emit(w, 124, {5, 69, 68});
  Emit(w, 65, {31, 70, 42, 69}); Emit(w, 61, {28, 71, 70});
  Emit(w, 86, {32, 72, 66, 71});
  Emit(w, 87, {7, 73, 72, 34});
  // the same image with the sampler of register 1 (word 49): must stay separate
  Emit(w, 65, {23, 74, 40, 11, 12, 19}); Emit(w, 61, {4, 75, 74}); Emit(w, 124, {5, 76, 75});
  Emit(w, 65, {31, 77, 42, 76}); Emit(w, 61, {28, 78, 77});
  Emit(w, 61, {24, 79, 65});
  Emit(w, 86, {32, 80, 79, 78});
  Emit(w, 87, {7, 81, 80, 34});
  Emit(w, 81, {4, 82, 73, 0});
  Emit(w, 12, {4, 83, 1, 43, 82, 17, 18});           // FClamp(x, FLT_MIN, FLT_MAX)
  Emit(w, 112, {4, 84, bad_use ? 62u : 63u});        // ConvertUToF(sign) (or of the raw word: bad)
  Emit(w, 129, {4, 85, 83, 84});
  Emit(w, 81, {4, 86, 81, 1});
  Emit(w, 129, {4, 87, 85, 86});
  Emit(w, 80, {7, 88, 87, 87, 87, 87});
  Emit(w, 62, {43, 88});
  Emit(w, 253, {});
  Emit(w, 56, {});
  return w;
}

static void TestA() {
  const Words in = Fixture(false);
  Validate(in, "fixture");
  Words out;
  me::native::FltBoundsStats stats;
  std::string reason;
  Check(me::native::ReadFltBoundsFromShared(in, out, stats, reason), "A failed");
  Check(stats.replaced == 2 && stats.functions == 1, "A: both bounds of the clamp, once");
  // The FClamp operands are loads of v[30].z and v[30].w of the shared block.
  const uint32_t* clamp = Find(out, 83);
  Check(clamp && (clamp[0] & 0xFFFF) == 12 && clamp[4] == 43, "A: clamp kept");
  const uint32_t expected_component[2] = {me::native::kSharedWordFltMin % 4, me::native::kSharedWordFltMax % 4};
  for (int k = 0; k < 2; ++k) {
    const uint32_t* load = Find(out, clamp[6 + k]);
    Check(load && (load[0] & 0xFFFF) == 61, "A: bound is a load");
    const uint32_t* chain = Find(out, load[3]);
    Check(chain && (chain[0] & 0xFFFF) == 65 && chain[3] == 40, "A: load of the shared block");
    const uint32_t* element = Find(out, chain[5]);
    const uint32_t* component = Find(out, chain[6]);
    Check(element && element[3] == me::native::kSharedWordFltMin / 4, "A: element 30");
    Check(component && component[3] == expected_component[k], "A: components 2 and 3");
  }
  // The renderer writes exactly the constants the shader had.
  Check(Find(in, 17)[3] == me::native::kFltMinBits && Find(in, 18)[3] == me::native::kFltMaxBits, "A: same bits");
  Validate(out, "a");
  // Nothing to do without clamp bounds: unchanged.
  Words no_bounds = in;
  for (size_t at = 5; at < no_bounds.size(); at += no_bounds[at] >> 16)
    if ((no_bounds[at] & 0xFFFF) == 12 && no_bounds[at + 2] == 83) no_bounds[at + 6] = no_bounds[at + 7] = 16;
  Check(me::native::ReadFltBoundsFromShared(no_bounds, out, stats, reason) && out == no_bounds && !stats.replaced,
        "A: unchanged without bounds");
}

static void TestB() {
  const Words in = Fixture(false);
  Words out;
  me::native::CombinedFetchStats stats;
  std::string reason;
  Check(me::native::CombineTexture2DFetches(in, out, stats, reason), "B failed");
  Check(stats.chains == 1 && stats.combined == 1 && stats.separate == 1, "B: one combined, one separate");
  // The combined heap variable at set 5 binding 0, listed in the entry point (SPIR-V 1.5).
  uint32_t combined = 0;
  for (size_t at = 5; at < out.size(); at += out[at] >> 16)
    if ((out[at] & 0xFFFF) == 59 && DecorationValue(out, out[at + 2], 34) == 5) combined = out[at + 2];
  Check(combined && DecorationValue(out, combined, 33) == 0, "B: set 5 binding 0");
  bool listed = false;
  for (size_t at = 5; at < out.size(); at += out[at] >> 16)
    if ((out[at] & 0xFFFF) == 15)
      for (size_t k = 5; k < (out[at] >> 16); ++k) listed |= out[at + k] == combined;
  Check(listed, "B: in the interface");
  // The 2D chain indexes the combined heap with the SAME index id (2D word 2 & 0xFFFFFF).
  const uint32_t* chain = Find(out, 65);
  Check(chain && chain[3] == combined && chain[4] == 64, "B: same index into the combined heap");
  // First sample: the combined descriptor itself (copy of the combined load of that chain).
  const uint32_t* copy = Find(out, 72);
  Check(copy && (copy[0] & 0xFFFF) == 83, "B: matching pair combined");
  const uint32_t* load = Find(out, copy[3]);
  Check(load && (load[0] & 0xFFFF) == 61 && load[3] == 65 && load[1] == 32, "B: load of the combined descriptor");
  // The image loads are OpImage of the combined loads (size queries see the same image).
  const uint32_t* image = Find(out, 66);
  Check(image && (image[0] & 0xFFFF) == 100 && image[3] == copy[3], "B: OpImage of the combined descriptor");
  // Second sample: sampler of another register: still OpSampledImage(OpImage(combined), separate sampler 78).
  const uint32_t* separate = Find(out, 80);
  Check(separate && (separate[0] & 0xFFFF) == 86 && separate[4] == 78, "B: other sampler kept");
  const uint32_t* image2 = Find(out, 79);
  Check(image2 && (image2[0] & 0xFFFF) == 100, "B: second image from the combined heap");
  Check(Count(out, 87) == 2, "B: both samples kept");
  Validate(out, "b");

  // Refusal: a 2D word used for something else than the sign byte or a 2D index.
  Check(!me::native::CombineTexture2DFetches(Fixture(true), out, stats, reason) && out.empty(), "B: refuses other uses");
}

static void TestC() {
  const Words in = Fixture(false);
  // Register 2: words 2, 18, 34 (2D, 3D, cube). Signs folded first, as the renderer does.
  const uint64_t known = (uint64_t(1) << 2) | (uint64_t(1) << 18) | (uint64_t(1) << 34);
  uint8_t signs[48] = {};
  me::native::TextureSignsFold fold;
  std::string reason;
  Words folded;
  Check(me::native::FoldTextureSigns(in, signs, known, folded, fold, reason) && fold.folded == 1, "signs fold");
  Words out;
  me::native::IndexMaskStats stats;
  Check(me::native::DropIndexMasks(folded, known, out, stats, reason), "C failed");
  Check(stats.removed == 1, "C: one mask");
  const uint32_t* index = Find(out, 64);
  Check(index && (index[0] & 0xFFFF) == 83 && index[3] == 62, "C: the index is the word itself");
  Validate(out, "c");
  // C then B (the renderer's order): still combined.
  Words both;
  me::native::CombinedFetchStats combined;
  Check(me::native::CombineTexture2DFetches(out, both, combined, reason) && combined.combined == 1, "C then B");
  Words all;
  me::native::FltBoundsStats bounds;
  Check(me::native::ReadFltBoundsFromShared(both, all, bounds, reason) && bounds.replaced == 2, "C, B then A");
  Validate(all, "cba");
  // Refusal: without the signs fold the sign byte is still read: the cleared byte would change the image.
  Check(!me::native::DropIndexMasks(in, known, out, stats, reason) && out.empty(), "C: refuses unfolded signs");
  // A word that is not cleared keeps its mask.
  Check(me::native::DropIndexMasks(folded, uint64_t(1) << 3, out, stats, reason) && !stats.removed && out == folded,
        "C: other words untouched");
}

static int Library(const char* folder) {
  size_t n = 0, changed = 0;
  for (const auto& file : std::filesystem::directory_iterator(folder)) {
    if (file.path().extension() != ".spv") continue;
    std::ifstream f(file.path(), std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    Words in(bytes.size() / 4);
    std::memcpy(in.data(), bytes.data(), in.size() * 4);
    uint8_t signs[48] = {};
    const uint64_t known = (uint64_t(1) << 48) - 1;
    me::native::TextureSignsFold fold;
    std::string reason;
    Words s, c, b, a;
    if (!me::native::FoldTextureSigns(in, signs, known, s, fold, reason)) throw std::runtime_error(reason);
    me::native::IndexMaskStats ms;
    me::native::CombinedFetchStats cs;
    me::native::FltBoundsStats fs;
    if (!me::native::DropIndexMasks(s, known, c, ms, reason)) {
      std::printf("  %s: C refused: %s\n", file.path().filename().c_str(), reason.c_str());
      c = s;
    }
    if (!me::native::CombineTexture2DFetches(c, b, cs, reason)) {
      std::printf("  %s: B refused: %s\n", file.path().filename().c_str(), reason.c_str());
      b = c;
    }
    if (!me::native::ReadFltBoundsFromShared(b, a, fs, reason)) throw std::runtime_error(reason);
    const std::string name = "lib_" + file.path().stem().string();
    Validate(a, name.c_str());
    ++n;
    changed += a != in;
  }
  std::printf("library: %zu modules, %zu rewritten, all valid%s\n", n, changed, g_validate ? "" : " (not checked: no spirv-val)");
  return 0;
}

int main() {
  try {
    g_validate = std::system("command -v spirv-val >/dev/null 2>&1") == 0;
    g_folder = std::filesystem::temp_directory_path() / "me_ps_descriptors_test";
    std::filesystem::create_directories(g_folder);
    TestA();
    TestB();
    TestC();
    if (const char* folder = std::getenv("ME_PS_LIBRARY_DIR")) Library(folder);
    std::printf("ok (spirv-val %s)\n", g_validate ? "checked" : "not found: structure only");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
