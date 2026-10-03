#include "me_depth_spirv.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

using Words = std::vector<uint32_t>;
static void Emit(Words& w, uint32_t op, std::initializer_list<uint32_t> args) {
  w.push_back((uint32_t(args.size()+1) << 16) | op); w.insert(w.end(), args);
}
static void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static uint32_t Count(const Words& w, uint32_t op) {
  uint32_t result = 0;
  for (size_t at = 5; at < w.size(); at += w[at] >> 16) if ((w[at] & 65535) == op) ++result;
  return result;
}
// IDs: void1 fn2 f323 vec44 Inputvecptr5 Outputfptr6 Inputfptr7 uint8
// zIndex9 zero10 bool11 true12; FragCoord13 FragDepth14 entry15.
static Words Fixture(unsigned kind, bool mode = true) {
  Words w{0x07230203, 0x00010400, 0, 100, 0};
  Emit(w, 17, {1});
  if (kind == 2 || kind == 6) Emit(w, 17, {4442});
  Emit(w, 14, {0, 1});
  Emit(w, 15, {4, 15, 0x6E69616D, 0, 13, 14});
  Emit(w, 16, {15, 7}); if (mode) Emit(w, 16, {15, 12});
  Emit(w, 71, {13, 11, 15}); Emit(w, 71, {14, 11, 22});
  Emit(w, 19, {1}); Emit(w, 33, {2, 1}); Emit(w, 22, {3, 32});
  Emit(w, 23, {4, 3, 4}); Emit(w, 32, {5, 1, 4}); Emit(w, 32, {6, 3, 3});
  Emit(w, 32, {7, 1, 3}); Emit(w, 21, {8, 32, 0}); Emit(w, 43, {8, 9, kind == 5 ? 0u : 2u});
  Emit(w, 43, {3, 10, 0}); Emit(w, 20, {11}); Emit(w, 41, {11, 12});
  Emit(w, 59, {5, 13, 1}); Emit(w, 59, {6, 14, 3});
  Emit(w, 54, {1, 15, 0, 2}); Emit(w, 248, {16});
  if (kind == 0) {
    Emit(w, 61, {4, 17, 13}); Emit(w, 81, {3, 18, 17, 2});
    Emit(w, 62, {14, 18}); Emit(w, 253, {});
  } else if (kind == 1 || kind == 2 || kind == 5) {
    Emit(w, 65, {7, 17, 13, 9}); Emit(w, 83, {7, 18, 17});
    if (kind == 2) Emit(w, 169, {7, 19, 12, 17, 18});
    Emit(w, 61, {3, 20, kind == 2 ? 19u : 18u}); Emit(w, 62, {14, 20}); Emit(w, 253, {});
  } else if (kind == 3) {
    Emit(w, 247, {30, 0}); Emit(w, 250, {12, 21, 22});
    Emit(w, 248, {21}); Emit(w, 62, {14, 10}); Emit(w, 253, {});
    Emit(w, 248, {22}); Emit(w, 247, {29, 0}); Emit(w, 250, {12, 23, 24});
    Emit(w, 248, {23}); Emit(w, 252, {});
    Emit(w, 248, {24}); Emit(w, 62, {14, 10}); Emit(w, 253, {});
    Emit(w, 248, {29}); Emit(w, 255, {}); Emit(w, 248, {30}); Emit(w, 255, {});
  } else if (kind == 4) {
    Emit(w, 62, {14, 10}); Emit(w, 61, {3, 17, 14}); Emit(w, 62, {14, 17}); Emit(w, 253, {});
  } else if (kind == 6) {
    Emit(w, 65, {7, 17, 13, 9}); Emit(w, 247, {23, 0}); Emit(w, 250, {12, 21, 22});
    Emit(w, 248, {21}); Emit(w, 249, {23}); Emit(w, 248, {22}); Emit(w, 249, {23});
    Emit(w, 248, {23}); Emit(w, 245, {7, 18, 17, 21, 17, 22});
    Emit(w, 61, {3, 19, 18}); Emit(w, 62, {14, 19}); Emit(w, 253, {});
  }
  Emit(w, 56, {}); return w;
}
static Words Read(const char* name) {
  std::ifstream file(name, std::ios::binary | std::ios::ate); const auto bytes = file.tellg();
  Check(bytes >= 20 && bytes % 4 == 0, "invalid input file"); Words w(size_t(bytes)/4);
  file.seekg(0); file.read(reinterpret_cast<char*>(w.data()), bytes); Check(bool(file), "read failed"); return w;
}
static void Write(const std::string& path, const Words& words) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(words.data()), words.size()*4); Check(bool(file), "write failed");
}
int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string(argv[1]) != "--fixtures") {
      const auto input = Read(argv[1]); Words output; std::string reason;
      if (!me::native::TransformDepthHalf(input, output, reason)) { std::cerr << reason << '\n'; return 1; }
      Write(argv[2], output); std::cout << "transformed " << input.size() << " -> " << output.size() << " words\n"; return 0;
    }
    for (unsigned kind = 0; kind <= 6; ++kind) {
      const auto input = Fixture(kind); Words output; std::string reason;
      // Vulkan permits variable pointers only to StorageBuffer / Workgroup,
      // not these Input pointers. Treat merges as an explicit rejection test.
      if (kind == 2 || kind == 6) {
        Check(!me::native::TransformDepthHalf(input, output, reason) && output.empty(), "Input pointer merge accepted");
        continue;
      }
      Check(me::native::TransformDepthHalf(input, output, reason), reason.c_str());
      Check(output[3] > input[3], "fresh IDs not allocated");
      Check(Count(output, 252) == Count(input, 252) && Count(output, 253) == Count(input, 253), "kill/returns changed");
      Check(Count(output, 133) == (kind == 4 ? 3u : kind == 5 ? 1u : 2u), "wrong scale count");
      Check(Count(output, 82) == (kind == 0 ? 1u : 0u), "XYW preservation reconstruction");
      if (argc == 3 && std::string(argv[1]) == "--fixtures") {
        Write(std::string(argv[2])+"/depth-half-"+std::to_string(kind)+"-input.spv", input);
        Write(std::string(argv[2])+"/depth-half-"+std::to_string(kind)+".spv", output);
      }
    }
    Words output; std::string reason; auto missing_mode = Fixture(0, false);
    Check(me::native::TransformDepthHalf(missing_mode, output, reason), reason.c_str());
    Check(Count(output, 16) == Count(missing_mode, 16)+1, "DepthReplacing not added");
    if (argc == 3) Write(std::string(argv[2])+"/depth-half-added-mode.spv", output);
    auto dynamic = Fixture(1);
    for (size_t at = 5; at < dynamic.size(); at += dynamic[at] >> 16)
      if ((dynamic[at]&65535) == 65) { dynamic[at+4] = 20; break; }
    Check(!me::native::TransformDepthHalf(dynamic, output, reason) && output.empty(), "dynamic pointer accepted");
    auto escape = Fixture(1);
    for (size_t at = 5; at < escape.size(); at += escape[at] >> 16)
      if ((escape[at]&65535) == 61) { escape[at] = (4u<<16)|124; break; }
    Check(!me::native::TransformDepthHalf(escape, output, reason) && output.empty(), "pointer escape accepted");
    auto block = Fixture(0); block.insert(block.begin()+5, {(5u<<16)|72, 4, 0, 11, 15});
    Check(!me::native::TransformDepthHalf(block, output, reason), "BuiltIn block accepted");
    auto malformed = Fixture(0); malformed.back() = 0;
    Check(!me::native::TransformDepthHalf(malformed, output, reason), "malformed module accepted");
    std::cout << "depth SPIR-V tests passed\n"; return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
