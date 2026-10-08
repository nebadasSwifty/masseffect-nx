// Mass Effect - location tour (me_tour.cpp) and UnrealScript profiler (me_script_prof.cpp): the calls between them
// and the console-command hook (me_console_exec.cpp). docs/tour.md.
#pragma once

#include <rex/ppc/func.h>

#include <cstdint>
#include <string>

struct PPCContext;

namespace me::exec {
// ULocalPlayer::Exec of the first local player with `command` (me_console_exec.cpp). Game thread only, from the
// UGameEngine::Tick hook. Returns the guest's "handled" result; 0 when there is no local player.
uint32_t RunConsoleCommand(PPCContext& ctx, uint8_t* base, const std::string& command);
}  // namespace me::exec

namespace me::tour {
// Called by the UGameEngine::Tick hook after the original Tick, every frame, on the game thread.
//   tick_ms        duration of the original Tick (a map load blocks inside one Tick)
//   exec_done      true once masseffect_exec ran (or it is not configured)
void Tick(PPCContext& ctx, uint8_t* base, int64_t tick_ms, bool exec_done);
bool Enabled();
// Hook body of UObject::ProcessInternal: while the tour runs (masseffect_tour_no_death), skips the death functions of
// the player's pawn and controller (masseffect_tour_no_death_names) and logs other death-like calls once.
void ScriptGuard(PPCContext& ctx, uint8_t* base, PPCFunc* original);
// "Name" or "Name_N" of a guest object from the FName table ("?" when the table cannot be read). Game thread only.
std::string ObjectNameOf(const uint8_t* base, uint32_t object);
}  // namespace me::tour

namespace me::sprof {
enum Via : uint32_t { kViaCallFunction = 0, kViaProcessEvent = 1 };
// Hook body of UObject::CallFunction / UObject::ProcessEvent: counts `function` and calls `original`.
void Wrap(PPCContext& ctx, uint8_t* base, uint32_t function, Via via, PPCFunc* original);
// Game thread: marks the thread whose calls are counted (the VM runs there). Called from the Tick hook.
void NoteGameThread();
// Logs the top-N table ("[script_prof] <label>: ...") and starts a new interval. No-op when the profiler is off.
void DumpAndReset(uint8_t* base, const std::string& label);
// GWorld is now `world`, named `label`: dumps (DumpAndReset) the previous world's table under its own label.
void NoteWorld(uint8_t* base, uint32_t world, const std::string& label);
bool Enabled();
}  // namespace me::sprof

// Console profiler (sdk/src/ui/switch_perf.cpp): the label written under each rex_profile.log block header, and the
// number of blocks written so far (to line up "[tour]" log lines with profile blocks).
extern "C" void RexSwitchPerfSetLabel(const char* label);
extern "C" uint32_t RexSwitchPerfReportIndex(void);
