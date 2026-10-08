// Collect the game's shader containers for the shader library (debug aid: only active when the environment variable
// below is set, which a normal Switch run never does).
//
// Unreal Engine 3 builds its Direct3D shader objects in place from its shader cache, so
// the original containers are not in any single file and never pass through a Direct3D
// creation call. Each object keeps a copy of the virtual part of
// its container (VS at object+0x368, PS at object+0x28) and points to the microcode
// (VS [object+0x20], PS [object+0x18]). Direct3D patches the vertex shader microcode in
// place the first time it binds the shader to a vertex declaration, in sub_8222F850
// (r3 = VS, r7 = PS). So the containers are copied here, before that call runs.
//
// With MASSEFFECT_SHADER_DUMP=<folder>, each distinct container is written once as
// <folder>/{vs,ps}_<hash>.bin = virtual part + microcode, the 2008 layout XenosRecomp
// reads. The files are game data: they stay on this machine (run/, not in git).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "native/me_native_system.h"

// The Switch has no environment variables: set this in masseffect.toml instead (for example
// masseffect_shader_dump_dir = "/switch/masseffect-nx/shader_dump"). It does the same as MASSEFFECT_SHADER_DUMP.
REXCVAR_DEFINE_STRING(masseffect_shader_dump_dir, "", "Mass Effect",
                      "Debug aid: write the game's shader containers (also the ones it builds at run time, e.g. menu and HUD "
                      "shaders) into this folder; the same as the MASSEFFECT_SHADER_DUMP environment variable")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

uint64_t Hash(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

std::mutex g_mutex;
std::unordered_set<uint32_t> g_objects;  // shader objects already handled
std::unordered_set<uint64_t> g_written;  // container hashes already on disk
std::string g_folder;
bool g_enabled = false;

void Init() {
  const char* folder = std::getenv("MASSEFFECT_SHADER_DUMP");
  std::string from_cvar;
  if (!folder || !*folder) {
    from_cvar = REXCVAR_GET(masseffect_shader_dump_dir);
    if (from_cvar.empty()) return;
    folder = from_cvar.c_str();
  }
  g_folder = folder;
  std::error_code ec;
  std::filesystem::create_directories(g_folder, ec);
  g_enabled = !ec;
}

// A function-local static instead of std::call_once: on this toolchain call_once takes a global lock
// (pthread_once has no fast path), and this runs on every hooked D3D call.
inline void EnsureInit() {
  // Not a function-local static: its guard is a load-acquire on every call (this runs on every shader bind).
  static std::atomic<bool> done{false};
  if (done.load(std::memory_order_relaxed)) [[likely]] return;
  static std::mutex m;
  std::lock_guard<std::mutex> lock(m);
  if (!done.load(std::memory_order_relaxed)) {
    Init();
    done.store(true, std::memory_order_release);
  }
}

// type 6 = vertex shader, 7 = pixel shader (word 0 of the object).
void Dump(const uint8_t* base, uint32_t object, bool vertex) {
  if (object < 0x40000000 || object >= 0x7F000000) return;
  if ((Load32(base, object) & 0xFF) != (vertex ? 6u : 7u)) return;
  if (!g_objects.insert(object).second) return;
  const uint32_t header = object + (vertex ? 0x368 : 0x28);
  const uint32_t signature = Load32(base, header);
  const uint32_t virtual_size = Load32(base, header + 4);
  const uint32_t physical_size = Load32(base, header + 8);
  const uint32_t microcode = Load32(base, object + (vertex ? 0x20 : 0x18));
  if ((signature & 0xFFFFFF00u) != 0x102A1100u || virtual_size < 24 || virtual_size > 0x40000 ||
      physical_size == 0 || physical_size > 0x40000 || microcode < 0xA0000000u) {
    std::fprintf(stderr, "[shader dump] %s %08X: unexpected container %08X %u %u %08X\n",
                 vertex ? "VS" : "PS", object, signature, virtual_size, physical_size, microcode);
    return;
  }
  std::vector<uint8_t> container(virtual_size + physical_size);
  std::memcpy(container.data(), base + header, virtual_size);
  std::memcpy(container.data() + virtual_size, base + microcode, physical_size);
  const uint64_t hash = Hash(container.data(), container.size());
  if (!g_written.insert(hash).second) return;
  char name[64];
  std::snprintf(name, sizeof(name), "/%s_%016llx.bin", vertex ? "vs" : "ps",
                static_cast<unsigned long long>(hash));
  const std::string path = g_folder + name;
  if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(container.data(), 1, container.size(), f);
    std::fclose(f);
  }
}

// A whole container that is contiguous in guest memory (Direct3D's own shaders, parsed at device
// creation by sub_8221E298 with r4 = container).
void DumpContainer(const uint8_t* base, uint32_t container) {
  if (container < 0x40000000 || container >= 0x7F000000) return;
  const uint32_t signature = Load32(base, container);
  if ((signature & 0xFFFFFF00u) != 0x102A1100u) return;
  const uint32_t virtual_size = Load32(base, container + 4);
  const uint32_t physical_size = Load32(base, container + 8);
  if (virtual_size < 24 || virtual_size > 0x40000 || !physical_size || physical_size > 0x40000) return;
  const uint8_t* p = base + container;
  const size_t n = size_t(virtual_size) + physical_size;
  const uint64_t hash = Hash(p, n);
  if (!g_written.insert(hash).second) return;
  char name[64];
  std::snprintf(name, sizeof(name), "/%s_%016llx.bin", (signature & 1) ? "vs" : "ps",
                static_cast<unsigned long long>(hash));
  if (std::FILE* f = std::fopen((g_folder + name).c_str(), "wb")) {
    std::fwrite(p, 1, n, f);
    std::fclose(f);
  }
}

// MASSEFFECT_SHADER_LINK_LOG=1: for vertex shaders without a library entry, the pixel shader they are
// bound with (its interpolator table) and the vertex shader microcode after Direct3D patched it for that
// pair, once per pair. Shows how Direct3D links exports and fetches of its own shaders.
void LogLink(const uint8_t* base, uint32_t vs, uint32_t ps) {
  // Plain relaxed atomic instead of a function-local static (no load-acquire guard per call).
  static std::atomic<int> state{-1};
  int e = state.load(std::memory_order_relaxed);
  if (e < 0) {
    const char* v = std::getenv("MASSEFFECT_SHADER_LINK_LOG");
    e = (v && *v == '1') ? 1 : 0;
    state.store(e, std::memory_order_relaxed);
  }
  const bool enabled = e == 1;
  if (!enabled || vs < 0x40000000 || vs >= 0x7F000000 || ps < 0x40000000 || ps >= 0x7F000000) return;
  if (me::native::HasEntry(vs)) return;
  static std::mutex mutex;
  static std::unordered_set<uint64_t> seen;
  std::lock_guard<std::mutex> lock(mutex);
  if (!seen.insert((uint64_t(vs) << 32) | ps).second) return;
  std::string text;
  const uint32_t ps_header = ps + 0x28;
  const uint32_t ps_shader = ps_header + Load32(base, ps_header + 24);
  const uint32_t count = (Load32(base, ps_shader + 20) >> 5) & 0x1F;
  text += "PS interpolators:";
  for (uint32_t i = 0; i < count && i < 16; ++i) {
    char w[16];
    std::snprintf(w, sizeof(w), " %08X", Load32(base, ps_shader + 32 + i * 4));
    text += w;
  }
  const uint32_t vs_header = vs + 0x368;
  const uint32_t vs_shader = vs_header + Load32(base, vs_header + 24);
  const uint32_t offset = Load32(base, vs_shader), bytes = Load32(base, vs_shader + 4);
  const uint32_t microcode = Load32(base, vs + 0x20);
  text += " | VS microcode:";
  for (uint32_t i = 0; i < bytes / 4 && i < 128; ++i) {
    char w[16];
    std::snprintf(w, sizeof(w), " %08X", Load32(base, microcode + offset + i * 4));
    text += w;
  }
  std::fprintf(stderr, "[shader link] VS %08X PS %08X %s\n", vs, ps, text.c_str());
}

}  // namespace

extern "C" void MeShaderDumpContainer(const uint8_t* base, uint32_t container) {
  EnsureInit();
  if (!g_enabled) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  DumpContainer(base, container);
}

REX_EXTERN(__imp__sub_8222F850);
extern "C" REX_FUNC(sub_8222F850) {
  me::native::NoteShaderObject(base, ctx.r3.u32, true);
  me::native::NoteShaderObject(base, ctx.r7.u32, false);
  EnsureInit();
  if (g_enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Dump(base, ctx.r3.u32, true);
    Dump(base, ctx.r7.u32, false);
  }
  const uint32_t vs = ctx.r3.u32, ps = ctx.r7.u32;
  __imp__sub_8222F850(ctx, base);
  LogLink(base, vs, ps);
}

// The draw-time shader load (every draw): the device's bound VS (+12416) and PS (+12412). Catches the
// objects that never reach sub_8222F850 (a pixel shader bound with an already patched VS).
REX_EXTERN(__imp__sub_8222F1C8);
extern "C" REX_FUNC(sub_8222F1C8) {
  const uint32_t device = ctx.r3.u32;
  if (device >= 0x40000000 && device < 0x7F000000) {
    me::native::NoteShaderObject(base, Load32(base, device + 12416), true);
    me::native::NoteShaderObject(base, Load32(base, device + 12412), false);
  }
  __imp__sub_8222F1C8(ctx, base);
  if (device >= 0x40000000 && device < 0x7F000000) {
    LogLink(base, Load32(base, device + 12416), Load32(base, device + 12412));
  }
}
