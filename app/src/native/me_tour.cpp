// Mass Effect - automated location tour: visit every map of a list in one game session and move the player through
// its navigation points and level-streaming triggers, so that everything the map can show is drawn at least once
// (pipeline prewarm list, docs/cold-start-hitches.md C), while the console profiler and the UnrealScript profiler
// attribute their numbers to the map and the point. docs/tour.md.
//
// HOW (all on the game thread, after UGameEngine::Tick, called by the hook in me_console_exec.cpp)
//   1. Start: once masseffect_exec ran (or is unset) and the player has had a pawn for masseffect_tour_frames frames
//      in world >= masseffect_exec_world: "EnableCheats".
//   2. Per map of masseffect_tour ('|'-separated "MAP START" entries, "here" = the current map): "AT MAP START"
//      (BioCheatManager.AT, the developers' travel), arrival = a new GWorld or a Tick >= masseffect_tour_load_ms, then
//      masseffect_tour_frames frames with a pawn, then the arrival commands (masseffect_tour_arrival, default "God").
//   3. Scan: every actor of every loaded level (GWorld->Levels -> ULevel->Actors). Stops are actors whose class
//      derives from NavigationPoint (PathNode, PlayerStart, CoverLink, ...) and BioTriggerStream volumes (ME1 streams
//      by those triggers, not by distance). Nav points closer than masseffect_tour_spacing to a kept one are dropped.
//      Class names come from the FName table (GNames) through UObject::Class and the super-class chain.
//   4. Visit: next stop = the nearest unvisited nav point of a level that is loaded now; when none is left, the nearest
//      untouched streaming trigger (its Touch is run directly: ABioTriggerStream vtable DoTouch); when none is left,
//      the nearest remaining point. The pawn is moved with UWorld::FarMoveActor (bNoCheck, the engine's own teleport,
//      which also updates touching volumes) to the stop + masseffect_tour_z, pinned there every frame, and the
//      controller yaw turns 360 degrees during masseffect_tour_dwell_ms. The stay is extended (up to
//      masseffect_tour_stream_wait_ms) while frames are long (>= 100 ms: blocking loads) or the set of loaded levels
//      changed in the last second; a changed set is rescanned and its new stops are added.
//   5. Leave the map after all stops, masseffect_tour_max_points stops or masseffect_tour_map_s seconds: the census
//      of every level seen (actor classes, Kismet sequence objects of the levels' GameSequences: Matinee =
//      SeqAct_Interp) and the UnrealScript profiler table are logged.
//   The console profiler block label (rex_profile.log) is "<MAP> <i>/<n>" and every stop logs the block number.
//
// masseffect_tour_diag = true (discovery): per map only the census, the checks of the assumed offsets (UObject Outer
// and Name: an actor's Outer must be its level, the world's Outer is the map package) and 16 sample stops; no
// movement. Run it once per edition before the first real tour.
//
// Safety: every wait has a timeout, the stop count and map time are capped, a missing pawn for 60 s skips the map,
// every guest pointer is range-checked before it is read. The player's health and shields are put back to their
// maximum every frame (masseffect_tour_keep_health, found by UE3 reflection) and the death guard swallows the death
// script calls on the player's objects. Not handled (docs/tour.md): combat (God only), cutscenes
// and conversations that take the camera, Mako areas (a vehicle pawn is moved without touching its physics).

#include "me_tour.h"
#include "me_tour_guest.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_STRING(masseffect_tour, "", "Mass Effect",
                      "Location tour (docs/tour.md): maps to visit in one session, '|'-separated \"MAP START\" entries "
                      "(the AT arguments), \"here\" = the current map; empty = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_tour_diag, false, "Mass Effect",
                    "Location tour: discovery mode, per map only the census, offset checks and sample stops (no "
                    "movement)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(masseffect_tour_arrival, "God", "Mass Effect",
                      "Location tour: console commands run after each arrival, '|'-separated")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_tour_dwell_ms, 1500, "Mass Effect",
                     "Location tour: time at each stop (the view turns 360 degrees in it)")
    .range(200, 60000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_tour_stream_wait_ms, 20000, "Mass Effect",
                     "Location tour: longest extension of a stop while loads are running")
    .range(0, 300000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_tour_max_points, 200, "Mass Effect", "Location tour: most stops per map")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_tour_spacing, 1200, "Mass Effect",
                     "Location tour: nav points closer than this (Unreal units) to a kept stop are dropped")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_tour_z, 100, "Mass Effect",
                     "Location tour: height above the stop the pawn is put at")
    .range(-1000, 5000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_tour_pitch, -1200, "Mass Effect",
                     "Location tour: view pitch while turning (65536 = 360 degrees; negative = down)")
    .range(-16000, 16000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_tour_pin, true, "Mass Effect",
                    "Location tour: put the pawn back on the stop every frame and stop its physics (no falls)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_tour_ghost, false, "Mass Effect",
                    "Location tour: also run the Ghost cheat after arrival (no collision: streaming triggers are then "
                    "only touched directly)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_tour_touch_streams, true, "Mass Effect",
                    "Location tour: call the DoTouch of a BioTriggerStream at its stop, at most one per stop, only "
                    "when streaming has been quiet for masseffect_tour_quiet_ms, no level is pending and "
                    "masseffect_tour_touch_gap_ms passed since the last touch (rapid touches preceded the crash in "
                    "BIOA_STA00, docs/tour.md)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_tour_touch_gap_ms, 10000, "Mass Effect",
                     "Location tour: shortest time between two streaming-trigger touches")
    .range(0, 600000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_tour_turn, false, "Mass Effect",
                    "Location tour: write the controller's Rotation (yaw turn, masseffect_tour_pitch). Off: ME1's "
                    "camera (BioCameraManager) ignores it (console 2026-10-09); tools/me1_tour.sh holds the right "
                    "stick instead")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_tour_quiet_ms, 2500, "Mass Effect",
                     "Location tour: a stop is left only after this long without a frame of 100 ms or more and "
                     "without a change of the loaded levels (streaming finished), up to masseffect_tour_stream_wait_ms")
    .range(0, 60000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_tour_stream_dwell_ms, 5000, "Mass Effect",
                     "Location tour: shortest stay at a streaming trigger (its levels load and unload)")
    .range(0, 60000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_tour_no_death, true, "Mass Effect",
                    "Location tour: while the tour runs, the script functions named in masseffect_tour_no_death_names "
                    "are not run when called on the player's pawn, its controller or an object owned by the pawn "
                    "(Shepard died in combat with God on, console 2026-10-09)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(masseffect_tour_no_death_names,
                      "TakeDamage|TakeRadiusDamage|Died|KilledBy|PawnDied|FellOutOfWorld|OutsideWorldBounds|Suicide|"
                      "CausePainTo",
                      "Mass Effect",
                      "Location tour: script function names (without class) skipped on the player's objects; the log "
                      "lists other damage/death-like calls once ([tour] guard: ...) to extend it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(masseffect_tour_no_death_any, "", "Mass Effect",
                      "Location tour: script functions (\"Class.Function\" or \"Function\") skipped on ANY object, e.g. "
                      "the mission-failure handler once its name is known from the guard log")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_tour_keep_health, true, "Mass Effect",
                    "Location tour: while the tour runs, put the player's health and shields back to their maximum every "
                    "frame (BioPawn.m_oBehavior -> m_PawnAttributes: m_HealthCurrent / m_HealthMax, m_ShieldCurrent / "
                    "m_oShield.m_pAttributes.m_ShieldMax; Pawn.Health / HealthMax), found through UE3 reflection. ME1 "
                    "applies damage natively too, so swallowing TakeDamage alone did not keep Shepard alive (console "
                    "2026-10-09). Never active outside the tour")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_tour_frames, 150, "Mass Effect",
                     "Location tour: frames with a pawn after an arrival before the tour of the map starts")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_tour_load_ms, 1500, "Mass Effect",
                     "Location tour: a Tick this long after AT counts as the arrival only when the world's package "
                     "already has the target map's name (same-map travel, the new UWorld may reuse the old address)")
    .range(100, 600000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_tour_start_s, 60, "Mass Effect",
                     "Location tour: seconds the player must have had a pawn in world >= masseffect_exec_world (the "
                     "new game's first map) before the first AT; earlier the travel is ignored")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_tour_retry_s, 45, "Mass Effect",
                     "Location tour: AT is run again when the map has not changed after this many seconds")
    .range(5, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_tour_map_s, 900, "Mass Effect", "Location tour: most seconds per map")
    .range(10, 36000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_tour_arrival_s, 240, "Mass Effect",
                     "Location tour: most seconds from the first AT to the arrival (AT is repeated every "
                     "masseffect_tour_retry_s); then the map is skipped")
    .range(10, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// <NRO folder>/logs/rex/ (sdk/src/ui/switch_crash_hooks.c).
extern "C" const char* RexSwitchLogDir(void);

// masseffect_exec_world (me_console_exec.cpp): the tour starts in that world or later.
REXCVAR_DECLARE(int32_t, masseffect_exec_world);

namespace {

namespace g = me::tour_guest;
using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------------------------------------------ guest memory
// Every read of the tour is checked against the guest heaps: a guest page that is not committed and readable is not
// touched (a read there is an unhandled access violation that froze the game in the second console run: the Kismet
// shape search took the float 1.0, 0x3F800000, for a pointer). The answer is cached per 4 KB page for one Tick.
std::unordered_map<uint32_t, bool> g_pages;
uint64_t g_reads_refused = 0;

bool PageReadable(uint32_t page) {
  auto it = g_pages.find(page);
  if (it != g_pages.end()) return it->second;
  bool ok = false;
  const uint32_t address = page << 12;
  if (address >= 0x00010000u) {
    auto* memory = rex::system::kernel_state()->memory();
    rex::memory::BaseHeap* heap = memory ? memory->LookupHeap(address) : nullptr;
    if (heap && address >= heap->heap_base() && address - heap->heap_base() < heap->heap_size()) {
      rex::memory::HeapAllocationInfo info{};
      uint32_t protect = 0;
      ok = heap->QueryRegionInfo(address, &info) && (info.state & rex::memory::kMemoryAllocationCommit) &&
           heap->QueryProtect(address, &protect) && (protect & rex::memory::kMemoryProtectRead);
    }
  }
  if (g_pages.size() > 65536) g_pages.clear();
  g_pages.emplace(page, ok);
  return ok;
}

bool Readable(uint32_t address, uint32_t bytes) {
  if (!bytes || address > 0xFFFFFFFFu - bytes) return false;
  for (uint32_t page = address >> 12, last = (address + bytes - 1) >> 12; page <= last; ++page)
    if (!PageReadable(page)) {
      ++g_reads_refused;
      return false;
    }
  return true;
}

// 0 for an address that is not readable.
uint32_t Load32(const uint8_t* base, uint32_t address) {
  if (!Readable(address, 4)) return 0;
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}
uint8_t Load8(const uint8_t* base, uint32_t address) { return Readable(address, 1) ? base[address] : 0; }
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}
float LoadF(const uint8_t* base, uint32_t address) {
  const uint32_t v = Load32(base, address);
  float f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}
void StoreF(uint8_t* base, uint32_t address, float f) {
  uint32_t v;
  std::memcpy(&v, &f, sizeof(v));
  Store32(base, address, v);
}
// Any non-null, aligned guest address. The first console run (RU, 2026-10-09) showed that the name table and the
// objects loaded at start-up (Core/Engine classes and functions) are not in 0x40000000-0xDFFFFFFF, so the range is not
// narrowed: the pointers come from the engine's own structures.
bool Plausible(uint32_t p) { return p >= 0x00010000u && p < 0xFFFF0000u && (p & 3) == 0; }
bool CodeAddress(uint32_t p) { return p >= 0x82210000u && p < 0x82FC0000u && (p & 3) == 0; }

struct Vec {
  float x = 0, y = 0, z = 0;
};
float Dist2(const Vec& a, const Vec& b) {
  const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
  return dx * dx + dy * dy + dz * dz;
}
bool Sane(const Vec& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) < 1e7f &&
         std::fabs(v.y) < 1e7f && std::fabs(v.z) < 1e7f;
}
Vec LoadVec(const uint8_t* base, uint32_t address) {
  return {LoadF(base, address), LoadF(base, address + 4), LoadF(base, address + 8)};
}

// ------------------------------------------------------------------------------------------------ names, classes
std::unordered_map<uint32_t, std::string> g_names;

// FNameEntry text: offset and encoding are calibrated once on entry 0, which is "None" in UE3 (static analysis says
// UTF-16BE at +16; the calibration also accepts another offset or ANSI and logs what it found).
struct NameFormat {
  bool checked = false, ok = false, utf16 = true;
  uint32_t offset = g::kNameEntryText;
};
NameFormat g_name_format;

void CheckNames(const uint8_t* base) {
  NameFormat& f = g_name_format;
  f.checked = true;
  const uint32_t data = Load32(base, g::kGNamesData);
  const int32_t num = int32_t(Load32(base, g::kGNamesData + 4));
  const int32_t max = int32_t(Load32(base, g::kGNamesData + 8));
  const bool data_readable = Readable(data, 4);  // heap check of me_tour's guarded reads (logged below)
  const uint32_t e0 = Plausible(data) ? Load32(base, data) : 0;
  std::string bytes;
  if (Plausible(e0))
    for (uint32_t i = 0; i < 48; ++i) bytes += fmt::format("{:02X}{}", Load8(base, e0 + i), (i & 3) == 3 ? " " : "");
  if (Plausible(e0) && Readable(e0, 64)) {
    for (uint32_t off = 0; off <= 40 && !f.ok; off += 2)
      if (!std::memcmp(base + e0 + off, "\0N\0o\0n\0e\0\0", 10)) f.ok = true, f.utf16 = true, f.offset = off;
    for (uint32_t off = 0; off <= 40 && !f.ok; ++off)
      if (!std::memcmp(base + e0 + off, "None\0", 5)) f.ok = true, f.utf16 = false, f.offset = off;
  }
  if (f.ok)
    REXLOG_INFO("[tour] names: GNames data {:08X} num {} max {}; entry 0 at {:08X}, \"None\" at +{} ({})", data, num, max,
                e0, f.offset, f.utf16 ? "UTF-16BE" : "ANSI");
  else
    REXLOG_WARN("[tour] names: entry 0 is not \"None\": GNames data {:08X} (heap says {}) num {} max {}; entry 0 at "
                "{:08X}: {} (names stay '?': check kGNamesData in me_tour_guest.h, or the heap check in PageReadable)",
                data, data_readable ? "readable" : "NOT readable", num, max, e0, bytes);
}

const std::string& NameText(const uint8_t* base, uint32_t index) {
  auto it = g_names.find(index);
  if (it != g_names.end()) return it->second;
  if (!g_name_format.checked) CheckNames(base);
  const NameFormat& f = g_name_format;
  std::string s = "?";
  const uint32_t names = Load32(base, g::kGNamesData);
  const int32_t count = int32_t(Load32(base, g::kGNamesData + 4));
  if (f.ok && Plausible(names) && index < 4000000u && (count <= 0 || index < uint32_t(count))) {
    const uint32_t entry = Load32(base, names + index * 4);
    if (Plausible(entry)) {
      s.clear();
      for (uint32_t i = 0; i < 128; ++i) {
        uint16_t ch;
        if (f.utf16) {
          const uint32_t a = entry + f.offset + i * 2;
          ch = uint16_t(Load8(base, a) << 8 | Load8(base, a + 1));
        } else {
          ch = Load8(base, entry + f.offset + i);
        }
        if (!ch) break;
        s.push_back(ch < 128 ? char(ch) : '?');
      }
      if (s.empty()) s = "?";
    }
  }
  return g_names.emplace(index, std::move(s)).first->second;
}

std::string ObjectName(const uint8_t* base, uint32_t object) {
  if (!Plausible(object)) return "None";
  std::string s = NameText(base, Load32(base, object + g::kObjName));
  const uint32_t number = Load32(base, object + g::kObjNameNumber);
  if (number && number < 1000000u) s += "_" + std::to_string(number - 1);
  return s;
}

enum Kind : uint32_t { kNav = 1, kStream = 2, kSequence = 4 };
struct ClassInfo {
  std::string name;
  uint32_t kind = 0;
};
std::unordered_map<uint32_t, ClassInfo> g_classes;

const ClassInfo& ClassOf(const uint8_t* base, uint32_t object) {
  static const ClassInfo none{"?", 0};
  const uint32_t cls = Plausible(object) ? Load32(base, object + g::kObjClass) : 0;
  if (!Plausible(cls)) return none;
  auto it = g_classes.find(cls);
  if (it != g_classes.end()) return it->second;
  ClassInfo info;
  info.name = ObjectName(base, cls);
  uint32_t c = cls;
  for (int depth = 0; depth < 40 && Plausible(c); ++depth) {
    const std::string& n = NameText(base, Load32(base, c + g::kObjName));
    if (n == "NavigationPoint") info.kind |= kNav;
    else if (n == "BioTriggerStream") info.kind |= kStream;
    else if (n == "Sequence") info.kind |= kSequence;
    c = Load32(base, c + g::kStructSuper);
  }
  return g_classes.emplace(cls, std::move(info)).first->second;
}

// ------------------------------------------------------------------------------------------------ guest calls
struct SavedRegs {
  uint64_t r1, r3, r4, r5, r6, r7, r8, r9, r10, lr;
  explicit SavedRegs(const PPCContext& c)
      : r1(c.r1.u64), r3(c.r3.u64), r4(c.r4.u64), r5(c.r5.u64), r6(c.r6.u64), r7(c.r7.u64), r8(c.r8.u64),
        r9(c.r9.u64), r10(c.r10.u64), lr(c.lr) {}
  void Restore(PPCContext& c) const {
    c.r1.u64 = r1, c.r3.u64 = r3, c.r4.u64 = r4, c.r5.u64 = r5, c.r6.u64 = r6, c.r7.u64 = r7, c.r8.u64 = r8;
    c.r9.u64 = r9, c.r10.u64 = r10, c.lr = lr;
  }
};

// Stack frame for a guest call below the hook's r1 (the original Tick has returned; nothing of the game is live
// there): back chain at sp, 0x80 bytes the callee may use as its caller's parameter area, then our data at sp+0x80.
uint32_t PushFrame(PPCContext& ctx, uint8_t* base) {
  const uint32_t sp = (ctx.r1.u32 - 0x200u) & ~15u;
  Store32(base, sp, ctx.r1.u32);
  ctx.r1.u64 = sp;
  return sp;
}

bool FarMove(PPCContext& ctx, uint8_t* base, uint32_t actor, const Vec& to) {
  const uint32_t world = Load32(base, g::kGWorld);
  if (!Plausible(world) || !Plausible(actor)) return false;
  const SavedRegs saved(ctx);
  const uint32_t sp = PushFrame(ctx, base);
  const uint32_t vec = sp + 0x80;
  StoreF(base, vec, to.x);
  StoreF(base, vec + 4, to.y);
  StoreF(base, vec + 8, to.z);
  ctx.r3.u64 = world;
  ctx.r4.u64 = actor;
  ctx.r5.u64 = vec;
  ctx.r6.u64 = 0;  // bTest
  ctx.r7.u64 = 1;  // bNoCheck: no encroachment test, always moves
  ctx.r8.u64 = 0;  // bAttachedMove
  g::FarMoveActor(ctx, base);
  const uint32_t moved = ctx.r3.u32;
  saved.Restore(ctx);
  return moved != 0;
}

bool TouchStreamTrigger(PPCContext& ctx, uint8_t* base, uint32_t trigger) {
  const uint32_t vtable = Plausible(trigger) ? Load32(base, trigger) : 0;
  if (vtable < 0x82000000u || vtable >= 0x82FC0000u) return false;
  const uint32_t fn = Load32(base, vtable + g::kTriggerDoTouchVtableOffset);
  if (!CodeAddress(fn)) return false;
  PPCFunc* host = rex::runtime::ResolveIndirectFunction(fn);
  if (!host) return false;
  const SavedRegs saved(ctx);
  PushFrame(ctx, base);
  ctx.r3.u64 = trigger;
  ctx.last_indirect_target = fn;
  host(ctx, base);
  saved.Restore(ctx);
  return true;
}

// ------------------------------------------------------------------------------------------------ world access
uint32_t LocalController(const uint8_t* base) {
  const uint32_t engine = Load32(base, g::kGEngine);
  if (!Plausible(engine) || int32_t(Load32(base, engine + g::kGamePlayersNum)) <= 0) return 0;
  const uint32_t players = Load32(base, engine + g::kGamePlayersData);
  const uint32_t player = Plausible(players) ? Load32(base, players) : 0;
  const uint32_t controller = Plausible(player) ? Load32(base, player + g::kPlayerActor) : 0;
  return Plausible(controller) ? controller : 0;
}
uint32_t PawnOf(const uint8_t* base, uint32_t controller) {
  const uint32_t pawn = controller ? Load32(base, controller + g::kControllerPawn) : 0;
  return Plausible(pawn) ? pawn : 0;
}

std::vector<uint32_t> Levels(const uint8_t* base, uint32_t world) {
  std::vector<uint32_t> out;
  if (!Plausible(world)) return out;
  const uint32_t data = Load32(base, world + g::kWorldLevelsData);
  const int32_t num = int32_t(Load32(base, world + g::kWorldLevelsNum));
  if (Plausible(data) && num > 0 && num < 4096)
    for (int32_t i = 0; i < num; ++i) {
      const uint32_t level = Load32(base, data + uint32_t(i) * 4);
      if (Plausible(level)) out.push_back(level);
    }
  const uint32_t persistent = Load32(base, world + g::kWorldPersistentLevel);
  if (Plausible(persistent) && std::find(out.begin(), out.end(), persistent) == out.end()) out.push_back(persistent);
  return out;
}

// ULevel+364: FActorIterator skips a level whose word there is non-zero (outside the editor): a level that is in
// GWorld->Levels but not (yet / any more) visible, i.e. a visibility change is pending. Assumed meaning; the diag
// line "levels pending" shows the count.
constexpr uint32_t kLevelPendingWord = 364;
bool LevelVisible(const uint8_t* base, uint32_t level) { return Load32(base, level + kLevelPendingWord) == 0; }

std::set<uint32_t> VisibleLevels(const uint8_t* base, const std::vector<uint32_t>& levels, uint32_t* pending) {
  std::set<uint32_t> out;
  uint32_t n = 0;
  for (uint32_t l : levels) {
    if (LevelVisible(base, l)) out.insert(l);
    else ++n;
  }
  if (out.empty() && !levels.empty()) {
    // Every level "pending" cannot be right (the persistent level is always visible): the assumed meaning of +364 is
    // wrong for this build, so fall back to "in GWorld->Levels" and say so once.
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_WARN("[tour] all {} levels have ULevel+364 set: visibility flag not understood, using every loaded level",
                  levels.size());
    }
    out.insert(levels.begin(), levels.end());
    n = 0;
  }
  if (pending) *pending = n;
  return out;
}

uint64_t LevelSignature(const std::vector<uint32_t>& levels) {
  uint64_t h = levels.size();
  for (uint32_t l : levels) h = h * 1000003u ^ l;
  return h;
}

template <class F>
void ForEachActor(const uint8_t* base, uint32_t level, F&& f) {
  const uint32_t data = Load32(base, level + g::kLevelActorsData);
  const int32_t num = int32_t(Load32(base, level + g::kLevelActorsNum));
  if (!Plausible(data) || num <= 0 || num > 200000) return;
  for (int32_t i = 0; i < num; ++i) {
    const uint32_t actor = Load32(base, data + uint32_t(i) * 4);
    if (!Plausible(actor) || (Load32(base, actor + g::kActorFlags0) & g::kActorDeleteMeBit)) continue;
    f(actor);
  }
}

// USequence::SequenceObjects: not located statically; found once by its shape (a TArray whose first elements are
// objects with this sequence as Outer) and checked on every sequence.
int32_t g_seq_objects_offset = -1;

bool SeqArrayAt(const uint8_t* base, uint32_t seq, uint32_t off, uint32_t* data_out, int32_t* num_out) {
  const uint32_t data = Load32(base, seq + off);
  const int32_t num = int32_t(Load32(base, seq + off + 4));
  const int32_t max = int32_t(Load32(base, seq + off + 8));
  if (!Plausible(data) || num <= 0 || num > 50000 || max < num || max > (1 << 20)) return false;
  for (int32_t i = 0; i < std::min(num, 4); ++i) {
    const uint32_t e = Load32(base, data + uint32_t(i) * 4);
    if (!Plausible(e) || Load32(base, e + g::kObjOuter) != seq || !Plausible(Load32(base, e + g::kObjClass)))
      return false;
  }
  *data_out = data;
  *num_out = num;
  return true;
}

// Caps of one level's Kismet walk (the walk has a visited set, so a cycle ends anyway).
constexpr uint32_t kMaxSequences = 4000, kMaxSeqObjects = 200000;

void CountSequence(const uint8_t* base, uint32_t seq, int depth, std::map<std::string, uint32_t>& counts,
                   std::unordered_set<uint32_t>& seen, uint32_t& objects) {
  if (depth > 12 || !Plausible(seq) || seen.size() >= kMaxSequences || objects >= kMaxSeqObjects ||
      !seen.insert(seq).second)
    return;
  uint32_t data = 0;
  int32_t num = 0;
  if (g_seq_objects_offset < 0 || !SeqArrayAt(base, seq, uint32_t(g_seq_objects_offset), &data, &num)) {
    for (uint32_t off = 60; off < 1024; off += 4)
      if (SeqArrayAt(base, seq, off, &data, &num)) {
        if (g_seq_objects_offset != int32_t(off))
          REXLOG_INFO("[tour] Kismet: USequence::SequenceObjects found at +{} (sequence {})", off,
                      ObjectName(base, seq));
        g_seq_objects_offset = int32_t(off);
        break;
      }
    if (!num) return;  // empty or not recognised
  }
  for (int32_t i = 0; i < num && objects < kMaxSeqObjects; ++i, ++objects) {
    const uint32_t obj = Load32(base, data + uint32_t(i) * 4);
    const ClassInfo& ci = ClassOf(base, obj);
    ++counts[ci.name];
    if (ci.kind & kSequence) CountSequence(base, obj, depth + 1, counts, seen, objects);
  }
}

std::string Top(const std::map<std::string, uint32_t>& counts, size_t n) {
  std::vector<std::pair<uint32_t, std::string>> v;
  for (auto& [k, c] : counts) v.emplace_back(c, k);
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
  std::string s;
  for (size_t i = 0; i < v.size() && i < n; ++i) s += (i ? ", " : "") + v[i].second + " " + std::to_string(v[i].first);
  return s;
}

// ------------------------------------------------------------------------------------------------ tour state
struct Stop {
  Vec at;
  uint32_t level = 0, actor = 0;
  uint32_t kind = 0;
  std::string cls;
  bool done = false;
};

enum class Phase { kWaitStart, kTravel, kWaitArrival, kSettle, kScan, kNext, kDwell, kMapDone, kFinished };

struct Tour {
  bool configured = false, diag = false;
  std::vector<std::string> maps;
  std::vector<std::string> arrival_commands;
  // UE3's God (and other cheats) toggle: run the arrival commands only for a controller that has not had them yet,
  // or a map change that keeps the controller turns God off again (2026-10-08: Shepard died on the console).
  uint32_t arrival_controller = 0;
  Phase phase = Phase::kWaitStart;
  size_t map_index = 0;
  std::string map;  // label of the current map (AT argument or package name)

  uint32_t worlds = 0, last_world = 0;
  uint32_t pawn_frames = 0;
  bool long_tick = false;
  Clock::time_point phase_start, map_start, last_long_frame, last_level_change, last_pawn;
  // Start: when the pawn was first seen in the start world (continuously since).
  bool start_pawn = false;
  Clock::time_point start_pawn_since;
  // Travel: what the world looked like when AT ran.
  uint32_t travel_world = 0, travel_level = 0;
  std::string travel_package, target;
  Clock::time_point last_at;
  uint32_t at_count = 0;

  // Current map.
  std::vector<Stop> stops;
  std::set<uint32_t> levels_seen;
  std::unordered_map<uint64_t, std::vector<uint32_t>> grid;  // kept nav stops by spacing cell
  uint64_t level_signature = 0;
  std::map<std::string, uint32_t> census, kismet;
  uint32_t nav_total = 0, nav_dropped = 0, streams_total = 0;
  size_t visited = 0, cur = SIZE_MAX;
  uint32_t move_failures = 0;
  int32_t yaw0 = 0;
  bool extended = false;
  // Streaming triggers: the touch of the current stop, the last touch, counters.
  bool touch_done = false;
  Clock::time_point last_touch;
  uint32_t touches = 0, touches_skipped = 0, rebound = 0;
  bool idle = false;  // no visitable stop, waiting for a pending level
  Clock::time_point idle_since;

  // Health keeper (per map: from the previous map's end to this map's end).
  uint32_t health_pawn_logged = 0;  // pawn whose health properties were logged on this map
  uint64_t health_frames = 0, health_low_frames = 0, health_restores = 0, shield_restores = 0;
  float health_min_pct = 100.0f;

  // Totals.
  size_t maps_done = 0, stops_done = 0;
  Clock::time_point tour_start;
};

std::vector<std::string> Split(const std::string& text) {
  std::vector<std::string> out;
  std::string item;
  auto flush = [&] {
    const size_t a = item.find_first_not_of(" \t\"");
    const size_t b = item.find_last_not_of(" \t\"");
    if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
    item.clear();
  };
  for (char c : text) {
    if (c == '|') flush();
    else item.push_back(c);
  }
  flush();
  return out;
}

Tour& Get() {
  static Tour t = [] {
    Tour s;
    s.maps = Split(REXCVAR_GET(masseffect_tour));
    s.arrival_commands = Split(REXCVAR_GET(masseffect_tour_arrival));
    s.diag = REXCVAR_GET(masseffect_tour_diag);
    s.configured = !s.maps.empty();
    if (s.configured)
      REXLOG_INFO("[tour] {} map(s){}, edition {}: dwell {} ms, at most {} stops and {} s per map, spacing {}",
                  s.maps.size(), s.diag ? " (discovery mode: no movement)" : "", g::kEdition,
                  REXCVAR_GET(masseffect_tour_dwell_ms), REXCVAR_GET(masseffect_tour_max_points),
                  REXCVAR_GET(masseffect_tour_map_s), REXCVAR_GET(masseffect_tour_spacing));
    return s;
  }();
  return t;
}

double Since(Clock::time_point t, Clock::time_point now) {
  return std::chrono::duration<double>(now - t).count();
}

void SetLabel(const std::string& label) { RexSwitchPerfSetLabel(label.c_str()); }

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(uint8_t(c)));
  return s;
}

std::string PackageOf(const uint8_t* base, uint32_t world) {
  return Plausible(world) ? ObjectName(base, Load32(base, world + g::kObjOuter)) : std::string("None");
}

// <NRO folder>/logs/rex/tour_status.txt, rewritten (open, write, close) on every change: sys-ftpd cannot read the log
// the game keeps open, so tools/me1_tour.sh polls this file. Line 1 "running" / "done", line 2 the last event.
// Line 4: "finished: 1 2 ..." = 1-based indices (in masseffect_tour) of the maps left normally, so the bot can resume
// after a hang with the maps that are not finished.
std::string g_finished;

void Status(const std::string& text, bool done = false) {
  static const std::string path = std::string(RexSwitchLogDir()) + "tour_status.txt";
  static const Clock::time_point start = Clock::now();
  if (std::FILE* f = std::fopen(path.c_str(), "w")) {
    std::fprintf(f, "%s\n%s\n%.0f s\nfinished:%s\n", done ? "done" : "running", text.c_str(),
                 Since(start, Clock::now()), g_finished.c_str());
    std::fclose(f);
  }
}

void Run(PPCContext& ctx, uint8_t* base, const std::string& command) {
  const uint32_t handled = me::exec::RunConsoleCommand(ctx, base, command);
  REXLOG_INFO("[tour] \"{}\" -> {}", command, handled ? "handled" : "not handled");
}

// Adds the stops of levels not scanned yet and counts their actors and Kismet objects for the census.
// Scans at most kLevelsPerScan levels not seen yet (the rest on the next frames, so one frame never walks a whole map)
// and returns true when every loaded level has been scanned. Logs a watchdog line when one call takes over 2 s.
constexpr size_t kLevelsPerScan = 2;

uint64_t CellKey(const Vec& v, float cell) {
  const auto q = [cell](float x) { return uint64_t(uint32_t(int32_t(std::floor(x / cell))) & 0x1FFFFF); };
  return q(v.x) << 42 | q(v.y) << 21 | q(v.z);
}

bool Scan(Tour& t, const uint8_t* base, uint32_t world) {
  const auto started = Clock::now();
  auto last_watchdog = started;
  const std::vector<uint32_t> levels = Levels(base, world);
  const float spacing = std::max(1.0f, float(REXCVAR_GET(masseffect_tour_spacing)));
  const float spacing2 = spacing * spacing;
  size_t added = 0, new_levels = 0, actors = 0;
  uint32_t outer_ok = 0, outer_bad = 0, kismet_objects = 0;
  bool more = false;
  // A nav point is dropped when a kept nav stop is within `spacing`: look in the 27 grid cells around it.
  // Returns the index of a kept nav stop within `spacing`, or -1.
  auto near_kept = [&](const Vec& at) -> int64_t {
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz) {
          const Vec probe{at.x + dx * spacing, at.y + dy * spacing, at.z + dz * spacing};
          auto it = t.grid.find(CellKey(probe, spacing));
          if (it == t.grid.end()) continue;
          for (uint32_t i : it->second)
            if (Dist2(t.stops[i].at, at) < spacing2) return int64_t(i);
        }
    return -1;
  };
  const std::set<uint32_t> present(levels.begin(), levels.end());
  auto watchdog = [&](const char* where, uint32_t level) {
    const auto now = Clock::now();
    if (Since(last_watchdog, now) < 2.0) return;
    last_watchdog = now;
    REXLOG_WARN("[tour] map {}: scan running {:.1f} s ({}, level {}, {} actors, {} Kismet objects, {} reads refused)",
                t.map, Since(started, now), where, ObjectName(base, level), actors, kismet_objects, g_reads_refused);
  };
  for (uint32_t level : levels) {
    if (t.levels_seen.count(level)) continue;
    if (new_levels == kLevelsPerScan) {
      more = true;
      break;
    }
    t.levels_seen.insert(level);
    ++new_levels;
    ForEachActor(base, level, [&](uint32_t actor) {
      if ((++actors & 1023) == 0) watchdog("actors", level);
      const ClassInfo& ci = ClassOf(base, actor);
      ++t.census[ci.name];
      if (Load32(base, actor + g::kObjOuter) == level) ++outer_ok;
      else ++outer_bad;
      if (!(ci.kind & (kNav | kStream))) return;
      const Vec at = LoadVec(base, actor + g::kActorLocation);
      if (!Sane(at)) return;
      if (ci.kind & kStream) {
        // A trigger of a reloaded level (new ULevel address) replaces its unvisited copy of the old level.
        for (Stop& old : t.stops)
          if (old.kind == kStream && !old.done && !present.count(old.level) && Dist2(old.at, at) < 1.0f) {
            old.level = level;
            old.actor = actor;
            ++t.rebound;
            return;
          }
        ++t.streams_total;
      } else {
        ++t.nav_total;
        const int64_t kept = near_kept(at);
        if (kept >= 0) {
          // Same place as a stop kept from a level that is not loaded any more (unloaded, then loaded again at
          // another address): the stop now belongs to this level, so it can be visited.
          Stop& old = t.stops[size_t(kept)];
          if (!old.done && !present.count(old.level)) {
            old.level = level;
            old.actor = actor;
            ++t.rebound;
          } else {
            ++t.nav_dropped;
          }
          return;
        }
        t.grid[CellKey(at, spacing)].push_back(uint32_t(t.stops.size()));
      }
      t.stops.push_back({at, level, actor, ci.kind & kStream ? uint32_t(kStream) : uint32_t(kNav), ci.name, false});
      ++added;
    });
    const uint32_t seqs = Load32(base, level + g::kLevelGameSequencesData);
    const int32_t nseq = int32_t(Load32(base, level + g::kLevelGameSequencesNum));
    if (Plausible(seqs) && nseq > 0 && nseq < 64) {
      std::unordered_set<uint32_t> seen;
      uint32_t objects = 0;
      for (int32_t i = 0; i < nseq; ++i)
        CountSequence(base, Load32(base, seqs + uint32_t(i) * 4), 0, t.kismet, seen, objects);
      kismet_objects += objects;
      if (objects >= kMaxSeqObjects || seen.size() >= kMaxSequences)
        REXLOG_WARN("[tour] map {}: Kismet walk of level {} capped ({} sequences, {} objects)", t.map,
                    ObjectName(base, level), seen.size(), objects);
      watchdog("Kismet", level);
    }
  }
  if (!more) t.level_signature = LevelSignature(levels);
  if (new_levels)
    REXLOG_INFO("[tour] map {}: scan of {} new level(s) ({} loaded{}): {} stops added, {} in total; actor Outer == "
                "level for {} of {} actors; {} Kismet objects; {:.0f} ms, {} reads refused",
                t.map, new_levels, levels.size(), more ? ", more next frame" : "", added, t.stops.size(), outer_ok,
                outer_ok + outer_bad, kismet_objects, Since(started, Clock::now()) * 1000.0, g_reads_refused);
  return !more;
}

void LogCensus(const Tour& t) {
  uint32_t actors = 0, kis = 0;
  for (auto& [k, c] : t.census) actors += c;
  for (auto& [k, c] : t.kismet) kis += c;
  REXLOG_INFO("[tour] census {}: {} levels, {} actors of {} classes; nav points {} ({} dropped by spacing), "
              "streaming triggers {}",
              t.map, t.levels_seen.size(), actors, t.census.size(), t.nav_total, t.nav_dropped, t.streams_total);
  REXLOG_INFO("[tour] census {} actors: {}", t.map, Top(t.census, 60));
  REXLOG_INFO("[tour] census {} kismet: {} objects of {} classes: {}", t.map, kis, t.kismet.size(),
              Top(t.kismet, 80));
  auto count = [&](const char* name) {
    auto it = t.kismet.find(name);
    return it == t.kismet.end() ? 0u : it->second;
  };
  REXLOG_INFO("[tour] census {} scripted: Matinee (SeqAct_Interp) {}, InterpData {}, Sequence {}", t.map,
              count("SeqAct_Interp"), count("InterpData"), count("Sequence"));
}

// Only stops whose level is loaded and visible now: a stop of an unloaded level shows sky or empty space (console
// 2026-10-09). Stops of levels that never come back are left (counted at the end of the map).
size_t PickNext(Tour& t, const Vec& from, const std::set<uint32_t>& loaded) {
  size_t best = SIZE_MAX;
  float best_d = 0;
  auto consider = [&](auto pred) {
    for (size_t i = 0; i < t.stops.size(); ++i) {
      const Stop& s = t.stops[i];
      if (s.done || !pred(s)) continue;
      const float d = Dist2(from, s.at);
      if (best == SIZE_MAX || d < best_d) best = i, best_d = d;
    }
    return best != SIZE_MAX;
  };
  if (consider([&](const Stop& s) { return s.kind == kNav && loaded.count(s.level); })) return best;
  consider([&](const Stop& s) { return s.kind == kStream && loaded.count(s.level); });
  return best;
}

void BeginMap(Tour& t) {
  t.stops.clear();
  t.levels_seen.clear();
  t.grid.clear();
  t.census.clear();
  t.kismet.clear();
  t.nav_total = t.nav_dropped = t.streams_total = 0;
  t.visited = 0;
  t.cur = SIZE_MAX;
  t.move_failures = 0;
  t.touches = t.touches_skipped = t.rebound = 0;
}

// ------------------------------------------------------------------------------------------------ death guard
// UObject::ProcessInternal runs a script function's body after its caller (CallFunction or ProcessEvent) has set up
// the parameters, so returning without calling it skips the body and leaves the caller's bytecode intact; the
// return value stays as the caller initialised it (zero for events). Active from the tour's start to its end, on the
// game thread only. Verdict per UFunction (cached; names are resolved once).
struct Guard {
  bool on = false;
  uint32_t pawn = 0, controller = 0, behavior = 0;  // behavior: BioPawn.m_oBehavior (set by the health keeper)
  std::unordered_set<std::string> names, any;
  std::unordered_map<uint32_t, uint8_t> verdict;  // 0 run, 1 skip on the player's objects, 2 skip on any object
  std::unordered_map<uint32_t, uint32_t> skipped;
  uint64_t total = 0;
};
Guard g_guard;
thread_local bool t_tour_thread = false;

bool DeathLike(const std::string& name) {
  std::string l = name;
  for (char& c : l) c = char(std::tolower(uint8_t(c)));
  for (const char* w : {"die", "dead", "death", "kill", "fail", "gameover", "damage", "suicide", "fellout",
                        "outsideworld", "health", "pain", "wound"})
    if (l.find(w) != std::string::npos) return true;
  return false;
}

// ------------------------------------------------------------------------------------------------ reflection
// UE3 property lookup on the live objects: a UClass / UScriptStruct keeps its fields in a list (UStruct::Children,
// UField::Next) and its parent in SuperField; a UProperty keeps the byte offset of its value in the object
// (UProperty::Offset). Offsets in me_tour_guest.h (static, from CallFunction), checked once at run time on
// Actor.Location / Rotation / Physics, whose offsets are verified elsewhere.
struct Prop {
  uint32_t field = 0;   // the UProperty object (0 = not found)
  uint32_t offset = 0;  // UProperty::Offset
  std::string type;     // class name of the UProperty: FloatProperty, IntProperty, StructProperty, ObjectProperty, ...
  std::string owner;    // name of the class / struct that declares it
  explicit operator bool() const { return field != 0; }
};
std::map<std::pair<uint32_t, std::string>, Prop> g_props;  // (class or struct, property name) -> property

bool EndsWith(const std::string& s, const char* tail) {
  const size_t n = std::strlen(tail);
  return s.size() >= n && !s.compare(s.size() - n, n, tail);
}

// Calls f(field, owner_struct) for every property of `strct` and its parents (nearest class first).
template <class F>
void ForEachProp(const uint8_t* base, uint32_t strct, F&& f) {
  uint32_t s = strct;
  for (int depth = 0; depth < 40 && Plausible(s); ++depth, s = Load32(base, s + g::kStructSuper)) {
    uint32_t field = Load32(base, s + g::kStructChildren);
    for (int n = 0; n < 8000 && Plausible(field); ++n, field = Load32(base, field + g::kFieldNext))
      if (EndsWith(ClassOf(base, field).name, "Property")) f(field, s);
  }
}

const Prop& FindProp(const uint8_t* base, uint32_t strct, const std::string& name) {
  static const Prop none;
  if (!Plausible(strct)) return none;
  auto key = std::make_pair(strct, name);
  auto it = g_props.find(key);
  if (it != g_props.end()) return it->second;
  Prop found;
  ForEachProp(base, strct, [&](uint32_t field, uint32_t owner) {
    if (found || NameText(base, Load32(base, field + g::kObjName)) != name) return;
    const uint32_t offset = Load32(base, field + g::kPropertyOffset);
    if (offset >= 0x10000u) return;  // not an offset: layout wrong
    found = {field, offset, ClassOf(base, field).name, ObjectName(base, owner)};
  });
  return g_props.emplace(std::move(key), std::move(found)).first->second;
}

const Prop& FindPropOf(const uint8_t* base, uint32_t object, const std::string& name) {
  return FindProp(base, Plausible(object) ? Load32(base, object + g::kObjClass) : 0, name);
}

// Value of an ObjectProperty `name` of `object` (0 when absent or not an object).
uint32_t ObjectRef(const uint8_t* base, uint32_t object, const std::string& name) {
  const Prop& p = FindPropOf(base, object, name);
  if (!p || p.type != "ObjectProperty") return 0;
  const uint32_t v = Load32(base, object + p.offset);
  return Plausible(v) ? v : 0;
}

// UStructProperty::Struct: the first word after the UProperty fields that points to a UScriptStruct (found once).
int32_t g_struct_prop_struct = -1;
uint32_t StructOf(const uint8_t* base, const Prop& p) {
  if (!p || p.type != "StructProperty") return 0;
  if (g_struct_prop_struct >= 0) {
    const uint32_t s = Load32(base, p.field + uint32_t(g_struct_prop_struct));
    if (Plausible(s) && ClassOf(base, s).name == "ScriptStruct") return s;
  }
  for (uint32_t off = g::kPropertyOffset + 4; off < 256; off += 4) {
    const uint32_t s = Load32(base, p.field + off);
    if (!Plausible(s) || ClassOf(base, s).name != "ScriptStruct") continue;
    if (g_struct_prop_struct != int32_t(off))
      REXLOG_INFO("[tour] reflection: UStructProperty::Struct at +{} ({}.{} -> {})", off, p.owner,
                  ObjectName(base, p.field), ObjectName(base, s));
    g_struct_prop_struct = int32_t(off);
    return s;
  }
  return 0;
}

// Once: the property offsets of Actor.Location / Rotation / Physics must be the verified ones, otherwise the
// reflection layout is wrong and nothing is written.
enum class Reflection { kUnchecked, kOk, kBad };
Reflection g_reflection = Reflection::kUnchecked;

bool ReflectionOk(const uint8_t* base, uint32_t pawn) {
  if (g_reflection != Reflection::kUnchecked) return g_reflection == Reflection::kOk;
  uint32_t actor = Plausible(pawn) ? Load32(base, pawn + g::kObjClass) : 0;
  for (int depth = 0; depth < 40 && Plausible(actor); ++depth, actor = Load32(base, actor + g::kStructSuper))
    if (NameText(base, Load32(base, actor + g::kObjName)) == "Actor") break;
  if (!Plausible(actor)) return false;  // names not readable yet: try again next frame
  const Prop& loc = FindProp(base, actor, "Location");
  const Prop& rot = FindProp(base, actor, "Rotation");
  const Prop& phys = FindProp(base, actor, "Physics");
  const bool ok = loc && rot && phys && loc.offset == g::kActorLocation && rot.offset == g::kActorRotation &&
                  phys.offset == g::kActorPhysics;
  g_reflection = ok ? Reflection::kOk : Reflection::kBad;
  const auto at = [](const Prop& p) { return p ? fmt::format("+{} ({})", p.offset, p.type) : std::string("not found"); };
  if (ok)
    REXLOG_INFO("[tour] reflection check ok: Actor.Location {}, Rotation {}, Physics {} (expected +{} / +{} / +{})",
                at(loc), at(rot), at(phys), g::kActorLocation, g::kActorRotation, g::kActorPhysics);
  else
    REXLOG_WARN("[tour] reflection check FAILED: Actor.Location {}, Rotation {}, Physics {} (expected +{} / +{} / +{}); "
                "the health keeper is off (check kStructChildren / kFieldNext / kPropertyOffset in me_tour_guest.h)",
                at(loc), at(rot), at(phys), g::kActorLocation, g::kActorRotation, g::kActorPhysics);
  return ok;
}

// ------------------------------------------------------------------------------------------------ health keeper
// ME1 keeps a pawn's health outside APawn::Health (static analysis of BIOC_Base, 2026-10-09):
//   BioPawn.m_oBehavior (BioPawnBehavior < BioEpicPawnBehavior < BioActorBehavior)
//     .m_PawnAttributes (BioAttributesEpicPawn / BioAttributesPawn)
//        m_HealthCurrent (float), m_HealthMax (BioComplexFloatStructAttribute: m_Base, m_Current, m_Min, m_Max, ...),
//        m_ShieldCurrent (float)
//     .m_oShield (BioShield) .m_pAttributes (BioAttributesShield) .m_ShieldMax (BioComplexFloatStructAttribute)
// plus UE3's Pawn.Health / HealthMax (ints). Each value is found by name through reflection; whatever is missing (a
// vehicle pawn, another class) is skipped. "max" of a complex attribute = its m_Current (the value with modifiers),
// m_Base when m_Current is not positive.
enum VitalKind { kVitalHealth, kVitalShield, kVitalPawnHealth };
struct Vital {
  const char* label = "";
  VitalKind kind = kVitalHealth;
  uint32_t cur = 0, max = 0;  // guest addresses of the values (0 = not found)
  bool cur_int = false, max_int = false;
};

struct Vitals {
  uint32_t behavior = 0, attributes = 0, shield = 0, shield_attributes = 0;
  std::vector<Vital> list;
};

// Address of the current value of a complex attribute struct property (m_Current, else m_Base), 0 when not found.
uint32_t ComplexValue(const uint8_t* base, uint32_t object, const Prop& p, bool* is_int) {
  if (!p) return 0;
  if (p.type == "FloatProperty" || p.type == "IntProperty") {
    *is_int = p.type == "IntProperty";
    return object + p.offset;
  }
  const uint32_t strct = StructOf(base, p);
  for (const char* member : {"m_Current", "m_Base"}) {
    const Prop& m = FindProp(base, strct, member);
    if (!m || (m.type != "FloatProperty" && m.type != "IntProperty")) continue;
    *is_int = m.type == "IntProperty";
    const uint32_t a = object + p.offset + m.offset;
    if (std::string(member) == "m_Base") return a;
    const uint32_t raw = Load32(base, a);
    float v;
    std::memcpy(&v, &raw, sizeof(v));
    if (*is_int ? int32_t(raw) > 0 : (std::isfinite(v) && v > 0.0f)) return a;
  }
  return 0;
}

Vitals FindVitals(const uint8_t* base, uint32_t pawn) {
  Vitals v;
  auto add = [&](const char* label, VitalKind kind, uint32_t object, const Prop& cur, uint32_t max_object,
                 const Prop& max) {
    if (!object || !max_object || !cur || (cur.type != "FloatProperty" && cur.type != "IntProperty")) return;
    Vital x;
    x.label = label;
    x.kind = kind;
    x.cur = object + cur.offset;
    x.cur_int = cur.type == "IntProperty";
    x.max = ComplexValue(base, max_object, max, &x.max_int);
    if (x.max) v.list.push_back(x);
  };
  v.behavior = ObjectRef(base, pawn, "m_oBehavior");
  v.attributes = ObjectRef(base, v.behavior, "m_PawnAttributes");
  v.shield = ObjectRef(base, v.behavior, "m_oShield");
  v.shield_attributes = ObjectRef(base, v.shield, "m_pAttributes");
  add("m_HealthCurrent", kVitalHealth, v.attributes, FindPropOf(base, v.attributes, "m_HealthCurrent"), v.attributes,
      FindPropOf(base, v.attributes, "m_HealthMax"));
  add("m_ShieldCurrent", kVitalShield, v.attributes, FindPropOf(base, v.attributes, "m_ShieldCurrent"),
      v.shield_attributes, FindPropOf(base, v.shield_attributes, "m_ShieldMax"));
  add("Pawn.Health", kVitalPawnHealth, pawn, FindPropOf(base, pawn, "Health"), pawn, FindPropOf(base, pawn, "HealthMax"));
  return v;
}

double LoadNumber(const uint8_t* base, uint32_t address, bool is_int) {
  return is_int ? double(int32_t(Load32(base, address))) : double(LoadF(base, address));
}

bool Lower(const std::string& s, const char* word) {
  std::string l = s;
  for (char& c : l) c = char(std::tolower(uint8_t(c)));
  return l.find(word) != std::string::npos;
}

// Every numeric property whose name contains "health" or "shield" on the objects searched (discovery: tells which
// other values exist if the ones above are not what the HUD and the death use).
std::string HealthLikeProps(const uint8_t* base, uint32_t object) {
  if (!Plausible(object)) return "";
  std::string out;
  uint32_t n = 0;
  ForEachProp(base, Load32(base, object + g::kObjClass), [&](uint32_t field, uint32_t owner) {
    const std::string name = NameText(base, Load32(base, field + g::kObjName));
    if (n >= 24 || (!Lower(name, "health") && !Lower(name, "shield"))) return;
    const std::string& type = ClassOf(base, field).name;
    const uint32_t off = Load32(base, field + g::kPropertyOffset);
    std::string value;
    if (type == "FloatProperty" || type == "IntProperty") {
      value = fmt::format("{:g}", LoadNumber(base, object + off, type == "IntProperty"));
    } else if (type == "StructProperty") {
      const Prop p{field, off, type, ObjectName(base, owner)};
      bool is_int = false;
      const uint32_t a = ComplexValue(base, object, p, &is_int);
      value = a ? fmt::format("{:g} (+{})", LoadNumber(base, a, is_int), a - object) : "struct";
    } else {
      return;
    }
    out += fmt::format("{}{}.{} @+{} = {}", n++ ? ", " : "", ObjectName(base, owner), name, off, value);
  });
  return out;
}

// `log_now`: the map's pawn is settled (logs the properties once per map and pawn).
void KeepHealth(Tour& t, uint8_t* base, uint32_t pawn, bool write, bool log_now) {
  if (!pawn || !ReflectionOk(base, pawn)) return;
  const Vitals v = FindVitals(base, pawn);
  g_guard.behavior = v.behavior;  // the guard's "player objects" include the pawn's behaviour object
  if (log_now && t.health_pawn_logged != pawn) {
    t.health_pawn_logged = pawn;
    std::string found;
    for (const Vital& x : v.list)
      found += fmt::format("{}{} @{:08X} = {:g} / max @{:08X} = {:g}", found.empty() ? "" : ", ", x.label, x.cur,
                           LoadNumber(base, x.cur, x.cur_int), x.max, LoadNumber(base, x.max, x.max_int));
    REXLOG_INFO("[tour] map {}: health properties of {} ({}): {}; behavior {} ({}), attributes {} ({}), shield "
                "attributes {}; keeping them full: {}",
                t.map, ObjectName(base, pawn), ClassOf(base, pawn).name, found.empty() ? "none found" : found,
                ObjectName(base, v.behavior), ClassOf(base, v.behavior).name, ObjectName(base, v.attributes),
                ClassOf(base, v.attributes).name, ObjectName(base, v.shield_attributes), write ? "yes" : "no");
    for (uint32_t object : {pawn, v.behavior, v.attributes, v.shield_attributes}) {
      const std::string list = HealthLikeProps(base, object);
      if (!list.empty()) REXLOG_INFO("[tour] map {}: health-like properties of {}: {}", t.map, ObjectName(base, object), list);
    }
  }
  // The below-50 % count uses ME1's health (m_HealthCurrent), UE3's Pawn.Health only when ME1's is not there.
  bool counted = false;
  for (const Vital& x : v.list) {
    if (!Readable(x.cur, 4) || !Readable(x.max, 4)) continue;
    const double cur = LoadNumber(base, x.cur, x.cur_int), max = LoadNumber(base, x.max, x.max_int);
    if (!std::isfinite(cur) || !std::isfinite(max) || max <= 0.0 || max > 1e7) continue;
    if (x.kind != kVitalShield && !counted) {
      counted = true;
      ++t.health_frames;
      const float pct = float(std::max(0.0, cur) * 100.0 / max);
      t.health_min_pct = std::min(t.health_min_pct, pct);
      if (pct < 50.0f) ++t.health_low_frames;
    }
    if (!write || cur >= max) continue;
    if (x.cur_int) Store32(base, x.cur, uint32_t(int32_t(max)));
    else StoreF(base, x.cur, float(max));
    ++(x.kind == kVitalShield ? t.shield_restores : t.health_restores);
  }
}

void FinishMap(Tour& t, uint8_t* base, const char* why) {
  const auto now = Clock::now();
  size_t unvisited = 0;
  for (const Stop& st : t.stops) unvisited += !st.done;
  REXLOG_INFO("[tour] map {} finished ({}): {} of {} stops visited in {:.0f} s; {} skipped (level not loaded or "
              "caps), {} rebound to a reloaded level; triggers {} touched, {} not touched | profile block {}",
              t.map, why, t.visited, t.stops.size(), Since(t.map_start, now), unvisited, t.rebound, t.touches,
              t.touches_skipped, RexSwitchPerfReportIndex());
  LogCensus(t);
  me::sprof::DumpAndReset(base, t.map);
  if (g_guard.total) REXLOG_INFO("[tour] map {}: {} death/damage calls swallowed so far", t.map, g_guard.total);
  if (t.health_frames)
    REXLOG_INFO("[tour] map {}: player health below 50 % in {} of {} frames (lowest {:.0f} %); health restored {} "
                "times, shields {} times{}",
                t.map, t.health_low_frames, t.health_frames, t.health_min_pct, t.health_restores, t.shield_restores,
                REXCVAR_GET(masseffect_tour_keep_health) ? "" : " (keeper off: counted only)");
  t.health_pawn_logged = 0;
  t.health_frames = t.health_low_frames = t.health_restores = t.shield_restores = 0;
  t.health_min_pct = 100.0f;
  g_props.clear();  // classes of an unloaded map's packages can go away
  g_finished += " " + std::to_string(t.map_index + 1);
  Status("map " + t.map + " finished (" + why + "): " + std::to_string(t.visited) + " of " +
         std::to_string(t.stops.size()) + " stops");
  ++t.maps_done;
  ++t.map_index;
  t.phase = t.map_index < t.maps.size() ? Phase::kTravel : Phase::kFinished;
  t.phase_start = now;
  if (t.phase == Phase::kFinished) {
    SetLabel("tour done");
    REXLOG_INFO("[tour] done: {} map(s), {} stops in {:.0f} s", t.maps_done, t.stops_done, Since(t.tour_start, now));
    Status("done: " + std::to_string(t.maps_done) + " map(s), " + std::to_string(t.stops_done) + " stops", true);
  }
}

}  // namespace

namespace me::tour {

void ScriptGuard(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  Guard& gd = g_guard;
  if (!gd.on || !t_tour_thread) [[likely]] {
    original(ctx, base);
    return;
  }
  const uint32_t object = ctx.r3.u32;
  const uint32_t function = Load32(base, ctx.r4.u32 + 4);  // FFrame::Node
  if (!Plausible(function)) {
    original(ctx, base);
    return;
  }
  auto it = gd.verdict.find(function);
  if (it == gd.verdict.end()) {
    const std::string name = ObjectName(base, function);
    const std::string full = ObjectName(base, Load32(base, function + g::kObjOuter)) + "." + name;
    uint8_t v = gd.any.count(full) || gd.any.count(name) ? 2 : gd.names.count(name) ? 1 : 0;
    it = gd.verdict.emplace(function, v).first;
    if (v == 0 && DeathLike(name))
      REXLOG_INFO("[tour] guard: death/damage-like script function {} seen (runs; add its name to "
                  "masseffect_tour_no_death_names to skip it on the player)", full);
    else if (v)
      REXLOG_INFO("[tour] guard: {} will be skipped {}", full, v == 2 ? "on every object" : "on the player");
  }
  const uint8_t v = it->second;
  const bool player = object && (object == gd.pawn || object == gd.controller || object == gd.behavior ||
                                 (gd.pawn && Load32(base, object + g::kObjOuter) == gd.pawn));
  if (v == 0 || (v == 1 && !player)) {
    original(ctx, base);
    return;
  }
  const uint32_t n = ++gd.skipped[function];
  ++gd.total;
  if (n <= 5 || n % 100 == 0)
    REXLOG_INFO("[tour] death event {}.{} on {} swallowed ({} times)", ObjectName(base, Load32(base, function + g::kObjOuter)),
                ObjectName(base, function), ObjectName(base, object), n);
}


bool Enabled() { return Get().configured; }

std::string ObjectNameOf(const uint8_t* base, uint32_t object) { return ObjectName(base, object); }

void Tick(PPCContext& ctx, uint8_t* base, int64_t tick_ms, bool exec_done) {
  me::sprof::NoteGameThread();
  if (!g_pages.empty()) g_pages.clear();  // guest pages can be freed between frames
  Tour& t = Get();
  const uint32_t world = Load32(base, g::kGWorld);
  if (!t.configured) {
    // Without the tour the UnrealScript profiler still dumps one table per map (on every GWorld change).
    static uint32_t seen_world = 0;
    if (world != seen_world && me::sprof::Enabled()) {
      seen_world = world;
      me::sprof::NoteWorld(base, world, ObjectName(base, Plausible(world) ? Load32(base, world + g::kObjOuter) : 0));
    }
    return;
  }
  if (t.phase == Phase::kFinished) {
    g_guard.on = false;
    return;
  }
  t_tour_thread = true;
  const auto now = Clock::now();
  if (tick_ms >= 100) t.last_long_frame = now;
  if (tick_ms >= REXCVAR_GET(masseffect_tour_load_ms)) t.long_tick = true;
  if (world != t.last_world) {
    t.last_world = world;
    t.pawn_frames = 0;
    if (world) ++t.worlds;
  }
  const uint32_t controller = LocalController(base);
  const uint32_t pawn = PawnOf(base, controller);
  if (pawn) {
    ++t.pawn_frames;
    t.last_pawn = now;
  } else {
    t.pawn_frames = 0;
  }
  const uint32_t want_frames = uint32_t(REXCVAR_GET(masseffect_tour_frames));
  // Death guard: on from the tour's start (after the start wait) to its end; follows the current pawn/controller.
  if (t.phase != Phase::kWaitStart && REXCVAR_GET(masseffect_tour_no_death) && !t.diag) {
    if (!g_guard.on) {
      for (const std::string& n : Split(REXCVAR_GET(masseffect_tour_no_death_names))) g_guard.names.insert(n);
      for (const std::string& n : Split(REXCVAR_GET(masseffect_tour_no_death_any))) g_guard.any.insert(n);
      REXLOG_INFO("[tour] guard on: {} name(s) skipped on the player, {} on any object", g_guard.names.size(),
                  g_guard.any.size());
    }
    g_guard.on = true;
    g_guard.pawn = pawn;
    g_guard.controller = controller;
  }
  // Health keeper: from the tour's start to its end (discovery mode: found and counted, not written).
  if (t.phase != Phase::kWaitStart) {
    g_guard.behavior = 0;
    const bool settled = t.phase == Phase::kScan || t.phase == Phase::kNext || t.phase == Phase::kDwell ||
                         (t.phase == Phase::kSettle && t.pawn_frames >= want_frames);
    KeepHealth(t, base, pawn, REXCVAR_GET(masseffect_tour_keep_health) && !t.diag, settled);
  }

  switch (t.phase) {
    case Phase::kWaitStart: {
      // The first map of a new game is still in its intro and streaming for a while; an AT then is "handled" but
      // does nothing (first console run). Wait masseffect_tour_start_s with a pawn, as tools/me1_location.sh did.
      const int32_t want_world = REXCVAR_GET(masseffect_exec_world);
      if (!exec_done || (want_world && t.worlds < uint32_t(want_world)) || !pawn) {
        t.start_pawn = false;
        return;
      }
      if (!t.start_pawn) {
        t.start_pawn = true;
        t.start_pawn_since = now;
        Status("waiting " + std::to_string(REXCVAR_GET(masseffect_tour_start_s)) + " s in the start map");
      }
      if (t.pawn_frames < want_frames || Since(t.start_pawn_since, now) < REXCVAR_GET(masseffect_tour_start_s)) return;
      t.tour_start = now;
      REXLOG_INFO("[tour] start in world #{} ({:08X}, package {})", t.worlds, world, PackageOf(base, world));
      Run(ctx, base, "EnableCheats");
      t.phase = Phase::kTravel;
      return;
    }
    case Phase::kTravel: {
      const std::string& entry = t.maps[t.map_index];
      t.travel_world = world;
      t.travel_level = Plausible(world) ? Load32(base, world + g::kWorldPersistentLevel) : 0;
      t.travel_package = Upper(PackageOf(base, world));
      t.long_tick = false;
      t.phase_start = now;
      t.last_at = now;
      t.at_count = 1;
      t.pawn_frames = 0;
      if (entry == "here" || entry == "HERE") {
        t.map = ObjectName(base, Plausible(world) ? Load32(base, world + g::kObjOuter) : 0);
        t.phase = Phase::kSettle;
        t.pawn_frames = pawn ? 1 : 0;
      } else {
        t.map = entry.substr(0, entry.find(' '));
        t.target = Upper(t.map);
        SetLabel(t.map + " travel");
        REXLOG_INFO("[tour] map {}/{}: AT {} (from package {}) | profile block {}", t.map_index + 1, t.maps.size(),
                    entry, t.travel_package, RexSwitchPerfReportIndex());
        Status("map " + std::to_string(t.map_index + 1) + "/" + std::to_string(t.maps.size()) + ": AT " + entry);
        Run(ctx, base, "AT " + entry);
        t.phase = Phase::kWaitArrival;
      }
      return;
    }
    case Phase::kWaitArrival: {
      // Arrival = the world changed since AT: a new GWorld or persistent level, or another package name. When the
      // names can be read the package must also be the target map; a long Tick then also counts (travel to the
      // map already loaded can reuse every address). A long Tick alone (a hitch) is not an arrival.
      const std::string package = Upper(PackageOf(base, world));
      const uint32_t level = Plausible(world) ? Load32(base, world + g::kWorldPersistentLevel) : 0;
      const bool names = g_name_format.ok && package != "?" && package != "NONE";
      const bool pointers = world != t.travel_world || level != t.travel_level;
      const bool arrived = world && pawn &&
                           (names ? package == t.target && (pointers || package != t.travel_package || t.long_tick)
                                  : pointers);
      if (arrived) {
        REXLOG_INFO("[tour] map {}: arrived (package {}, GWorld {:08X}{}{}) after {:.1f} s, {} AT", t.map, package,
                    world, pointers ? ", new world/level" : "", t.long_tick ? ", long Tick" : "",
                    Since(t.phase_start, now), t.at_count);
        Status("map " + t.map + ": arrived");
        t.phase = Phase::kSettle;
        t.phase_start = now;
        t.pawn_frames = 0;
        return;
      }
      if (Since(t.phase_start, now) > REXCVAR_GET(masseffect_tour_arrival_s)) {
        REXLOG_WARN("[tour] map {}: no arrival within {} s after {} AT (package {}); skipped", t.map,
                    REXCVAR_GET(masseffect_tour_arrival_s), t.at_count, package);
        BeginMap(t);
        t.map_start = now;
        FinishMap(t, base, "no arrival");
        return;
      }
      // Still the old map (or a load that did not happen): run AT again, once the game is not loading.
      if (Since(t.last_at, now) > REXCVAR_GET(masseffect_tour_retry_s) && pawn && Since(t.last_long_frame, now) > 2.0) {
        t.last_at = now;
        ++t.at_count;
        t.long_tick = false;
        REXLOG_INFO("[tour] map {}: still in package {} after {:.0f} s: AT again ({})", t.map, package,
                    Since(t.phase_start, now), t.at_count);
        Run(ctx, base, "AT " + t.maps[t.map_index]);
      }
      return;
    }
    case Phase::kSettle: {
      if (Since(t.phase_start, now) > REXCVAR_GET(masseffect_tour_arrival_s)) {
        REXLOG_WARN("[tour] map {}: no pawn for {} frames within {} s; skipped", t.map, want_frames,
                    REXCVAR_GET(masseffect_tour_arrival_s));
        BeginMap(t);
        t.map_start = now;
        FinishMap(t, base, "no pawn");
        return;
      }
      if (t.pawn_frames < want_frames) return;
      const uint32_t package = Plausible(world) ? Load32(base, world + g::kObjOuter) : 0;
      REXLOG_INFO("[tour] map {}: world package {} (Outer check: expected the map name), pawn {} ({}), "
                  "controller {} | profile block {}",
                  t.map, ObjectName(base, package), ObjectName(base, pawn), ClassOf(base, pawn).name,
                  ClassOf(base, controller).name, RexSwitchPerfReportIndex());
      me::sprof::DumpAndReset(base, t.map + " (loading)");
      if (controller != t.arrival_controller) {
        for (const std::string& c : t.arrival_commands) Run(ctx, base, c);
        t.arrival_controller = controller;
      } else {
        REXLOG_INFO("[tour] map {}: same controller as before, arrival commands not repeated (toggles)", t.map);
      }
      if (REXCVAR_GET(masseffect_tour_ghost) && !t.diag) Run(ctx, base, "Ghost");
      BeginMap(t);
      t.map_start = now;
      t.last_level_change = now;
      t.phase = Phase::kScan;
    }
      [[fallthrough]];
    case Phase::kScan: {
      if (!Scan(t, base, world)) return;  // more levels on the next frames
      if (t.diag) {
        const Vec p = pawn ? LoadVec(base, pawn + g::kActorLocation) : Vec{};
        const uint32_t rot = controller + g::kActorRotation;
        uint32_t pending = 0;
        const size_t visible = VisibleLevels(base, Levels(base, world), &pending).size();
        REXLOG_INFO("[tour] diag {}: levels visible {}, pending (ULevel+364 != 0) {}", t.map, visible, pending);
        REXLOG_INFO("[tour] diag {}: pawn at ({:.0f}, {:.0f}, {:.0f}) physics {}, controller rotation ({}, {}, {})",
                    t.map, p.x, p.y, p.z, pawn ? int(Load8(base, pawn + g::kActorPhysics)) : -1,
                    controller ? int32_t(Load32(base, rot)) : 0, controller ? int32_t(Load32(base, rot + 4)) : 0,
                    controller ? int32_t(Load32(base, rot + 8)) : 0);
        for (size_t i = 0; i < t.stops.size() && i < 16; ++i) {
          const Stop& s = t.stops[i];
          REXLOG_INFO("[tour] diag {}: stop {} {} {} at ({:.0f}, {:.0f}, {:.0f}) level {}", t.map, i,
                      s.kind == kStream ? "stream" : "nav", s.cls, s.at.x, s.at.y, s.at.z,
                      ObjectName(base, s.level));
        }
        FinishMap(t, base, "discovery mode");
        return;
      }
      t.phase = Phase::kNext;
      return;
    }
    case Phase::kNext: {
      const int32_t max_points = REXCVAR_GET(masseffect_tour_max_points);
      if (Since(t.map_start, now) > REXCVAR_GET(masseffect_tour_map_s)) return FinishMap(t, base, "map time cap");
      if (t.visited >= size_t(max_points)) return FinishMap(t, base, "stop cap");
      if (!pawn) {
        if (Since(t.last_pawn, now) > 60.0) FinishMap(t, base, "no pawn for 60 s");
        return;
      }
      const std::vector<uint32_t> levels = Levels(base, world);
      uint32_t pending = 0;
      const std::set<uint32_t> loaded = VisibleLevels(base, levels, &pending);
      const size_t next = PickNext(t, LoadVec(base, pawn + g::kActorLocation), loaded);
      if (next == SIZE_MAX) {
        if (!t.idle) t.idle = true, t.idle_since = now;
        if (pending && Since(t.idle_since, now) < REXCVAR_GET(masseffect_tour_stream_wait_ms) / 1000.0)
          return;  // a level is becoming visible: its stops may follow
        t.idle = false;
        return FinishMap(t, base, "all stops of loaded levels visited");
      }
      t.idle = false;
      Stop& s = t.stops[next];
      s.done = true;
      t.cur = next;
      ++t.visited;
      ++t.stops_done;
      const Vec to{s.at.x, s.at.y, s.at.z + float(REXCVAR_GET(masseffect_tour_z))};
      if (REXCVAR_GET(masseffect_tour_pin) && base[pawn + g::kActorPhysics] != g::kPhysRigidBody)
        base[pawn + g::kActorPhysics] = g::kPhysNone;
      const bool moved = FarMove(ctx, base, pawn, to);
      if (!moved && ++t.move_failures <= 20)
        REXLOG_WARN("[tour] map {}: FarMoveActor refused the move to stop {} ({})", t.map, t.visited, s.cls);
      t.touch_done = s.kind != kStream || !REXCVAR_GET(masseffect_tour_touch_streams);
      const size_t total = std::min(t.stops.size(), size_t(max_points));
      const std::string label = t.map + " " + std::to_string(t.visited) + "/" + std::to_string(total);
      SetLabel(label);
      Status("map " + label);
      REXLOG_INFO("[tour] map {}: point {}/{} {} {} at ({:.0f}, {:.0f}, {:.0f}){} | profile block {}", t.map,
                  t.visited, total, s.kind == kStream ? "stream" : "nav", s.cls, s.at.x, s.at.y, s.at.z,
                  moved ? "" : " (move refused)", RexSwitchPerfReportIndex());
      t.yaw0 = controller ? int32_t(Load32(base, controller + g::kActorRotation + 4)) : 0;
      t.extended = false;
      t.phase_start = now;
      t.phase = Phase::kDwell;
      return;
    }
    case Phase::kDwell: {
      if (!pawn || t.cur >= t.stops.size()) {
        t.phase = Phase::kNext;
        return;
      }
      const Stop& s = t.stops[t.cur];
      const double turn = REXCVAR_GET(masseffect_tour_dwell_ms) / 1000.0;  // one full turn of the view
      const double dwell =
          s.kind == kStream ? std::max(turn, REXCVAR_GET(masseffect_tour_stream_dwell_ms) / 1000.0) : turn;
      const double in = Since(t.phase_start, now);
      if (REXCVAR_GET(masseffect_tour_pin)) {
        if (base[pawn + g::kActorPhysics] != g::kPhysRigidBody) base[pawn + g::kActorPhysics] = g::kPhysNone;
        const Vec to{s.at.x, s.at.y, s.at.z + float(REXCVAR_GET(masseffect_tour_z))};
        if (Dist2(LoadVec(base, pawn + g::kActorLocation), to) > 25.0f) FarMove(ctx, base, pawn, to);
      }
      if (controller && REXCVAR_GET(masseffect_tour_turn)) {
        const int32_t yaw = t.yaw0 + int32_t(std::fmod(in / turn, 1.0) * 65536.0);
        Store32(base, controller + g::kActorRotation, uint32_t(REXCVAR_GET(masseffect_tour_pitch)));
        Store32(base, controller + g::kActorRotation + 4, uint32_t(yaw));
        Store32(base, controller + g::kActorRotation + 8, 0);
      }
      const uint64_t sig = LevelSignature(Levels(base, world));
      if (sig != t.level_signature) {
        t.last_level_change = now;
        Scan(t, base, world);
      }
      const double quiet = REXCVAR_GET(masseffect_tour_quiet_ms) / 1000.0;
      const double wait = REXCVAR_GET(masseffect_tour_stream_wait_ms) / 1000.0;
      if (!t.touch_done) {
        // One touch per trigger stop, only when nothing streams: no long frame and no level change for `quiet`, no
        // level with a pending visibility change, and touch_gap since the last touch. Then the stay restarts, so the
        // stop lasts until the touch's own loads have settled (the exit test below).
        uint32_t pending = 0;
        const std::set<uint32_t> visible = VisibleLevels(base, Levels(base, world), &pending);
        const bool calm = Since(t.last_long_frame, now) >= quiet && Since(t.last_level_change, now) >= quiet &&
                          !pending && Since(t.last_touch, now) >= REXCVAR_GET(masseffect_tour_touch_gap_ms) / 1000.0;
        if (calm && visible.count(s.level)) {
          const bool ok = TouchStreamTrigger(ctx, base, s.actor);
          t.touch_done = true;
          t.last_touch = now;
          t.phase_start = now;
          ok ? ++t.touches : ++t.touches_skipped;
          REXLOG_INFO("[tour] map {}: stop {} trigger {}", t.map, t.visited, ok ? "touched" : "not touched (vtable)");
          return;
        }
        if (in < wait && visible.count(s.level)) return;  // pinned, waiting for a quiet moment
        t.touch_done = true;
        ++t.touches_skipped;
        REXLOG_INFO("[tour] map {}: stop {} trigger not touched ({})", t.map, t.visited,
                    visible.count(s.level) ? "streaming never quiet" : "its level is not visible");
      }
      if (in < dwell) return;
      // Leave only when streaming has settled: moving on (and touching the next trigger) while levels still load or
      // unload is the likely cause of the game's own crash in BIOA_STA00 (docs/tour.md, third console run).
      const bool busy = Since(t.last_long_frame, now) < quiet || Since(t.last_level_change, now) < quiet;
      if (busy && in < dwell + wait) {
        if (!t.extended) {
          t.extended = true;
          REXLOG_INFO("[tour] map {}: stop {} extended (loading)", t.map, t.visited);
        }
        return;
      }
      t.phase = Phase::kNext;
      return;
    }
    case Phase::kMapDone:
    case Phase::kFinished:
      return;
  }
}

}  // namespace me::tour
