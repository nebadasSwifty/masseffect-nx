# Location tests: starting the game in a chosen location

Goal: reach each of the 19 test points of [external-practices-2.md](external-practices-2.md) section 4 quickly and the
same way on every run, so that every location can be measured on the console (fps, CPU/GPU cost, contact sheet).

Status (2026-10-07): first console runs done (1.5). `AT` is never refused: Eden Prime and the Presidium arrived;
Feros hangs on the loading screen because the Russian repack's `BIOA_WAR00.xxx` is the wrong (and truncated) file,
fixed by uploading the Disc 1 file (`WAR00_FIX`). Everything below that is marked "unverified" was derived offline
from the game data and the recompiled code.

## 1. Methods, ranked

| Rank | Method | What it gives | Verdict |
|---|---|---|---|
| 1 | **Console command `AT <map> <start>`** run by the port (`masseffect_exec`) | The developers' own travel: map + start point, the game's own transition | **chosen** |
| 2 | Startup URL: SDK cvar `cl` (guest command line) or Coalesced `[URL] LocalMap` | The persistent map only, no start point | already known to be unreliable |
| 3 | Prepared save games | Exact position and story state | best fidelity, needs one manual play per location |
| 4 | Coalesced cheat/debug settings | nothing usable | the debug menu exists but has no button to open it |

### 1.1 `AT`: the developers' travel command (chosen)

- `Coalesced.ini` (`Xe-BIOEngine.ini`, `[BIOC_Base.BioPlayerInput]`) still carries the developers' debug travel menu:
  `DebugMenu=(Name="STA Chunks>",Command="showsubmenu STA")` and entries such as
  `STA=(Name="STA20_01 - The Presidium",Command="AT BIOA_STA00 start_STA20_01")`, about 150 of them for every area
  and start point. No input binding opens this menu on the console build.
- `AT` is an exec function of `BioCheatManager` (script, `BIOC_Base.xxx`, flags Defined|Exec|Public):
  `AT(name newArea, name startPoint)` = `BioWorldInfo(Outer.WorldInfo).MoveToArea(newArea, startPoint)`.
- `ABioWorldInfo::MoveToArea` is native (EN `sub_82947668`, RU `sub_829476D8`): it writes the area name and the start
  point into the `BioSaveGame` object (`m_DesiredStartPoint`, `m_UseDesiredStartPoint`), saves the current state and
  runs `GEngine->Exec("DEFER BIOAREATRANSITION <map>")`, the same deferred transition the game uses. After the load,
  `BioSPGame.FindPlayerStart` reads `m_DesiredStartPoint` and calls `FindStartingNavPoint`. A plain URL portal
  (`BIOA_STA00#start_STA20_01`) is **not** used by ME1: `FindPlayerStart` only falls back to the engine's portal match
  (Teleporter tags) when no desired start point is set.
- The cheat manager does not exist by default. `PlayerController.EnableCheats` (exec) calls `AddCheats`, which only
  checks `WorldInfo.NetMode == NM_Standalone` and creates `CheatClass`; `Default__BioPlayerController.CheatClass` is
  `BioCheatManager`. So the command list is `EnableCheats|AT <map> <start>`.
- Other useful exec commands on the same path: `God`, `PlayersOnly` (freezes AI and physics actors: steadier
  measurements), `Teleport`, `Walk`, `Ghost`, `StreamLevelIn <level>`, `CauseEvent <name>` (Kismet console events),
  `BioCheatManager.SetLocation`, `GetLocation`, `BioLoadGame`, and the engine commands below.
- The engine commands reached at the end of the chain (`UGameEngine::Exec`, EN `sub_825E2708`, RU `sub_825E3248`):
  `OPEN`, `START`, `STREAMMAP`, `SERVERTRAVEL`, `DISCONNECT`, ...; BioWare's (`sub_82364260`): `DEFER`,
  `NEXTFRAMEDEFER`, `SAVEGAME`, `LOADGAME <name> DevId=<n>`, `SAVEPROFILE`; the world's (`sub_8294A7A0`):
  `BIOAREATRANSITION`, `BIOSTREAMSTATEON/OFF`, `BIOLISTSTATES`, `COLLECTGARBAGE`, `AUTOSAVE`, `DUMPSAVEGAME`, ...

### 1.2 Startup URL (`cl`, `LocalMap`)

- The guest command line is the SDK cvar `cl` (`sdk/src/kernel/xboxkrnl/xboxkrnl_module.cpp`): it is appended to
  `"default.xex"` in `ExLoadedCommandLine`. The game reads it when it has no launch data (EN/RU `sub_82811650` returns
  `ExLoadedCommandLine`, `sub_82216EF0` copies it into `GCmdLine`), and `UGameEngine::Init` (EN `sub_825E1AD0`,
  RU `sub_825E2610`) takes the first token as the start URL unless it begins with `-`; otherwise `FURL::DefaultLocalMap`
  (`[URL] LocalMap=EntryMenu`). So `cl = "BIOA_STA00"` in the toml is the same as the existing Coalesced `LocalMap`
  override (tools `coalesced.py set-map`, `switch_cycle.sh --map`), without editing `Coalesced.ini` or its SHA-1.
- Measured before (backlog t266-t269): the persistent maps load and render, but the map override "does not reliably
  select the level (several maps boot into the Normandy intro/squad screens)": there is no start point and no story
  state. A `#portal` does not help (1.1). Useful only for load/streaming smoke tests.

### 1.3 Save games

- Saves are Xbox content packages (`BioSaveGame%d`, file `BioSaveGame%d.sav` inside). The port's content manager keeps
  them as folders: `<user_data_root>/<XUID>/<TitleID>/00000001/<package>/`, and on the Switch `user_data_root` is the
  folder of the NRO (`sdmc:/switch/masseffect-nx/`; `filesystem_posix.cpp`, `content_manager.cpp`). Exact folder
  names are unverified: list them over FTP after the first in-game save.
- A save made once on the console can be downloaded and uploaded again before a test (the file is self-contained).
  Loading: the main menu's Continue/Load, or `masseffect_exec = "LOADGAME <name> DevId=<n>"` /
  `"EnableCheats|BioLoadGame <n>"` (argument formats unverified).
- Best fidelity (real plot state, squad, position, vehicle), so it is the right method for the final acceptance runs
  of locations whose content depends on the story (section 5). It needs one manual play per location.

### 1.4 Coalesced cheat settings

`[BIOG_QA.BioSeqAct_ReadQAConsoleCommand] sConsoleCommand=start bioa_pro00` and `bQAMachine` / `bQAAutomation` exist,
but they are read by QA Kismet actions that the shipped maps do not reach on their own. The debug menu entries need a
key binding that opens the menu (none on the pad). Nothing here beats 1.1.

### 1.5 First console runs (2026-10-07): why the Presidium and Feros looked refused

Nothing refuses the travel. `BioCheatManager.AT` calls `MoveToArea` without any check, and the native
`ABioWorldInfo::MoveToArea` (RU `sub_829476D8`) has no plot, galaxy-map or streaming-state condition either: it
copies the area name into the `BioSaveGame` (+1116) at +60, the start point FName to +140/+144, zeroes +148..+156,
sets bit 31 of +136 (use the desired start point), saves the state (`sub_8274A330`, `sub_8274F410`: the
`a00_AutoSave`) and runs `DEFER BIOAREATRANSITION <area>` through `GEngine`'s FExec. So plot variables, galaxy-map
rules or a prepared save are not needed to reach any of the 19 points; they only matter for what the area contains
(section 5).

- **Presidium (`loc_presidium`): it arrived.** 7 s after `AT` the relay loading screen was up (`wait01`), then the
  log reads `BIOA_STA00.xxx`, opens `BIOA_STA20_00_SND`, `BIOA_STA20_T`, `BIOA_STA30_T`, `BIOA_STA60_T` and the
  Keeper packages, and no `NOR10` package again; from `wait02` on Shepard stands alone (no squad) in a white curved
  corridor with the HUD, which is not the prologue Normandy. The hook never logged `world #3` (so the arrival
  command `God` never ran) because the new UWorld got **the same address as the old one** (`40DB72C0`). The hook
  now also counts a long game Tick after the commands as the arrival (`masseffect_exec_load_ms`, section 2).
- **Feros (`loc_zhu`): broken game data.** After `AT` the game opened `BIOA_WAR00.xxx`, read 0 bytes at offset
  1900544 (past the end of the 1835008-byte file) and stayed on the relay loading screen for the rest of the run.
  The Russian release is two DVD images. In **Disc 2** (which the `game_root` was made from: all its file sizes
  match) the directory entry `Layer0/Maps/BIOA_WAR00.xxx` holds a recompressed copy of the **Ilos** map
  (`BIOA_LOS00`, the same data once decompressed), cut to the old 1835008-byte size: its chunk table needs 1966781
  bytes. **Disc 1** has the real Feros map under that name (1835008 bytes, sha1 `e03d3d716fb649eece8323c5f16473ebf6b8070e`,
  references `BIOA_WAR20..50` and `start_war20_01` / `start_war40_01`) and a Feros copy in place of `BIOA_LOS00`.
  The repack evidently swaps maps per disc. Of all 2194 packages of the `game_root` (and of the 533 that differ
  between the two discs) this is the only one whose chunk table points past its end; the other 236 `BIOA_WAR*`
  packages decompress fine (an earlier note in section 5 said otherwise: wrong parser). Without the fix Feros cannot
  be reached at all, also not by playing; the earlier "WAR00 28.8 fps" sweep (t266) was the EN data.
  Fix: replace `/switch/masseffect-nx/game_root/Layer0/Maps/BIOA_WAR00.xxx` with the Disc 1 file (same size, so the
  file index cache stays valid; delete it anyway). `tools/me1_location.sh` checks the console's copy for Feros and
  uploads `WAR00_FIX=<file>` when given. To get the file from your own Disc 1 image:
  `python3 tools/extract_iso.py "<Disc1.iso>" -o /tmp/disc1` and take `Layer0/Maps/BIOA_WAR00.xxx` (check the sha1).
  `python3 tools/extract_iso.py <game_root> --check-packages` lists packages cut shorter than their chunk table
  (on this `game_root`: 1808 compressed packages, only `BIOA_WAR00.xxx` cut). `python3 tools/check_packages.py
  <game_root>` also reports Disc 1's swap: its `BIOA_LOS00.xxx` is a valid Feros copy with `BIOA_WAR00`'s package
  GUID (DUP), so a `game_root` made from Disc 1 alone has no Ilos. The installer (from 2026-10-08) merges two ISOs per
  file with this check (WAR00 from Disc 1, LOS00 from Disc 2, decisions in its Details log) and warns about bad or
  swapped packages of a folder or single-ISO source; a `game_root` made by it needs no `WAR00_FIX`.

## 2. Implementation

`app/src/native/me_console_exec.cpp` (EN) and `editions/ru/overlay/app/src/native/me_console_exec.cpp` (RU addresses).

| cvar | default | meaning |
|---|---|---|
| `masseffect_exec` | `""` (off) | commands run once, separated by `\|` |
| `masseffect_exec_arrival` | `""` | commands run once in the first world loaded after `masseffect_exec` ran (after the travel) |
| `masseffect_exec_world` | `2` | run `masseffect_exec` in the N-th loaded world or later (1 = start menu, 2 = first map after it); 0 = any |
| `masseffect_exec_frames` | `300` | frames with a player pawn in that world before running |
| `masseffect_exec_load_ms` | `0` (off) | after `masseffect_exec` ran, a game Tick at least this long (ms) counts as the arrival even when GWorld keeps its address; `me1_location.sh` sets 1500 |

How it works: a hook after `UGameEngine::Tick` (vtable slot 71; EN `sub_825E6978`, RU `sub_825E74B8`) counts worlds
(`GWorld` changes) and frames in which `GEngine->GamePlayers[0]->Actor->Pawn` is set, then calls
`ULocalPlayer::Exec` (EN `sub_824BDA68`, RU `sub_824BE7A8`) with `this` = the player's FExec subobject, the command
as UTF-16 on the guest stack, and `GLog` as the output device. That is the function a typed console command reaches,
so engine commands, script exec functions and cheats all work. Each command is logged:
`[exec] commands: "AT BIOA_STA00 start_STA20_01" -> handled`. Arrival: a new GWorld pointer (`[exec] world #3`), or,
with `masseffect_exec_load_ms`, the blocking map load measured around the original Tick
(`[exec] Tick of N ms after the commands: counted as the map load`); then `masseffect_exec_frames` pawn frames, then
`[exec] arrival: "God" -> handled`.

| | EN | RU |
|---|---|---|
| GEngine | `0x82EAEA1C` | `0x82EAEA3C` |
| GLog | `0x82E624B4` | `0x82E624D4` |
| GWorld | `0x82EAEA94` | `0x82EAEAB4` |
| UGameEngine::Tick | `sub_825E6978` | `sub_825E74B8` |
| ULocalPlayer::Exec | `sub_824BDA68` | `sub_824BE7A8` |

Offsets (same in both): `UGameEngine` +744 Client, +748/+752 GamePlayers data/num, +760 GameViewport;
`ULocalPlayer` +60 FExec, +64 Actor; `APlayerController` +492 Pawn; `ABioWorldInfo` +1116 BioSaveGame. The hook
checks that the FExec vtable's slot 0 is `ULocalPlayer::Exec` and does nothing otherwise. With both command cvars
empty it only calls the original Tick.

## 3. The 19 locations

"Start ref." = the start point name appears in the persistent map package (checked offline in the RU data). A start
point that is not referenced there may still live in a sublevel; the debug menu name is used as given. Test position:
**walk** = `MODE=walk` (stick forward, turns, switch_bot `film`), **view** = `MODE=watch` (no input). Mako start
points are unverified: the area may start on foot or in the vehicle depending on the start point actor.

| # | Script name | `AT` arguments | Place | Start ref. | Test position |
|---|---|---|---|---|---|
| 1 | `eden` | `BIOA_PRO00 Start_PRO10_01` | Eden Prime landing zone | yes | walk up the slope over the grass toward the long view; black terrain patches are a known older bug |
| 2 | `normandy` | `BIOA_NOR00 Start_NOR10_01` | Normandy CIC / cockpit | yes | walk the CIC to the galaxy map and the cockpit corridor (crew, interior lights) |
| 3 | `presidium` | `BIOA_STA00 start_STA20_01` | Citadel Presidium | yes | walk along the lake (water, long interior, crowds); lowest fps of the earlier sweep |
| 4 | `wards` | `BIOA_STA00 Start_STA60_01` | Wards, Chora's Den | yes | walk into Chora's Den (dancers, translucency, many lights); DF's 15 fps spot |
| 5 | `tower` | `BIOA_STA00 Start_STA70_01` | Council tower and chamber | yes | walk up to the chamber; view the hall (DOF in cutscenes) |
| 6 | `zhu` | `BIOA_WAR00 Start_WAR20_01` | Feros, Zhu's Hope | yes, Disc 1 file only (1.5) | walk through the colony (NPCs, skyway view) |
| 7 | `feros` | `BIOA_WAR00 Start_WAR40_01` | Feros war zone (skyway) | yes, Disc 1 file only (1.5) | walk/drive along the skyway, geth combat (`ARRIVAL=God`) |
| 8 | `aleutsk` | `BIOA_ICE00 Start_ICE25_01` | Noveria, Aleutsk Valley (Mako) | yes | drive through the valley (snow, cliffs, particles); DF's other 15 fps spot |
| 9 | `hanshan` | `BIOA_ICE00 Start_ICE20_01` | Port Hanshan | yes | walk the hub (NPCs, interior) |
| 10 | `hotlabs` | `BIOA_ICE00 Start_ICE60_01` | Peak 15 science station / hot labs | yes | walk the labs, combat effects (`ARRIVAL=God`). The menu's `Start_ICE70_01` has no ICE70 packages on this disc |
| 11 | `beach` | `BIOA_JUG00 Start_JUG20_01` | Virmire landing zone (Mako) | yes | drive along the beach (foliage, open terrain, AA turrets) |
| 12 | `bomb` | `BIOA_JUG00 Start_JUG80_07` | Virmire central trench, bomb site | yes | walk the trench hub (heavy combat, effects; `ARRIVAL=God\|PlayersOnly` for a steady number) |
| 13 | `ocean` | `BIOA_JUG00 Start_JUG80_03` | Virmire ocean view facade | yes | view the facade and the sea. The doc's `Start_JUG70_10` is not referenced in JUG00; `JUG70_09` is the interior facade |
| 14 | `therum` | `BIOA_LAV00 Start_LAV60_01` | Therum, Ring of Fire | yes | drive/walk the lava ring (heat haze distortion) |
| 15 | `trench` | `BIOA_LOS00 Start_LOS50_01` | Ilos trench run (Mako) | yes | drive forward through the trench (fast camera, streaming, many geth) |
| 16 | `archives` | `BIOA_LOS00 Start_LOS40_01` | Ilos archives | yes | walk the large interior |
| 17 | `plaza` | `BIOA_END00 Start_END70_00` | Final Citadel plaza | yes | walk into the battle (`ARRIVAL=God`) |
| 18 | `spacewalk` | `BIOA_END00 Start_END80_00` | Citadel tower space walk | yes | walk on the tower outside (open sky, effects) |
| 19 | `uncharted` | `BIOA_UNC93 start_UNC93_01` | Uncharted world, lichen (Purging the Veil 4) | yes | drive on open terrain (terrain LOD). Only UNC90/92/93 are on this disc; UNC21 of the earlier list is not |

Any other entry of the debug menu works the same way: `zsh tools/me1_location.sh BIOA_STA00:Start_STA30_01`.

### 3.1 Command sequence per location

Every point uses the same sequence; only the `AT` arguments change. On the console (what the script does):

1. (Feros only, once) the Disc 1 `BIOA_WAR00.xxx` in `/switch/masseffect-nx/game_root/Layer0/Maps/` (1.5).
2. `masseffect.toml`: `masseffect_exec = "EnableCheats|AT <map> <start>"`, `masseffect_exec_arrival = "God"`,
   `masseffect_exec_world = 2`, `masseffect_exec_frames = 150`, `masseffect_exec_load_ms = 1500`.
3. Launch, title, New game with the default character (the commands run ~5 s after Shepard has a pawn on the
   prologue Normandy; ~10 s of relay loading screen; arrival ~5 s later).
4. Expect in the log: `commands: "AT ..." -> handled`, then `world #3` or `Tick of N ms ...`, then `arrival: "God"`.
5. Remove the autosave `a00_AutoSave` from the profile afterwards (the script moves it to `test_saves_*`).

From `mass-effect-recomp`, `NRO=run/me1/ru_loc2.nro` and the base toml as usual:

| # | Command | Notes |
|---|---|---|
| 1 | `zsh tools/me1_location.sh eden` | verified 2026-10-07 |
| 2 | `zsh tools/me1_location.sh normandy` | |
| 3 | `zsh tools/me1_location.sh presidium` | verified 2026-10-07 (arrival not logged before `masseffect_exec_load_ms`) |
| 4 | `zsh tools/me1_location.sh wards` | |
| 5 | `MODE=watch zsh tools/me1_location.sh tower` | |
| 6 | `WAR00_FIX=<Disc 1 BIOA_WAR00.xxx> zsh tools/me1_location.sh zhu` | hangs without the fix |
| 7 | `WAR00_FIX=<Disc 1 BIOA_WAR00.xxx> zsh tools/me1_location.sh feros` | the file stays on the console after 6 |
| 8 | `zsh tools/me1_location.sh aleutsk` | Mako start unverified |
| 9 | `zsh tools/me1_location.sh hanshan` | |
| 10 | `zsh tools/me1_location.sh hotlabs` | |
| 11 | `zsh tools/me1_location.sh beach` | Mako start unverified |
| 12 | `ARRIVAL="God\|PlayersOnly" zsh tools/me1_location.sh bomb` | |
| 13 | `MODE=watch zsh tools/me1_location.sh ocean` | |
| 14 | `zsh tools/me1_location.sh therum` | |
| 15 | `zsh tools/me1_location.sh trench` | Mako start unverified |
| 16 | `zsh tools/me1_location.sh archives` | |
| 17 | `zsh tools/me1_location.sh plaza` | |
| 18 | `zsh tools/me1_location.sh spacewalk` | |
| 19 | `zsh tools/me1_location.sh uncharted` | |

If a point does not arrive: look at `wait01.jpg` (a relay loading screen means the travel started), at the
`[io]` lines after the `AT` line (which map packages opened; a read of 0 bytes means a broken file, as WAR00), and
check the data with `python3 tools/extract_iso.py <game_root> --check-packages` before suspecting the hook.

## 4. How to run

1. Build (one build at a time in this tree): `EDITION_XEX=<ru default.xex> bash tools/edition.sh ru all`, then copy
   `out/nx-ru/masseffect-nx-rus.nro` to `mass-effect-recomp/run/me1/ru_location.nro`.
2. From `mass-effect-recomp`, with the console at HOME:
   `NRO=run/me1/ru_location.nro zsh tools/me1_location.sh presidium run/me1/<base>.toml`
   - the base toml is any settings file (default `masseffect-nx/app/masseffect.toml`); the script drops its own keys
     from it and appends `masseffect_exec = "EnableCheats|AT ..."`, `masseffect_exec_arrival = "God"`,
     `masseffect_exec_world = 2`, `masseffect_exec_frames = 150`, `masseffect_exec_load_ms = 1500`.
   - route: launch, title, new game, default character (the `me1_anderson.sh` steps); `AT` runs ~5-10 s after the
     player has a pawn in the first map after the menu; `wait*.jpg` every 10 s for `WAIT` s (default 150); then
     `MODE=walk|watch` filming for `SECS` s; the game is closed and the log fetched.
   - environment: `NRO`, `MODE`, `SECS`, `WAIT`, `ARRIVAL` (`""` = none), `FRAMES`, `LOADMS` (default 1500),
     `WAR00_FIX` (Feros, 1.5), `COLD=1` (delete the pipeline and file-index caches: acceptance runs are cold).
3. Results in `run/me1/loc_<name>/`: `wait*.jpg`, `film/f*.jpg`, `console.log`, `exec.txt` (the `[exec]` lines: world
   numbers, commands, handled or not).
4. First run: check `exec.txt`. If `AT` ran too early (e.g. in a character-creation world) or never, change
   `masseffect_exec_world` / `masseffect_exec_frames` from the logged `world #N` lines.

## 5. Risks and open points

- **Story state.** `AT` after a new game keeps the new-game plot: doors locked by plot flags, NPCs and squad members
  missing or different, arrival cutscenes or conversations may start, areas that expect the Mako may start on foot.
  What loads (streaming states, crowds) can differ from a real play-through, so numbers from `AT` are a lower or
  upper bound, not the final word. Final acceptance runs of story-heavy places (Presidium, Chora's Den, Virmire,
  the final battle) should use saves (1.3).
- **Feros data of the RU release.** Only `BIOA_WAR00.xxx` is broken: Disc 2 holds the Ilos map under that name,
  truncated; Disc 1 has the right file (1.5). Points 6 and 7 need `WAR00_FIX`. (Corrects an earlier note here that
  counted 183 broken `BIOA_WAR*` packages: that parser misread the chunk tables.)
- **Unverified on the console:** that `EnableCheats` creates the cheat manager in the shipped build, that `AT` works
  from the first gameplay world, Mako start points, the world count of the new-game route (character creation may
  be its own world), `LOADGAME`/`BioLoadGame` argument formats, the save folder names.
- **Guest stack use.** The command string and the call frame live below the hook's stack pointer after the original
  Tick has returned; nothing of the game is live there at that moment.
- **Generated code.** The new hook adds addresses to the hooked set, so the next build of each edition regenerates
  the code from scratch (tools/edition.sh does this automatically).
- **Game files** are only read locally for analysis (package names, start points); nothing is copied anywhere.

## 6. Offline analysis notes

- 360 packages are LZO1X-compressed in 128 KB blocks (`CompressionFlags = 2`), not LZX. A small read-only unpacker
  (scratch script, not committed) gave the name/export tables used above.
- Native exec thunks are found through the `int<Class>exec<Function>` name table (pairs of name pointer and function
  pointer, e.g. RU `0x82E3D738` -> `execMoveToArea` `sub_8290F288`).
