// Mass Effect - run game console commands from the settings file (location tests, docs/location-tests.md).
//
// WHY
//   To measure every location on the console the test needs to reach it quickly and the same way each time. The
//   game ships the developers' travel command: BioCheatManager.AT(name newArea, name startPoint), the command of the
//   debug travel menu in Coalesced.ini ([BIOC_Base.BioPlayerInput] DebugMenu / "AT BIOA_STA00 start_STA20_01"). Its
//   script calls the native ABioWorldInfo::MoveToArea, which stores the start point in the BioSaveGame object
//   (m_DesiredStartPoint, read back by BioSPGame.FindPlayerStart) and runs "DEFER BIOAREATRANSITION <map>", the same
//   transition the game uses itself. The cheat manager exists once PlayerController.EnableCheats ran (AddCheats only
//   checks NetMode == NM_Standalone; CheatClass is BioCheatManager in Default__BioPlayerController).
//
// HOW
//   After UGameEngine::Tick (vtable slot 71 of the UGameEngine vtable, sub_825E6978), on the game thread: once the
//   player controller has a pawn in the configured world, each '|'-separated command goes through
//   ULocalPlayer::Exec (sub_824BDA68), the function a typed console command reaches: viewport client, UWorld, then
//   the script exec functions of the pawn, PlayerInput, PlayerController and CheatManager, then GEngine->Exec.
//   - GEngine 0x82EAEA1C: UGameEngine*, GamePlayers TArray at +748 (Data) / +752 (Num) (UGameEngine::Init stores
//     Client at +744 and GameViewport at +760; ABioWorldInfo::MoveToArea calls GEngine's FExec at +60).
//   - ULocalPlayer: FExec subobject at +60 (Exec's `this`), Actor (PlayerController) at +64; PlayerController Pawn at
//     +492 (all read by ULocalPlayer::Exec itself).
//   - GLog 0x82E624B4 (the output device MoveToArea passes), GWorld 0x82EAEA94 (a new UWorld per loaded map).
//   The command string is UTF-16BE on the guest stack below the hook's frame (the original Tick has returned).
//
// ARRIVAL
//   A new map normally shows up as a new GWorld pointer. The allocator can give the new UWorld the address of the
//   old one (seen on the console: the travel from the prologue to BIOA_STA00 kept 40DB72C0), so with
//   masseffect_exec_load_ms > 0 a Tick of the original that runs at least that long after the commands (the blocking
//   map load of the area transition happens inside one Tick) also counts as the arrival. Measured with steady_clock
//   (cntpct_el0 through libnx), only around the original call.
//
// Off by default (both command cvars empty): the hook then only calls the original.
//
// The same hook drives the location tour and the UnrealScript profiler's per-map tables (me_tour.cpp, docs/tour.md):
// me::tour::Tick runs after the original Tick every frame, and me::exec::RunConsoleCommand is its way to the console.

#include "me_tour.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(masseffect_exec, "", "Mass Effect",
                      "Location tests: game console commands run once, separated by '|', e.g. "
                      "\"EnableCheats|AT BIOA_STA00 start_STA20_01\" (docs/location-tests.md); empty = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(masseffect_exec_arrival, "", "Mass Effect",
                      "Location tests: commands run once in the first world loaded after masseffect_exec ran (after "
                      "the travel), e.g. \"God|PlayersOnly\"; empty = none")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_exec_world, 2, "Mass Effect",
                     "Location tests: run masseffect_exec in the N-th loaded world or later (1 = the start menu "
                     "world, 2 = the first map after it); 0 = any world")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_exec_frames, 300, "Mass Effect",
                     "Location tests: frames the player must have a pawn in that world before the commands run")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_exec_load_ms, 0, "Mass Effect",
                     "Location tests: after masseffect_exec ran, a game Tick at least this long (ms) counts as the "
                     "arrival in a new map even when GWorld keeps its address (the map load blocks inside one Tick); "
                     "tools/me1_location.sh uses 1500; 0 = only GWorld changes count")
    .range(0, 600000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr uint32_t kGEngine = 0x82EAEA1C;
constexpr uint32_t kGLog = 0x82E624B4;
constexpr uint32_t kGWorld = 0x82EAEA94;
constexpr uint32_t kLocalPlayerExec = 0x824BDA68;
constexpr uint32_t kGamePlayersData = 748, kGamePlayersNum = 752;
constexpr uint32_t kPlayerFExec = 60, kPlayerActor = 64, kControllerPawn = 492;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

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

struct Stage {
  std::vector<std::string> commands;
  bool done = false;
};

struct State {
  bool configured = false;
  Stage first, arrival;
  uint32_t world = 0;   // GWorld seen last frame
  uint32_t worlds = 0;  // distinct worlds seen so far
  uint32_t frames = 0;  // frames with a pawn in the current world
  uint32_t fired_world = 0;
  bool load_seen = false;  // a long Tick after the commands ran (masseffect_exec_load_ms)
};

State& Get() {
  static State s = [] {
    State st;
    st.first.commands = Split(REXCVAR_GET(masseffect_exec));
    st.arrival.commands = Split(REXCVAR_GET(masseffect_exec_arrival));
    st.first.done = st.first.commands.empty();
    st.arrival.done = st.arrival.commands.empty();
    st.configured = !st.first.done || !st.arrival.done;
    if (st.configured)
      REXLOG_INFO("[exec] {} command(s), {} on arrival; world >= {}, after {} frames with a pawn",
                  st.first.commands.size(), st.arrival.commands.size(), REXCVAR_GET(masseffect_exec_world),
                  REXCVAR_GET(masseffect_exec_frames));
    return st;
  }();
  return s;
}

}  // namespace

REX_EXTERN(__imp__sub_825E6978);  // UGameEngine::Tick
REX_EXTERN(sub_824BDA68);         // ULocalPlayer::Exec(FExec* this, const TCHAR* Cmd, FOutputDevice& Ar)

namespace {

// ULocalPlayer::Exec with the command on the guest stack. Returns the guest's UBOOL (handled).
uint32_t RunCommand(PPCContext& ctx, uint8_t* base, uint32_t fexec, const std::string& command) {
  const uint64_t r1 = ctx.r1.u64, r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, lr = ctx.lr;
  const uint32_t bytes = (uint32_t(command.size() + 1) * 2 + 15u) & ~15u;
  // 0x80 bytes of scratch above the callee's frame (it stores its arguments at r1+20..), then the string.
  const uint32_t sp = (ctx.r1.u32 - 0x100u - bytes) & ~15u;
  const uint32_t text = sp + 0x80;
  for (size_t i = 0; i <= command.size(); ++i) {
    const uint16_t ch = i < command.size() ? uint16_t(uint8_t(command[i])) : 0;
    base[text + 2 * i] = uint8_t(ch >> 8);
    base[text + 2 * i + 1] = uint8_t(ch);
  }
  Store32(base, sp, ctx.r1.u32);  // back chain
  ctx.r1.u64 = sp;
  ctx.r3.u64 = fexec;
  ctx.r4.u64 = text;
  ctx.r5.u64 = Load32(base, kGLog);
  sub_824BDA68(ctx, base);
  const uint32_t handled = ctx.r3.u32;
  ctx.r1.u64 = r1;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.lr = lr;
  return handled;
}

uint32_t LocalPlayerFExec(const uint8_t* base) {
  const uint32_t engine = Load32(base, kGEngine);
  if (!engine || int32_t(Load32(base, engine + kGamePlayersNum)) <= 0) return 0;
  const uint32_t players = Load32(base, engine + kGamePlayersData);
  const uint32_t player = players ? Load32(base, players) : 0;
  if (!player) return 0;
  const uint32_t vtable = Load32(base, player + kPlayerFExec);
  return vtable && Load32(base, vtable) == kLocalPlayerExec ? player + kPlayerFExec : 0;
}

void RunStage(PPCContext& ctx, uint8_t* base, Stage& stage, uint32_t fexec, const char* name) {
  stage.done = true;
  for (const std::string& command : stage.commands) {
    const uint32_t handled = RunCommand(ctx, base, fexec, command);
    REXLOG_INFO("[exec] {}: \"{}\" -> {}", name, command, handled ? "handled" : "not handled");
  }
}

}  // namespace

namespace me::exec {
uint32_t RunConsoleCommand(PPCContext& ctx, uint8_t* base, const std::string& command) {
  const uint32_t fexec = LocalPlayerFExec(base);
  return fexec ? RunCommand(ctx, base, fexec, command) : 0;
}
}  // namespace me::exec

REX_HOOK_RAW(sub_825E6978) {
  State& s = Get();
  const int32_t load_ms = REXCVAR_GET(masseffect_exec_load_ms);
  const bool timed = s.configured && s.first.done && !s.arrival.done && load_ms > 0;
  const bool tour = me::tour::Enabled();
  const auto tick_start =
      timed || tour ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
  __imp__sub_825E6978(ctx, base);
  const int64_t ms =
      timed || tour
          ? int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tick_start)
                        .count())
          : 0;
  me::tour::Tick(ctx, base, ms, s.first.done);
  if (!s.configured || (s.first.done && s.arrival.done)) return;
  if (timed) {
    if (ms >= load_ms) {
      s.load_seen = true;
      s.frames = 0;
      REXLOG_INFO("[exec] Tick of {} ms after the commands: counted as the map load (GWorld {:08X})", int64_t(ms),
                  Load32(base, kGWorld));
    }
  }

  const uint32_t world = Load32(base, kGWorld);
  if (world != s.world) {
    s.world = world;
    s.frames = 0;
    if (world) {
      ++s.worlds;
      REXLOG_INFO("[exec] world #{} ({:08X})", s.worlds, world);
    }
  }
  const uint32_t engine = Load32(base, kGEngine);
  if (!world || !engine || int32_t(Load32(base, engine + kGamePlayersNum)) <= 0) return;
  const uint32_t players = Load32(base, engine + kGamePlayersData);
  const uint32_t player = players ? Load32(base, players) : 0;
  const uint32_t controller = player ? Load32(base, player + kPlayerActor) : 0;
  const uint32_t pawn = controller ? Load32(base, controller + kControllerPawn) : 0;
  if (!pawn) {
    s.frames = 0;
    return;
  }
  if (++s.frames < uint32_t(REXCVAR_GET(masseffect_exec_frames))) return;

  const uint32_t fexec = player + kPlayerFExec;
  const uint32_t vtable = Load32(base, fexec);
  if (!vtable || Load32(base, vtable) != kLocalPlayerExec) {
    REXLOG_WARN("[exec] unexpected local player FExec vtable {:08X} (slot 0 {:08X}); commands not run", vtable,
                vtable ? Load32(base, vtable) : 0);
    s.first.done = s.arrival.done = true;
    return;
  }
  if (!s.first.done) {
    const int32_t want = REXCVAR_GET(masseffect_exec_world);
    if (want && s.worlds < uint32_t(want)) return;
    RunStage(ctx, base, s.first, fexec, "commands");
    s.fired_world = s.worlds;
    return;
  }
  if (!s.arrival.done && (s.worlds > s.fired_world || s.load_seen)) RunStage(ctx, base, s.arrival, fexec, "arrival");
}
