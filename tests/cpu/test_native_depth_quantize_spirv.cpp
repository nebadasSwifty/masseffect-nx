#include "me_depth_quantize_spirv.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace me::native;
using Words = std::vector<uint32_t>;
static void Emit(Words& w, uint32_t op, std::initializer_list<uint32_t> args) {
  w.push_back((uint32_t(args.size() + 1) << 16) | op);
  w.insert(w.end(), args);
}
static Words Fixture(bool coord, bool multiple_returns = false, bool kill = false) {
  Words w = {0x07230203, 0x00010300, 0, 100, 0};
  Emit(w, 17, {1});
  Emit(w, 14, {0, 1});
  if (coord) Emit(w, 15, {4, 20, 0x6E69616D, 0, 8});
  else Emit(w, 15, {4, 20, 0x6E69616D, 0});
  Emit(w, 16, {20, 7});
  Emit(w, 16, {20, 9});
  if (coord) Emit(w, 71, {8, 11, 15});
  Emit(w, 19, {1}); Emit(w, 33, {2, 1}); Emit(w, 22, {3, 32});
  Emit(w, 21, {4, 32, 0}); Emit(w, 20, {5}); Emit(w, 23, {6, 3, 4});
  Emit(w, 32, {7, 1, 6}); Emit(w, 41, {5, 11});
  if (coord) Emit(w, 59, {7, 8, 1});
  Emit(w, 54, {1, 20, 0, 2}); Emit(w, 248, {21});
  if (multiple_returns || kill) {
    Emit(w, 247, {24, 0}); Emit(w, 250, {11, 22, 23});
    Emit(w, 248, {22}); Emit(w, kill ? 252 : 253, {});
    Emit(w, 248, {23}); Emit(w, 253, {});
    Emit(w, 248, {24}); Emit(w, 255, {});
  } else Emit(w, 253, {});
  Emit(w, 56, {});
  Emit(w, 54, {1, 30, 0, 2}); Emit(w, 248, {31}); Emit(w, 253, {}); Emit(w, 56, {});
  return w;
}
static Words ExistingDepthFixture(bool read_pointer = false, uint32_t depth_id = 10) {
  Words w = {0x07230203, 0x00010300, 0, 100, 0};
  Emit(w, 17, {1}); Emit(w, 14, {0, 1});
  Emit(w, 15, {4, 20, 0x6E69616D, 0, depth_id});
  Emit(w, 16, {20, 7}); Emit(w, 16, {20, 9}); Emit(w, 16, {20, 12});
  Emit(w, 71, {depth_id, 11, 22});
  Emit(w, 19, {1}); Emit(w, 33, {2, 1}); Emit(w, 22, {3, 32});
  Emit(w, 32, {9, 3, 3}); Emit(w, 43, {3, 12, 0x3E800003});
  Emit(w, 59, {9, depth_id, 3});
  Emit(w, 54, {1, 20, 0, 2}); Emit(w, 248, {21});
  if (read_pointer) Emit(w, 61, {3, 42, depth_id});
  Emit(w, 62, {depth_id, 12}); Emit(w, 253, {}); Emit(w, 56, {});
  Emit(w, 54, {1, 30, 0, 2}); Emit(w, 248, {31});
  Emit(w, 62, {depth_id, 12}); Emit(w, 253, {}); Emit(w, 56, {});
  return w;
}

// Independent execution of the emitted scalar SPIR-V operations for the simple
// entry fixture. Tests the actual code-generation opcodes/constants, not only
// the CPU contract. Existing arbitrary guest instructions are not interpreted.
static float ExecuteQuantizer(const Words& w, float host) {
  std::vector<uint32_t> values(w[3]);
  uint32_t function = 0;
  for (size_t at = 5; at < w.size();) {
    const auto* i = w.data() + at;
    const uint32_t wc = i[0] >> 16, op = i[0] & 65535;
    const auto f = [&](uint32_t id) { return std::bit_cast<float>(values.at(id)); };
    if (op == 43 && wc == 4) values.at(i[2]) = i[3];
    if (op == 54) function = i[2];
    if (function == 20) {
      switch (op) {
        case 61: values.at(i[2]) = 0; break;
        case 81: assert(i[4] == 2); values.at(i[2]) = std::bit_cast<uint32_t>(host); break;
        case 112: values.at(i[2]) = std::bit_cast<uint32_t>(float(values.at(i[3]))); break;
        case 124: values.at(i[2]) = values.at(i[3]); break;
        case 128: values.at(i[2]) = values.at(i[3]) + values.at(i[4]); break;
        case 130: values.at(i[2]) = values.at(i[3]) - values.at(i[4]); break;
        case 133: values.at(i[2]) = std::bit_cast<uint32_t>(f(i[3]) * f(i[4])); break;
        case 169: values.at(i[2]) = values.at(i[3]) ? values.at(i[4]) : values.at(i[5]); break;
        case 174: values.at(i[2]) = values.at(i[3]) >= values.at(i[4]); break;
        case 176: values.at(i[2]) = values.at(i[3]) < values.at(i[4]); break;
        case 186: values.at(i[2]) = f(i[3]) > f(i[4]); break;
        case 194: assert(values.at(i[4]) < 32); values.at(i[2]) = values.at(i[3]) >> values.at(i[4]); break;
        case 196: assert(values.at(i[4]) < 32); values.at(i[2]) = values.at(i[3]) << values.at(i[4]); break;
        case 197: values.at(i[2]) = values.at(i[3]) | values.at(i[4]); break;
        case 199: values.at(i[2]) = values.at(i[3]) & values.at(i[4]); break;
        case 62: return f(i[2]);
        default: break;
      }
    }
    at += wc;
  }
  assert(false); return 0;
}

static void CheckStructure(const Words& w, uint32_t stores, uint32_t kills, bool helper_stores = false) {
  uint32_t function = 0, count = 0, kill_count = 0, replacing = 0;
  for (size_t at = 5; at < w.size();) {
    const auto* i = w.data() + at; const uint32_t wc = i[0] >> 16, op = i[0] & 65535;
    assert(wc && wc <= w.size() - at);
    if (op == 54) function = i[2];
    if (op == 56) function = 0;
    if (op == 62) { assert(function == 20 || (helper_stores && function == 30)); ++count; }
    if (op == 252) ++kill_count;
    if (op == 16 && i[2] == 9) assert(false);
    if (op == 16 && i[2] == 12) ++replacing;
    at += wc;
  }
  assert(count == stores && kill_count == kills && replacing == 1);
}

static void Save(const std::filesystem::path& path, const Words& words) {
  std::ofstream f(path, std::ios::binary); assert(f);
  f.write(reinterpret_cast<const char*>(words.data()), std::streamsize(words.size() * 4)); assert(f);
}
static size_t FindOpcode(const Words& w, uint32_t wanted) {
  for (size_t at = 5; at < w.size(); at += w[at] >> 16)
    if ((w[at] & 65535) == wanted) return at;
  assert(false); return w.size();
}

static void CheckNative2xStructure(const Words& input, const Words& output, uint32_t stores,
                                 uint32_t kills) {
  CheckStructure(output, stores, kills);
  uint32_t function = 0, depth = 0, coord = 0, sample = 0;
  uint32_t fine_x = 0, fine_y = 0, sample_cap = 0, derivative_cap = 0, precise = 0;
  uint32_t coord_loads = 0, sample_loads = 0;
  bool first_label = false, body_seen = false, divergent = false;
  uint32_t original_coord = 0, original_coord_loads = 0;
  for (size_t at = 5; at < input.size(); at += input[at] >> 16) {
    const auto* w = input.data() + at;
    if ((w[0] & 65535) == 71 && (w[0] >> 16) == 4 && w[2] == 11 && w[3] == 15)
      original_coord = w[1];
    if (original_coord && (w[0] & 65535) == 61 && w[3] == original_coord) ++original_coord_loads;
  }
  for (size_t at = 5; at < output.size();) {
    const auto* w = output.data() + at;
    const uint32_t wc = w[0] >> 16, op = w[0] & 65535;
    if (op == 71 && wc == 4 && w[2] == 11) {
      if (w[3] == 22) depth = w[1];
      if (w[3] == 15) coord = w[1];
      if (w[3] == 18) sample = w[1];
    }
    if (op == 71 && wc == 3 && w[2] == 42) ++precise;
    if (op == 17 && wc == 2 && w[1] == 35) ++sample_cap;
    if (op == 17 && wc == 2 && w[1] == 51) ++derivative_cap;
    if (op == 54) function = w[2];
    if (op == 56) function = 0;
    if (function == 20) {
      if (op == 248 && !first_label) first_label = true;
      else if (first_label && op != 8 && op != 317) {
        if (op == 59) assert(!body_seen);
        else body_seen = true;
      }
      if (op == 249 || op == 250 || op == 251 || op == 252 || op == 253) divergent = true;
      if (op == 61 && w[3] == coord) { if (!coord_loads) assert(!divergent); ++coord_loads; }
      if (op == 61 && w[3] == sample) { assert(!divergent); ++sample_loads; }
      if (op == 210 || op == 211) {
        assert(!divergent);
        if (op == 210) ++fine_x;
        else ++fine_y;
      }
    }
    at += wc;
  }
  assert(depth && coord && sample && fine_x == 1 && fine_y == 1 && sample_cap == 1 &&
         derivative_cap == 1 && precise == 4 && coord_loads == 1 + original_coord_loads && sample_loads == 1);
  // Every original instruction survives unchanged except entry interfaces and
  // the intentional removal of EarlyFragmentTests. No material FragCoord read
  // or variable is replaced; injected arithmetic is a separate SSA prologue.
  size_t output_at = 5;
  for (size_t input_at = 5; input_at < input.size();) {
    const uint32_t wc = input[input_at] >> 16, op = input[input_at] & 65535;
    if (op != 15 && !(op == 16 && input[input_at + 2] == 9)) {
      bool found = false;
      while (output_at < output.size()) {
        const uint32_t out_wc = output[output_at] >> 16;
        if (wc == out_wc && std::equal(input.begin() + input_at, input.begin() + input_at + wc,
                                      output.begin() + output_at)) {
          found = true; output_at += out_wc; break;
        }
        output_at += out_wc;
      }
      assert(found);
    }
    input_at += wc;
  }
}

int main(int argc, char** argv) {
  std::string reason; Words out;
  for (bool round : {false, true}) {
    assert(TransformDepthQuantizeIncoming(Fixture(false), out, reason, round));
    CheckStructure(out, 1, 0);
    if (argc > 1) Save(std::filesystem::path(argv[1]) / (round ? "round.spv" : "truncate.spv"), out);
    // All 16,777,216 exact FLOAT24 encodings survive both modes bitwise.
    for (uint32_t code = 0; code < 0x1000000; ++code) {
      const float host = DecodeDepthFloat20e4(code) * 0.5f;
      assert(std::bit_cast<uint32_t>(QuantizeIncomingDepthCPU(host, round)) == std::bit_cast<uint32_t>(host));
    }
    const std::array<uint32_t, 10> boundaries = {0, 1, 0x00800000, 0x2E000000,
        0x2E800000, 0x38800000, 0x3F800000, 0x3FFFFFF8, 0x7F800000, 0x7FC00000};
    for (uint32_t bits : boundaries) for (int32_t offset = -16; offset <= 16; ++offset) {
      const float host = std::bit_cast<float>(bits + uint32_t(offset)) * 0.5f;
      assert(std::bit_cast<uint32_t>(ExecuteQuantizer(out, host)) ==
             std::bit_cast<uint32_t>(QuantizeIncomingDepthCPU(host, round)));
    }
    for (uint32_t code = 1; code < 0x1000000; code += 4093) {
      const float host = DecodeDepthFloat20e4(code) * 0.5f;
      assert(std::bit_cast<uint32_t>(ExecuteQuantizer(out, host)) == std::bit_cast<uint32_t>(host));
    }
  }
  assert(TransformDepthQuantizeIncoming(Fixture(true, true), out, reason));
  CheckStructure(out, 2, 0);
  if (argc > 1) Save(std::filesystem::path(argv[1]) / "multiple-returns.spv", out);
  assert(TransformDepthQuantizeIncoming(Fixture(true, false, true), out, reason));
  CheckStructure(out, 1, 1);
  if (argc > 1) Save(std::filesystem::path(argv[1]) / "kill.spv", out);
  Words rejected;
  assert(TransformDepthQuantizeIncoming(ExistingDepthFixture(), out, reason));
  CheckStructure(out, 2, 0, true);
  assert(std::bit_cast<uint32_t>(ExecuteQuantizer(out, 0)) ==
         std::bit_cast<uint32_t>(QuantizeDepthHalfCpu(std::bit_cast<float>(0x3E800003u))));
  if (argc > 1) Save(std::filesystem::path(argv[1]) / "existing-depth.spv", out);
  assert(!TransformDepthQuantizeIncoming(ExistingDepthFixture(true), rejected, reason));
  assert(rejected.empty() && reason == "FragDepth pointer read/escape/derived use unsupported");
  // Package regression: 23 valid PS had FragDepth ID7 and Function-storage
  // locals. StorageClass literal7 is not an alias/read of pointer ID7.
  auto literal_collision = ExistingDepthFixture(false, 7);
  literal_collision.insert(literal_collision.begin() + FindOpcode(literal_collision, 54),
                            {(4u << 16) | 32, 43, 7, 3});
  const size_t collision_local_at = FindOpcode(literal_collision, 248) + 2;
  literal_collision.insert(literal_collision.begin() + collision_local_at,
                            {(4u << 16) | 59, 43, 44, 7});
  assert(TransformDepthQuantizeIncoming(literal_collision, out, reason, true, true));
  CheckStructure(out, 2, 0, true);
  if (argc > 1) Save(std::filesystem::path(argv[1]) / "explicit-depth-id7-local.spv", out);
  auto initializer_escape = literal_collision;
  initializer_escape[collision_local_at] += 1u << 16;
  initializer_escape.insert(initializer_escape.begin() + collision_local_at + 4, 7);
  assert(!TransformDepthQuantizeIncoming(initializer_escape, rejected, reason));
  assert(rejected.empty() && reason == "FragDepth pointer read/escape/derived use unsupported");
  assert(TransformDepthQuantizeIncoming(MakeDepthOnlyFragmentForQuantization(), out, reason));
  if (argc > 1) Save(std::filesystem::path(argv[1]) / "synthetic-depth-only.spv", out);
  auto malformed = Fixture(false); malformed[5] = 0;
  assert(!TransformDepthQuantizeIncoming(malformed, rejected, reason));
  auto alias = Fixture(false); assert(!TransformDepthQuantizeIncoming(alias, alias, reason));
  for (bool coord : {false, true}) for (bool round : {false, true}) {
    auto input = Fixture(coord, true, true);
    if (coord) {
      Words material_reads;
      Emit(material_reads, 61, {6, 51, 8});
      Emit(material_reads, 81, {3, 52, 51, 0});
      Emit(material_reads, 81, {3, 53, 51, 1});
      Emit(material_reads, 81, {3, 54, 51, 2});
      input.insert(input.begin() + FindOpcode(input, 248) + 2,
                   material_reads.begin(), material_reads.end());
    }
    assert(TransformDepthQuantizeIncoming(input, out, reason, round, true));
    CheckNative2xStructure(input, out, 1, 1);
    if (argc > 1) Save(std::filesystem::path(argv[1]) /
        ("native2x-" + std::to_string(coord) + "-" + std::to_string(round) + ".spv"), out);
  }
  // Existing explicit guest depth follows the same per-fragment transform.
  Words explicit_single;
  assert(TransformDepthQuantizeIncoming(ExistingDepthFixture(), explicit_single, reason, true));
  assert(TransformDepthQuantizeIncoming(ExistingDepthFixture(), out, reason, true, true));
  assert(out == explicit_single);
  // Preserve function-local variable placement and signed SampleId imports.
  auto local = Fixture(true);
  local.insert(local.begin() + FindOpcode(local, 54), {(4u << 16) | 32, 43, 7, 3});
  local.insert(local.begin() + FindOpcode(local, 248) + 2, {(4u << 16) | 59, 43, 42, 7});
  assert(TransformDepthQuantizeIncoming(local, out, reason, false, true));
  CheckNative2xStructure(local, out, 1, 0);
  if (argc > 1) Save(std::filesystem::path(argv[1]) / "native2x-local.spv", out);
  auto imported_sample = Fixture(true);
  size_t interface_at = FindOpcode(imported_sample, 15);
  imported_sample.insert(imported_sample.begin() + interface_at + 6, 46);
  imported_sample[interface_at] += 1u << 16;
  imported_sample.insert(imported_sample.begin() + FindOpcode(imported_sample, 19),
                         {(4u << 16) | 71, 46, 11, 18, (3u << 16) | 71, 46, 14});
  Words sample_declarations;
  Emit(sample_declarations, 21, {44, 32, 1});
  Emit(sample_declarations, 32, {45, 1, 44});
  Emit(sample_declarations, 59, {45, 46, 1});
  imported_sample.insert(imported_sample.begin() + FindOpcode(imported_sample, 54),
                         sample_declarations.begin(), sample_declarations.end());
  Words import;
  Emit(import, 11, {47, 0x4C534C47, 0x6474732E, 0x3035342E, 0});
  imported_sample.insert(imported_sample.begin() + FindOpcode(imported_sample, 14),
                         import.begin(), import.end());
  // The original module already declares SampleRateShading. Do not duplicate.
  imported_sample.insert(imported_sample.begin() + FindOpcode(imported_sample, 11),
                         {(2u << 16) | 17, 35});
  assert(TransformDepthQuantizeIncoming(imported_sample, out, reason, true, true));
  CheckNative2xStructure(imported_sample, out, 1, 0);
  if (argc > 1) Save(std::filesystem::path(argv[1]) / "native2x-signed-import.spv", out);
  for (int arg = 2; arg < argc; ++arg) {
    std::ifstream f(argv[arg], std::ios::binary | std::ios::ate); assert(f && f.tellg() > 0);
    const size_t bytes = size_t(f.tellg()); assert(!(bytes & 3));
    Words real(bytes / 4); f.seekg(0); f.read(reinterpret_cast<char*>(real.data()), std::streamsize(bytes)); assert(f);
    assert(TransformDepthQuantizeIncoming(real, out, reason));
    Save(std::filesystem::path(argv[1]) / ("real-" + std::to_string(arg) + ".spv"), out);
    assert(TransformDepthQuantizeIncoming(real, out, reason, true, true));
    Save(std::filesystem::path(argv[1]) / ("real-native2x-" + std::to_string(arg) + ".spv"), out);
  }
  std::puts("native incoming-depth FLOAT24 quantizer: PASS (all codes, both modes; bounded native2x structure)");
}
