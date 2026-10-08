# Location tour, per-map profiles and UnrealScript hot spots

One unattended game session that travels to every location, moves Shepard through each map so that everything the
map can show is drawn at least once, and records per map: the pipeline cache (for the shipped prewarm list,
[cold-start-hitches.md](cold-start-hitches.md) section C), the console stack profile, the UnrealScript functions the
game runs, and a census of the map's actors and Kismet objects.

Status: written 2026-10-08. First console run 2026-10-09 (RU build ru_glob20, discovery mode, eden + normandy):
the object/level layout was right (`actor Outer == level` for 4317 of 4317 actors, Kismet objects found at +216), but
(1) no name could be read (every class `?`), (2) `AT` did not travel and a hitch was taken for the arrival, (3) the
script profiler counted 1-2 calls per map, (4) the bot could not read the open log. Fixed the same day (section 8);
the fixes are syntax-checked, not yet run.

## 1. Pieces

| Part | File | What it does |
|---|---|---|
| Tour driver | `app/src/native/me_tour.cpp` | state machine on the game thread, called after every `UGameEngine::Tick` |
| Addresses | `app/src/native/me_tour_guest.h` (EN), `editions/ru/overlay/app/src/native/me_tour_guest.h` (RU) | guest globals, functions, UE3 offsets; the script-profiler hooks |
| Interfaces | `app/src/native/me_tour.h` | tour / profiler / console-command calls between the files |
| Script profiler | `app/src/native/me_script_prof.cpp` | hooks on `UObject::CallFunction` and `UObject::ProcessEvent`, top-N tables per map |
| Console hook | `app/src/native/me_console_exec.cpp` (EN and RU overlay) | now also calls `me::tour::Tick` and exports `me::exec::RunConsoleCommand` |
| Profile label | `sdk/src/ui/switch_perf.cpp` | `RexSwitchPerfSetLabel` / `RexSwitchPerfReportIndex`; each `rex_profile.log` block gets a line `block N \| label: <map> i/n` |
| Bot | `mass-effect-recomp/tools/me1_tour.sh`, `tools/me1_tour_summary.py` | new game once, wait for `[tour] done`, fetch logs/profile/cache, prewarm list, per-map summary |

Wiring (not done, CMake is yours): add `src/native/me_tour.cpp` and `src/native/me_script_prof.cpp` to
`MASSEFFECT_SOURCES` in `app/CMakeLists.txt` (next to `me_console_exec.cpp`). The overlay has no CMakeLists, so the
same two lines serve the RU build. The two new hooks (`CallFunction`, `ProcessEvent`) and the new direct call
(`FarMoveActor`) change the hooked set: the next build of each edition regenerates the code (`tools/edition.sh` does
this; for EN run `tools/codegen.sh`).

## 2. How the tour works

All of it runs on the game thread right after `UGameEngine::Tick` returns (the hook in `me_console_exec.cpp`), so it
never races the game. Guest functions are called with a frame below the hook's stack pointer, as `masseffect_exec`
already does for console commands.

1. **Start.** `masseffect_exec` runs first (the bot sets it to `EnableCheats`, world 2, 150 frames, as the location
   tests). When it has run and the player has had a pawn in world >= `masseffect_exec_world` for
   `masseffect_tour_start_s` seconds (default 60: the new game's first map is still in its intro, an `AT` then is
   "handled" and does nothing), the tour runs `EnableCheats` itself and starts.
2. **Travel.** For each entry of `masseffect_tour` (`"MAP START|MAP START|..."`, the arguments of
   `BioCheatManager.AT`; `here` = the current map) it runs `AT MAP START`. Arrival = the world changed since the `AT`
   (another `GWorld`, another persistent level, or another package name) **and**, when names can be read, the world's
   package is the target map. A `Tick` of at least `masseffect_tour_load_ms` counts only together with the target
   package name (travel within the map already loaded may reuse every address); a long Tick alone is a hitch. Without
   an arrival the `AT` is repeated every `masseffect_tour_retry_s` (default 45 s, not during loads), and the map is
   skipped after `masseffect_tour_arrival_s`. Then `masseffect_tour_frames` frames with a pawn, then the arrival
   commands (`masseffect_tour_arrival`, default `God`) and, if set, `Ghost`.
3. **Scan.** Every actor of every loaded level (`GWorld->Levels`, plus the persistent level) is classified by its class
   and the class's super chain names (`FName` text from the name table): actors derived from `NavigationPoint`
   (PathNode, PlayerStart, CoverLink, Bio nav points, ...) and `BioTriggerStream` volumes become stops. Nav points
   closer than `masseffect_tour_spacing` to a kept stop are dropped. Each scan also counts every actor class and the
   Kismet objects of the levels' `GameSequences` (recursively into sub-sequences) for the census.
4. **Visit.** Next stop = the nearest unvisited nav point of a level that is loaded now; then the nearest untouched
   streaming trigger; then whatever is left. ME1 streams by `BioTriggerStream` touches (Kismet/streaming states), not by
   viewer distance, so the triggers are what loads the rest of a map. The pawn is moved with
   `UWorld::FarMoveActor(pawn, stop + masseffect_tour_z, bTest=0, bNoCheck=1)`, the engine's own teleport (it also
   updates touching volumes when the pawn collides), its `Physics` is set to `PHYS_None` (not for a rigid-body
   vehicle) and it is put back every frame while it stays (`masseffect_tour_pin`). For a trigger stop the trigger's
   own `Touch` (vtable `DoTouch`, what `ABioTriggerStream.execDoTouch` calls) is also run. During the stay the
   controller's yaw turns 360 degrees in `masseffect_tour_dwell_ms`, pitch `masseffect_tour_pitch`.
5. **Streaming.** The stay is extended (at most `masseffect_tour_stream_wait_ms`) while a frame was 100 ms or longer in
   the last second (blocking loads) or the set of loaded levels changed in the last second. A changed set is
   rescanned at once and its stops join the list.
6. **Leave.** After all stops, `masseffect_tour_max_points` stops or `masseffect_tour_map_s` seconds (or 60 s without a
   pawn), the census and the script-profiler table are logged and the next `AT` runs. After the last map:
   `[tour] done: N map(s), M stops in S s`; the game then stays where it is.

Log lines (all `[tour]`): `map i/n: AT ...`, `map X: arrived (...)`, `map X: world package P (...)`,
`map X: scan of K new level(s) ...; actor Outer == level for A of B actors`, `map X: point i/n nav PathNode at (x, y, z)
| profile block B`, `map X: stop i extended (loading)`, `map X finished (reason): ...`, `census X: ...`,
`census X actors: ...`, `census X kismet: ...`, `census X scripted: Matinee (SeqAct_Interp) ...`, `done: ...`.

`profile block B` = number of `rex_profile.log` blocks written when the line was logged (the stop's time falls into
block B+1 and later). Each block also carries the label that was current when it was written (`<map> i/n`,
`<map> travel`, `tour done`).

## 3. UnrealScript profiler

`masseffect_script_prof = 1` counts calls per `UFunction`, `= 2` also measures self and inclusive time
(`armGetSystemTick`, i.e. `cntpct_el0`; `cntvct_el0` traps on Horizon). Hooks:

- `UObject::CallFunction(Stack, Result, Function)`: every script-to-script call, and every call of a native function
  that has no fixed bytecode (`iNative == 0`): these are the native C++ `exec` functions called from script, marked
  `native` in the table (`FUNC_Native`, 0x400 at `UFunction+140`).
- `UObject::ProcessEvent(Function, Parms)`: every call from C++ into script (events, timers, delegates), marked `event`.
- Not seen: natives with a fixed bytecode (operators, `iNative != 0`), which go straight through `GNatives`.

Only the game thread is counted (the thread that runs the Tick hook). Names (`Outer.Name`, i.e. `Class.Function` or
`State.Function`) are resolved once per function. Output per map (and `<map> (loading)` for the load itself):

```
[script_prof] BIOA_PRO00: 812345 calls of 2130 functions in 412.0 s, top 50 by calls
[script_prof] BIOA_PRO00 |   1 |    123456 calls     299.6/s | event | BioPawn.Tick
[script_prof] BIOA_PRO00 |   1 |     ... self   812.3 ms   1.97 ms/s | incl ... | script | BioAiController.X   (mode 2)
```

Without the tour it dumps one table per `GWorld` change. Off (default) it costs one relaxed atomic load per call plus
the hook's call level; both functions keep the old argument ABI because they are hooked. If that shows in a profile,
drop `me_script_prof.cpp` from the build (the tour does not need it).

## 4. Cvars

| Cvar | Default | Meaning |
|---|---|---|
| `masseffect_tour` | `""` (off) | `MAP START` entries separated by `\|`; `here` = current map |
| `masseffect_tour_diag` | `false` | discovery mode (section 5) |
| `masseffect_tour_arrival` | `"God"` | commands after each arrival |
| `masseffect_tour_dwell_ms` | 1500 | time per stop (one full turn) |
| `masseffect_tour_stream_wait_ms` | 20000 | longest extension of a stop while loading |
| `masseffect_tour_max_points` | 200 | stops per map |
| `masseffect_tour_spacing` | 1200 | Unreal units between kept nav points |
| `masseffect_tour_z` | 100 | height above the stop |
| `masseffect_tour_pitch` | -1200 | view pitch (65536 = 360 degrees) |
| `masseffect_tour_pin` | `true` | put the pawn back every frame, `PHYS_None` |
| `masseffect_tour_ghost` | `false` | also run `Ghost` (no collision: triggers then only by the direct Touch) |
| `masseffect_tour_touch_streams` | `true` | DoTouch of a `BioTriggerStream` at its stop: one per stop, only when quiet (below) |
| `masseffect_tour_touch_gap_ms` | 10000 | shortest time between two trigger touches |
| `masseffect_tour_turn` | `false` | write the controller Rotation (yaw turn); ME1's camera ignores it, the bot holds the right stick instead |
| `masseffect_tour_quiet_ms` | 2500 | leave a stop only after this long without a 100 ms frame and without a change of the loaded levels |
| `masseffect_tour_stream_dwell_ms` | 5000 | shortest stay at a streaming trigger |
| `masseffect_tour_no_death` | `true` | death guard (section "Death guard") |
| `masseffect_tour_keep_health` | `true` | keep the player's health and shields full every frame while the tour runs (section "Health keeper") |
| `masseffect_tour_frames` | 150 | frames with a pawn after an arrival |
| `masseffect_tour_start_s` | 60 | seconds with a pawn in the start world before the first `AT` |
| `masseffect_tour_retry_s` | 45 | `AT` again when the map has not changed after this long |
| `masseffect_tour_load_ms` | 1500 | Tick length that counts as the load of the target map (only with its package name) |
| `masseffect_tour_map_s` | 900 | seconds per map |
| `masseffect_tour_arrival_s` | 240 | seconds from the first `AT` to the arrival, then the map is skipped |
| `masseffect_script_prof` | 0 | 0 off, 1 calls, 2 calls + time |
| `masseffect_script_prof_top` | 50 | rows per table |

All are init-only (read from `masseffect.toml` at start).

## 5. Offsets and addresses

Found by static analysis of the decrypted `default.xex` of each edition (EN: AES + LZX; RU: unencrypted, basic
compression) and of the generated code. The native table (`int<Class>exec<Function>` UTF-16 names paired with thunk
addresses) gave the starting points; the EN->RU function map (`masseffect-nx-rus/rus_address_map.json`) and a diff of
the function bodies (only global addresses differ) gave the RU ones.

| | EN | RU | Found from |
|---|---|---|---|
| `GEngine` | `0x82EAEA1C` | `0x82EAEA3C` | me_console_exec.cpp |
| `GWorld` | `0x82EAEA94` | `0x82EAEAB4` | me_console_exec.cpp; `AActor::execSetLocation` loads it |
| `GNames` (TArray data, num at +4) | `0x82EC2608` | `0x82EC2628` | `FName::ToString` (EN `sub_82389790`, RU `sub_82389E90`), from the `Accessed None '%s'` message |
| `GNatives` | `0x82E9C5F8` | `0x82E9C618` | every exec thunk |
| `UWorld::FarMoveActor` | `sub_823C0468` | `sub_823C10F8` | called by `AActor::execSetLocation` (EN `sub_8237D688`, RU `sub_8237DBF0`) |
| `UObject::CallFunction` | `sub_823E8640` | `sub_823E8EC8` | FFrame construction, `iNative` / `FUNC_Native` tests, GNatives dispatch of the parameters |
| `UObject::ProcessEvent` | `sub_823E8D38` | `sub_823E95C0` | `FUNC_Native\|FUNC_Defined` test, probe mask on names 300..363, locals alloca + FFrame |
| `FActorIterator::operator++` (reference) | `sub_8225C318` | `sub_8225BF20` | used by `AActor::execAllActors` (EN `sub_8246EEF0`) |
| `UBioCheatManager::execSetLocation` (not used) | `sub_8291FF40` | `sub_82920378` | a native `SetLocation(vector)` cheat (vtable +284 of the cheat manager) |

| Offset | Value | Status |
|---|---|---|
| `UObject::Class` | +52 | verified (`execAllActors` IsA loop) |
| `UObject::Name` (FName index, number) | +44 / +48 | verified for UFunction/UProperty (`FName::ToString` of `GProperty+44`); same field for all objects |
| `UObject::Outer` | +40 | verified on the console (RU 2026-10-09: `actor Outer == level` for 4317 of 4317 actors) |
| `UField::SuperField` (class super chain) | +60 | verified (IsA loop) |
| `UStruct::Children`, `UField::Next` | +76, +64 | verified (`CallFunction` parameter loop) |
| `UProperty::PropertyFlags` (QWORD), `UProperty::Offset` | +76, +100 | static (`CallFunction`: `ld r11,76(prop)` flag tests, `Locals + lwz 100(prop)`); checked at run time on `Actor.Location/Rotation/Physics` (`[tour] reflection check ok`) |
| `UStructProperty::Struct` | found at run time | first word after `Offset` that points to a `ScriptStruct` (`[tour] reflection: UStructProperty::Struct at +N`) |
| `UFunction` flags / iNative / Func / ParmsSize | +140 / +144 / +168 / +158 | verified (`CallFunction`, `ProcessEvent`) |
| `FNameEntry` text (UTF-16BE) | +16 | verified (`FName::ToString`) |
| `UWorld` Levels data/num, PersistentLevel, CurrentLevel | +72/+76, +84, +88 | verified (iterator, FarMoveActor, `execGetGameSequence`) |
| `ULevel` Actors data/num | +60/+64 | verified |
| `ULevel` GameSequences data/num | +160/+164 | verified (`AWorldInfo::execGetGameSequence`) |
| `USequence::SequenceObjects` | +216 (found at run time) | heuristic (first TArray at +60..+1020 whose first elements have the sequence as Outer); RU console run: +216 |
| `AActor` Location / Rotation / Physics | +260 / +272 / +104 (byte) | verified (FarMoveActor, SetRotation; Physics values 7 and 10 tested there) |
| `AActor` bDeleteMe | +132 bit 0x10000000 | verified (`execAllActors` skips it) |
| `ABioTriggerStream` DoTouch | vtable +840 | verified (`execDoTouch`, both editions) |
| `ULocalPlayer` Actor, `APlayerController` Pawn, `UGameEngine` GamePlayers | +64, +492, +748/+752 | verified earlier (location tests) |
| Controller view = `Rotation` (+272) | | *assumed*: the camera follows the controller rotation as in UE3; if the view does not turn, BioCamera has its own |

### Discovery mode (first run of a new build)

`DIAG=1 zsh tools/me1_tour.sh` (or `masseffect_tour_diag = true`): per map only the scan, the census and these lines,
no movement:

- `map X: world package P` -> P must be the map name (checks `Outer` +40 and the name table);
- `scan of ... actor Outer == level for A of B actors` -> A close to B (checks `Outer`);
- `census X actors:` -> real class names (`StaticMeshActor`, `PathNode`, `BioPawn`, ...) and not `?` (checks GNames);
- `diag X: pawn at (...) physics 1, controller rotation (...)` -> plausible numbers;
- `diag X: stop i nav PathNode at (...)` -> 16 sample stops;
- `Kismet: USequence::SequenceObjects found at +N` and `census X kismet:` with `SeqAct_*`, `SeqEvent_*`, `SeqVar_*`.

If the class names are `?`, GNames is wrong; if `actor Outer == level` is 0 of N, `Outer` is not +40 (try +36/+44 in
`me_tour_guest.h`).

## 6. Bot

`zsh tools/me1_tour.sh [TOML]` in `mass-effect-recomp` (environment in its header: `EDITION`, `NRO`, `ELF`, `LOCS`,
`DIAG`, `PROF`, `STACKS`, `DWELL`, `MAXPTS`, `SPACING`, `MAP_S`, `COLD`, `MAXWAIT`, `WAR00_FIX`). It moves the user's
saves aside (the new-game route needs a profile without saves) and puts them back at the end, keeps the `AT` autosave
aside as `test_saves_*`, writes the toml (`masseffect_exec = "EnableCheats"` + `masseffect_tour = "<all 19 locations>|BIOA_UNC52 start_UNC52_00"`),
uploads `logs/rex/stacks_profile.flag`, plays the new-game route once, then polls `logs/rex/tour_status.txt` every
minute until its first line is `done` (or `MAXWAIT`, or the game is gone). sys-ftpd cannot read the log the game keeps
open (it lists it with size 0), so the tour rewrites that small file (open, write, close) at every travel, arrival,
stop and map end: line 1 `running`/`done`, line 2 the last event, line 3 seconds since start. Then it fetches every part of the run's log
(`masseffect_N.log`, `masseffect_N.1.log`, ...), `logs/rex/rex_profile.log` and `cache/masseffect_native_pipelines.bin`,
removes the stack flag, runs `tools/extract_prewarm_list.py` (-> `prewarm_list.bin`), `tools/symbolize_profile.py` when
`ELF` is given, and `tools/me1_tour_summary.py` (-> `summary.txt`: fps per 10 s, hottest functions of the busiest
threads, tour/census lines, top UnrealScript functions, per map). Output folder: `run/me1/tour_<stamp>/`.

Bring Down the Sky: persistent map `BIOA_UNC52`, start points in its name table: `start_UNC52_00`,
`start_unc52_ext_mainbase`, `start_unc52_ext_torch1`, `start_unc52_ext_torch2` (the DLC must be installed).

## 7. Limits

- **Story state.** `AT` after a new game keeps the new-game plot: locked doors, missing NPCs, different streaming
  states (docs/location-tests.md 3.1). The tour reaches what the nav network and the streaming triggers of that state
  reach; levels that only Kismet loads (plot-driven `BioSeqAct_SetStreamingState`) stay unloaded.
- **Combat.** Only `God` (and optionally `PlayersOnly` via `masseffect_tour_arrival`). Enemies keep fighting; effects of
  combat are drawn only where combat happens to start.
- **Cutscenes and conversations.** Not triggered on purpose (the census lists their Matinee/`SeqAct_Interp` count per
  map so the gap is known); one that starts on arrival may take the camera while the pawn is moved under it.
- **Mako.** A vehicle pawn is moved without touching its rigid-body physics; it may fall or roll between frames.
  Vehicle-only areas are covered only as far as `FarMoveActor` keeps the vehicle on the stop.
- **Kill volumes.** Not detected (a volume's extent is not read). Stops are nav points, normally above `KillZ`; with
  pinning and `God` a fall does not happen, but a pain volume at a stop could still kill a non-god pawn.
- **Rotation.** Written to the controller every frame; if ME1's camera ignores it, only the pawn's surroundings in
  the default view are drawn.
- **Profile attribution** is per 10 s block (the label at the end of the block); a block that spans two stops or a map
  change is attributed to the later one.

## 8. First console run and fixes (2026-10-09)

| Finding (RU, DIAG, eden + normandy) | Cause | Fix |
|---|---|---|
| world package `?`, pawn `?_8`, all 4317 actors of class `?`, Kismet `?`, script functions `?.?`; 0 nav points | the name number (`_8`) was read, so `Name` +44/+48 is right; the lookups were cut by the pointer range check `0x40000000..0xDFFFFFFF`: the name table and the objects loaded at start-up (classes, functions of Core/Engine) live outside it | range check reduced to "non-null, aligned, below 0xFFFF0000" (tour and profiler). The entry text is calibrated on entry 0 (`None`): offset and UTF-16/ANSI, logged once as `[tour] names: GNames data ... "None" at +16 (UTF-16BE)`; if it fails, a warning with the raw bytes of entry 0 |
| `AT ... -> handled` but no travel; "arrived (long Tick, same GWorld)" after 6 s | `AT` ran 10 s after the new game reached world #2 (intro, streaming), and a hitch was taken for the load | first `AT` after `masseffect_tour_start_s` (60 s) with a pawn; arrival needs a changed world/level/package and the target package name; `AT` repeated every `masseffect_tour_retry_s` |
| script profiler: 1-2 calls per map | most likely the same range check: `Wrap` skipped every `UFunction` outside the 0x4xxxxxxx heap | range check fixed; each table now starts with `[script_prof] <map>: hooks: CallFunction N, ProcessEvent M counted; X on other threads, Y filtered`. If `CallFunction` stays near zero, the hooks are not linked in (the generated code was not regenerated after the hooks were added) |
| the bot never saw `[tour] done` | sys-ftpd cannot read the open log | `logs/rex/tour_status.txt` |

Still to check on the console (next DIAG run): the `[tour] names:` line, real class names in the census, nav points
and streaming triggers counted, `arrived (package BIOA_PRO00, ...)` with Eden Prime on screen, the `hooks:` line with
thousands of `CallFunction` calls, and `tour_status.txt` read by the bot.

### Second console run (RU ru_glob21, DIAG, 2026-10-09)

Names (`"None" at +16 (UTF-16BE)`, GNames data at `0x00010000`, 48294 names), arrival (`package BIOA_PRO00`, 1 `AT`)
and the script profiler (1.4 M `CallFunction` + 0.96 M `ProcessEvent` calls, `BioWorldInfo.Tick`, ...) worked. The
game then froze right after `Kismet: USequence::SequenceObjects found at +216`: the log ends with `Unhandled guest
access violation: read of guest 0x3F800028`. The Kismet shape search of the next sequence had taken a float (1.0 =
0x3F800000) for a pointer and read its "Outer"; an unmapped guest read is not recoverable.

Fixes:
- **Guarded reads.** Every guest read of the tour (and of the names the script profiler prints) checks the guest heap
  first: the page must be committed and readable (`BaseHeap::QueryRegionInfo` / `QueryProtect`, cached per 4 KB page
  for one Tick); anything else reads as 0 and is counted (`reads refused` in the scan lines). The `[tour] names:` warning
  says whether the heap check accepted the name table, in case it is too strict.
- **Budget.** A scan handles at most 2 new levels per frame and continues on the next frames (`more next frame`); the
  Kismet walk of a level is capped at 4000 sequences / 200000 objects (it already had a visited set, so cycles end);
  a scan call that runs longer than 2 s logs `[tour] map X: scan running N s (...)` every 2 s. The nav-point spacing
  test uses a grid instead of comparing with every kept stop.
- **Bot saves.** The new game's own autosave (`a09_AutoSave`) and the `AT` autosaves stayed in the profile. At the end
  every save in the profile (all made during the tour: the user's are in the backup folder) is moved to
  `/switch/masseffect-nx/test_saves_tour_<stamp>/` (kept, not deleted), then the user's saves are moved back.

### Third console run: full RU tour (ru_glob22, 2026-10-09)

The trial (eden + normandy) completed: 195 + 47 stops, 254 / 118 levels streamed, prewarm list 2302 records. The full
tour froze on the third map, `BIOA_STA00`, 1 s after stop 195/200 (a streaming trigger, `touched`, the sixth trigger
in a row with no stop extended in between).

- `rex_crash.log` keeps every crash of every build (27 entries). The `std::terminate: std::system_error` entries at its
  top come from older builds: their code ends at image+0x297F000 / 0x2980000, while ru_glob22's `.text` ends at
  0x2A01000 (the `.nx-module-name` VMA in the ELF). No ELF of those builds is kept (the oldest in `run/me1` ends at
  0x29A3000), so they cannot be symbolized; symbolizing them with ru_glob22.elf gives meaningless names. The entry of
  this run is the last one: `memory access fault`, a guest read of `0xC6EE9A45` on the game thread.
- Symbolized with ru_glob22.elf (`image+X` = ELF address, `.text` at VMA 0): `UGameEngine::Tick` (original,
  `__imp__sub_825E74B8`) -> `sub_823A0AC0` -> ... -> the script VM functions `sub_823EC520` / `sub_823ED078` /
  `sub_823ECCA8` / `sub_823EF8E0` -> `sub_827538F0` (pc). Neither tour code nor the profiler hooks are in the chain.
- `0xC6EE9A45` is the float -30541.1, a Z coordinate of that area (the stops are at Z -29712..-30496): game code read
  a location where it expected an object pointer, the pattern of a reference to an actor of a level that was just
  unloaded (its memory reused).
- Most likely trigger: the tour called `DoTouch` on streaming triggers one after the other every ~1.5 s, switching
  streaming states while the previous switch was still loading/unloading, something a player never does.

Fix: `masseffect_tour_touch_streams` now defaults to `false` (only the engine's own touch from `FarMoveActor`); a stop is
left only after `masseffect_tour_quiet_ms` (2.5 s) without a long frame and without a level change; a streaming
trigger stop lasts at least `masseffect_tour_stream_dwell_ms` (5 s). This crash path only exists with the tour (direct
trigger touches and teleports); v0.1.3 players do not run it.

`tools/me1_tour.sh` now also watches for hangs (status unchanged 5 min, identical screenshots at two checks, game
gone), saves `hang_<n>/` (rex_crash.log, log parts, screenshot, status), closes the game and resumes with a new game
for the maps after the one that hung (at most 3 resumes; `tour_status.txt` line 4 lists the finished maps). Each
session keeps its own logs, profile and `pipelines.bin` under `session<n>/`; the prewarm list is merged over all of
them, the profiles are concatenated for the summary.

## Console note 2026-10-08 (full RU tour)
The 360-degree turn through the controller Rotation does not turn ME1's camera (screenshots during the tour showed one
fixed view per stop). Holding the right stick through sys-botbase spins the camera (checked with four screenshots 1 s
apart); tools/me1_tour.sh now holds it after the new game. The native turn can be removed or redone through
BioCameraManager later.

### Coverage fixes after the full RU tour (ru_glob23, 2026-10-09)

- **View turn.** The controller's `Rotation` does not turn ME1's camera (`BioCameraManager` keeps its own yaw).
  `masseffect_tour_turn` (default off) keeps the old code; `tools/me1_tour.sh` holds the right stick (sys-botbase
  `setStick RIGHT 32000 0`) after the new game, verified on the console. The camera manager's rotation was not
  located statically.
- **Stops in the sky.** A stop is visited only when its level is in `GWorld->Levels` and visible (ULevel+364 == 0, the
  word `FActorIterator` tests; assumed meaning, the diag line `levels visible N, pending M` shows it; if every level
  looks pending the flag is ignored with a warning). Stops of levels that are not loaded wait; when nothing else is
  left and no level is pending, the map ends and they are counted as skipped. When an unloaded level comes back at
  another address, its stops are re-bound to the new level (`rebound`) instead of being dropped as duplicates.
- **Streaming triggers, again, safely.** One DoTouch per trigger stop, only when there was no long frame and no level
  change for `masseffect_tour_quiet_ms`, no level is pending, and `masseffect_tour_touch_gap_ms` (10 s) passed since the
  last touch; otherwise the pinned pawn waits up to `masseffect_tour_stream_wait_ms`, then the touch is skipped. After a
  touch the stay restarts and lasts until its loads have settled. Log: `stop N trigger touched / not touched (...)`.
- **Counters** in `map X finished (...)`: stops skipped (level not loaded, or caps), rebound stops, triggers touched /
  not touched. Empty views (nothing drawn) are not detected: the draw counts live in the renderer files
  (`app/src/native/masseffect/`), which belong to the renderer work.

### Death guard (2026-10-09: Shepard died in a firefight at Port Hanshan with God on)

- **Hook.** `UObject::ProcessInternal` (EN `sub_823E8B28`, RU `sub_823E93B0`; in `me_tour_guest.h` with the profiler's
  hooks) runs a script function's body after `CallFunction` or `ProcessEvent` has set up the parameters; not calling
  it skips the body without breaking the caller's bytecode (skipping `CallFunction` itself would). The return value
  stays as the caller initialised it.
- **What is skipped.** While the tour runs (from its start to `done`, not in discovery mode) and
  `masseffect_tour_no_death` is on (default): functions named in `masseffect_tour_no_death_names` (default
  `TakeDamage|TakeRadiusDamage|Died|KilledBy|PawnDied|FellOutOfWorld|OutsideWorldBounds|Suicide|CausePainTo`) when
  called on the player's pawn, its controller, or an object whose Outer is the pawn (e.g. a behaviour object); and
  `masseffect_tour_no_death_any` (`Class.Function` or `Function`, default empty) on any object, for the
  mission-failure handler once its name is known. Log: `[tour] guard: X will be skipped`, `[tour] death event X on Y
  swallowed (n times)`, and once per function `[tour] guard: death/damage-like script function X seen` for every
  other function whose name contains die/dead/death/kill/fail/gameover/damage/suicide/fellout/outsideworld/health/
  pain/wound: that list tells which ME1 names to add.
- **Not covered.** (It is: see "Health keeper" below; ME1 applies the damage through the behaviour object.) Damage applied in C++ without a script call (if ME1's health system does that) and Kismet kills
  that do not go through these functions; the log shows whether the swallowed calls stop the death. KillZ / pain
  volumes are not tested before a move (their offsets were not located); `FellOutOfWorld` and `TakeDamage` are
  swallowed instead.
- **Enemies.** No ME1 cheat that makes the AI ignore the player was found among the native cheat functions
  (`BioCheatManager` has no such exec); `PlayersOnly` (engine cheat) freezes every non-player actor, AI included, but
  also their animation and effects (less to draw), so it stays an option (`masseffect_tour_arrival = "God|PlayersOnly"`).
- **Bot.** A hang whose last screenshot looks like the failure screen is logged as `possible death screen`
  (`tools/me1_death_screen.py`: dark frame + cyan text in the middle; it also matches the mass-relay loading screen,
  so it only names the outcome, it never ends a session).

### Health keeper (2026-10-09: Shepard still died with the guard on)

Console run `tour_400` (RU): the guard swallowed `Pawn.TakeDamage` on the player's pawn 800+ times on Virmire and 336
times at Noveria, yet the health reached zero (`PlayerController.PawnDied` swallowed) and the screen went dark (death
fade, HUD gone) until the next map. ME1 applies damage outside `Pawn.TakeDamage`: the log shows
`BioPawnBehavior.ProxyTakeDamage` running, on the pawn's behaviour object, which the guard did not count as the player.

Where ME1 keeps health (static analysis of `BIOC_Base.xxx`, EN, LZO chunks decompressed, export table parsed):

| Value | Path from the player's `BioPawn` | Type |
|---|---|---|
| health | `m_oBehavior` (BioPawnBehavior) -> `m_PawnAttributes` (BioAttributesEpicPawn / BioAttributesPawn) -> `m_HealthCurrent` | float |
| health max | same attributes -> `m_HealthMax` -> `m_Current` | `BioComplexFloatStructAttribute` (m_Base, m_Current, m_Min, m_Max, modifiers) |
| shields | same attributes -> `m_ShieldCurrent` | float |
| shields max | `m_oBehavior` -> `m_oShield` (BioShield) -> `m_pAttributes` (BioAttributesShield) -> `m_ShieldMax` -> `m_Current` | complex float |
| UE3 health | `Pawn.Health` / `Pawn.HealthMax` | int (likely unused by ME1, kept full too) |

Also there (not used): `BioActorBehavior.m_bMin1Health` / `m_bMin1HealthOverride` (the game's own "cannot drop below 1"
flag for plot-protected characters), `BioEpicPawnBehavior.SetCurrentHealth` / `Heal`, `BioBaseSquad.GetPawnHealthPct`.

How it works (`masseffect_tour_keep_health`, default on; only from the tour's start to `done`, never without
`masseffect_tour`; discovery mode finds and counts but does not write):

- **Reflection.** Each value is found by name on the live objects: the object's class (`UObject::Class` +52), the class
  chain (`SuperField` +60), each class's fields (`Children` +76, `Next` +64), the field's class name ending in
  `Property`, its `Offset` (+100). Struct members through `UStructProperty::Struct` (found at run time). Once, the
  offsets of `Actor.Location` / `Rotation` / `Physics` must come out as +260 / +272 / +104 (verified elsewhere), else
  the keeper stays off with `[tour] reflection check FAILED`. Same layout in both editions (no overlay change beyond
  the shared constants `kFieldNext`, `kStructChildren`, `kPropertyOffset` in both `me_tour_guest.h`).
- **Every frame** after `UGameEngine::Tick`: a value below its maximum is set to the maximum (health, shields, UE3
  health). A guest page that is not readable is skipped. Nothing else of the game is changed (AI, combat and damage
  events run as before).
- **Second layer.** The death guard stays; its "player objects" now also include `BioPawn.m_oBehavior`, so the default
  names (`TakeDamage`, `TakeRadiusDamage`, ...) declared on `BioActorBehavior` are swallowed there too.
  `ProxyTakeDamage` is not in the default list (it also plays hit reactions); add it to `masseffect_tour_no_death_names`
  if one frame's damage can still exceed full health plus shields.
- **Limit.** Health is refilled once per frame, after the frame: a single frame that deals more than full health and
  shields (falls, kill volumes, big explosions) can still kill.

Log:

- `[tour] reflection check ok: Actor.Location +260 (StructProperty), Rotation +272 ..., Physics +104 (ByteProperty)`
- `[tour] reflection: UStructProperty::Struct at +N (...)`
- once per map and pawn, when the map is settled: `[tour] map X: health properties of BioPawn_N (BioPawn):
  m_HealthCurrent @ADDR = v / max @ADDR = w, m_ShieldCurrent ..., Pawn.Health ...; behavior ..., attributes ...,
  shield attributes ...; keeping them full: yes`, then `[tour] map X: health-like properties of <object>: ...` (every
  float/int/complex property with "health" or "shield" in its name on the pawn, the behaviour, the attributes and the
  shield attributes, with offsets and values: if the HUD and the death use another value, it is in these lines).
- at the end of each map: `[tour] map X: player health below 50 % in N of M frames (lowest P %); health restored H
  times, shields S times` (ME1's `m_HealthCurrent`, or `Pawn.Health` when that is not found; counted before the refill,
  so N > 0 means damage got through within a frame).
