// Mass Effect - override game config (Coalesced.ini) values at load time from the settings file, without editing
// Coalesced.ini (docs/engine-settings-ab.md).
//
// WHY
//   Engine settings ([XeD3D] ring size, [TextureStreaming] PoolSize, [Engine.Engine] GC interval, ISACT audio,
//   PhysX step, ...) can only be A/B-tested today by repacking Coalesced.ini and patching its SHA-1
//   (masseffect_coalesced_sha1). One cvar is quicker and keeps the game data untouched.
//
// HOW (addresses of this edition; the full analysis is in docs/engine-settings-ab.md)
//   Every config read of the game goes through TMap<FString,FConfigSection>::Find (sub_826DF588, 40-byte pairs):
//   FConfigCacheIni::GetString(FString&) (vtable slot 6; GetInt/GetFloat/GetDouble call it), the section getters
//   (GetSectionPrivate, slot 8, which UObject::LoadConfig uses for script `config` properties such as
//   TimeBetweenPurgingPendingKillObjects or ISACT TimeBetweenHWUpdates), GetBool, ... After the original Find returns
//   a section, and only when (a) the map is one of the config files held by GConfig (0x82EA2050: vtable, then
//   TMap<FString,FConfigFile> {Data +4, Num +8}, 48-byte pairs, FConfigFile at pair +16) and (b) the section name
//   matches an override, each overridden key is looked up with the section's own TMultiMap<FString,FString>::Find
//   and its value FString is replaced in place: a new FString is built by the game's FString(const TCHAR*) (game
//   allocator), swapped in, and the old one freed by ~FString. So every later reader (native GetInt, LoadConfig,
//   SaveConfig) sees the new text, as if the ini had it. Re-applied on every lookup of that section (cheap compare),
//   so a value the ini parser re-adds later is overridden again. A key the ini does not have is not added (logged
//   once).
//
// masseffect_ini_override = "Section:Key=Value|Section:Key=Value", e.g.
//   "XeD3D:RBSecondarySize=8388608|XeD3D:RBSegmentCount=128|TextureStreaming:PoolSize=140"
// Section and key compare case-insensitively (as the engine does). Empty (default): the hook only calls the original.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(masseffect_ini_override, "", "Mass Effect",
                      "Engine settings A/B: override game config values at load, \"Section:Key=Value|...\" "
                      "(e.g. \"XeD3D:RBSecondarySize=8388608|TextureStreaming:PoolSize=140\"); only keys present "
                      "in Coalesced.ini; docs/engine-settings-ab.md; empty = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_826DF588);  // TMap<FString,FConfigSection>::Find(const FString&) -> FConfigSection*
// Game functions this file calls without hooking them, by their __imp__ names spelled in two parts:
// tools/direct_calls.py counts every literal 8-digit 82... address in the sources as hooked, and that would stop the
// generated code from inlining the hot FString constructor/destructor everywhere.
#define ME_GUEST_FN(hi, lo) __imp__sub_##hi##lo
#define ME_SECTION_FIND ME_GUEST_FN(8262, 75C8)  // FConfigSection (TMultiMap<FString,FString>)::Find -> FString*
#define ME_FSTRING_CTOR ME_GUEST_FN(8221, 0488)  // FString::FString(const TCHAR*)
#define ME_FSTRING_DTOR ME_GUEST_FN(8221, 0510)  // FString::~FString()
REX_EXTERN(ME_SECTION_FIND);
REX_EXTERN(ME_FSTRING_CTOR);
REX_EXTERN(ME_FSTRING_DTOR);

namespace {

constexpr uint32_t kGConfig = 0x82EA2050;       // FConfigCacheIni*
constexpr uint32_t kFilesData = 4, kFilesNum = 8;  // TMap<FString,FConfigFile> inside FConfigCacheIni
constexpr uint32_t kFilePairSize = 48, kFileInPair = 16;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

struct Override {
  std::string section, key, value;
  bool applied_logged = false, missing_logged = false;
  uint32_t applied = 0;
};

struct State {
  std::vector<Override> items;
};

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

std::string Trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\"");
  if (a == std::string::npos) return {};
  const size_t b = s.find_last_not_of(" \t\"");
  return s.substr(a, b - a + 1);
}

State& Get() {
  static State s = [] {
    State st;
    const std::string text = REXCVAR_GET(masseffect_ini_override);
    size_t start = 0;
    while (start <= text.size()) {
      size_t end = text.find('|', start);
      if (end == std::string::npos) end = text.size();
      const std::string item = Trim(text.substr(start, end - start));
      start = end + 1;
      if (item.empty()) continue;
      const size_t colon = item.find(':');
      const size_t eq = item.find('=', colon == std::string::npos ? 0 : colon);
      if (colon == std::string::npos || eq == std::string::npos || colon == 0 || eq == colon + 1) {
        REXLOG_WARN("[ini] ignored \"{}\": expected Section:Key=Value", item);
        continue;
      }
      Override o;
      o.section = Trim(item.substr(0, colon));
      o.key = Trim(item.substr(colon + 1, eq - colon - 1));
      o.value = Trim(item.substr(eq + 1));
      REXLOG_INFO("[ini] override [{}] {}={}", o.section, o.key, o.value);
      st.items.push_back(std::move(o));
    }
    return st;
  }();
  return s;
}

// Guest FString (Data, Num including the terminator, Max) equals an ASCII string, case-insensitively.
bool GuestStringEquals(const uint8_t* base, uint32_t fstring, const std::string& ascii, bool ignore_case) {
  const uint32_t data = Load32(base, fstring);
  const uint32_t num = Load32(base, fstring + 4);
  if (!data || num != ascii.size() + 1) return false;
  for (size_t i = 0; i < ascii.size(); ++i) {
    const uint16_t ch = uint16_t(base[data + 2 * i] << 8 | base[data + 2 * i + 1]);
    if (ch > 0x7F) return false;
    const char c = char(ch);
    if (ignore_case ? Lower(c) != Lower(ascii[i]) : c != ascii[i]) return false;
  }
  return true;
}

std::string GuestStringToAscii(const uint8_t* base, uint32_t fstring) {
  const uint32_t data = Load32(base, fstring);
  const uint32_t num = Load32(base, fstring + 4);
  std::string out;
  for (uint32_t i = 0; data && i + 1 < num && i < 120; ++i) {
    const uint16_t ch = uint16_t(base[data + 2 * i] << 8 | base[data + 2 * i + 1]);
    out.push_back(ch < 0x80 ? char(ch) : '?');
  }
  return out;
}

void WriteTchar(uint8_t* base, uint32_t at, const std::string& text) {
  for (size_t i = 0; i <= text.size(); ++i) {
    const uint16_t ch = i < text.size() ? uint16_t(uint8_t(text[i])) : 0;
    base[at + 2 * i] = uint8_t(ch >> 8);
    base[at + 2 * i + 1] = uint8_t(ch);
  }
}

// Is `map` the section map (FConfigFile) of one of GConfig's files?
bool IsConfigFile(const uint8_t* base, uint32_t map) {
  const uint32_t config = Load32(base, kGConfig);
  if (!config) return false;
  const uint32_t data = Load32(base, config + kFilesData);
  const int32_t num = int32_t(Load32(base, config + kFilesNum));
  if (!data || num <= 0 || map < data + kFileInPair) return false;
  const uint32_t offset = map - data - kFileInPair;
  return offset % kFilePairSize == 0 && offset / kFilePairSize < uint32_t(num);
}

void ApplyToSection(PPCContext& ctx, uint8_t* base, uint32_t section, Override& o) {
  // Scratch on the guest stack below this hook's frame: key FString (12 bytes) + temp FString (12) + texts.
  const size_t text_bytes = (o.key.size() + 1) * 2 + (o.value.size() + 1) * 2;
  const uint32_t sp = (ctx.r1.u32 - 0x200u - uint32_t(text_bytes) - 0x40u) & ~15u;
  const uint32_t key_fstring = sp + 0x80, temp_fstring = sp + 0x90;
  const uint32_t key_text = sp + 0xA0;
  const uint32_t value_text = (key_text + uint32_t(o.key.size() + 1) * 2 + 15u) & ~15u;
  Store32(base, sp, ctx.r1.u32);  // back chain
  WriteTchar(base, key_text, o.key);
  Store32(base, key_fstring, key_text);
  Store32(base, key_fstring + 4, uint32_t(o.key.size() + 1));
  Store32(base, key_fstring + 8, uint32_t(o.key.size() + 1));

  const uint64_t r1 = ctx.r1.u64;
  ctx.r1.u64 = sp;
  ctx.r3.u64 = section;
  ctx.r4.u64 = key_fstring;
  ME_SECTION_FIND(ctx, base);
  const uint32_t value = ctx.r3.u32;
  if (!value) {
    ctx.r1.u64 = r1;
    if (!o.missing_logged) {
      o.missing_logged = true;
      REXLOG_WARN("[ini] [{}] {} is not in the game config: override not applied", o.section, o.key);
    }
    return;
  }
  if (GuestStringEquals(base, value, o.value, false)) {
    ctx.r1.u64 = r1;
    return;
  }
  const std::string old = GuestStringToAscii(base, value);
  WriteTchar(base, value_text, o.value);
  ctx.r3.u64 = temp_fstring;
  ctx.r4.u64 = value_text;
  ME_FSTRING_CTOR(ctx, base);  // temp = FString(value_text), allocated by the game
  for (uint32_t i = 0; i < 12; i += 4) {  // swap: the config entry takes the new string, temp the old one
    const uint32_t a = Load32(base, value + i), b = Load32(base, temp_fstring + i);
    Store32(base, value + i, b);
    Store32(base, temp_fstring + i, a);
  }
  ctx.r3.u64 = temp_fstring;
  ME_FSTRING_DTOR(ctx, base);  // free the old value
  ctx.r1.u64 = r1;
  ++o.applied;
  if (!o.applied_logged) {
    o.applied_logged = true;
    REXLOG_INFO("[ini] [{}] {}: \"{}\" -> \"{}\"", o.section, o.key, old, o.value);
  } else if (o.applied <= 5) {
    REXLOG_INFO("[ini] [{}] {} re-applied ({} times; the ini set \"{}\" again)", o.section, o.key, o.applied, old);
  }
}

}  // namespace

REX_HOOK_RAW(sub_826DF588) {
  const uint32_t map = ctx.r3.u32, name = ctx.r4.u32;
  __imp__sub_826DF588(ctx, base);
  State& s = Get();
  if (s.items.empty()) return;
  const uint32_t section = ctx.r3.u32;
  if (!section || !name || !IsConfigFile(base, map)) return;
  bool any = false;
  for (const Override& o : s.items) any = any || GuestStringEquals(base, name, o.section, true);
  if (!any) return;
  // Config reads can come from several guest threads; replacing a value is a write.
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64, lr = ctx.lr;
  for (Override& o : s.items)
    if (GuestStringEquals(base, name, o.section, true)) ApplyToSection(ctx, base, section, o);
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.lr = lr;
}
