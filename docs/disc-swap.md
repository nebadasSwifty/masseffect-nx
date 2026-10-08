# Disc swap (two-disc Russian repack)

Status: analysis done; code is a small SDK change (`sdk/src/kernel/xboxkrnl/xboxkrnl_io.cpp`), syntax-checked on the
Mac, not yet built or tested on the console.

## 1. What the game does: nothing

The executable has no disc-swap logic. Evidence from the RU image (XEX 0.0.0.5, media ID `572BA75D`, decompressed on
the Mac):

- **Execution id:** `disc_number = 1`, `disc_count = 1` (XEX optional header `0x40006`). It is a one-disc title.
  The SDK's `XamGetExecutionId` returns this header as it is, so the game already sees "disc 1 of 1".
- **Imports:** no `XamSwapDisc`, `XamLoaderGetMediaInfo` or `XamLoaderGetDvdTrayState` (the `XamContent*` imports are
  save-game devices). `XamGetExecutionId` is only used by the XDK content check `sub_82AE8EE0` (compares the title id).
  `XamLoaderLaunchTitle` is only reached from `sub_82364080` (only referenced from vtables, e.g. `0x82101500`): it
  relaunches `default.xex` with the `Splash.bmp` / `EntryMenu` launch data, i.e. "restart to the main menu".
- **Strings:** no "disc"/"insert"/"swap"/"DVD" string in the image (ASCII or UTF-16), no native UnrealScript function
  with "Disc"/"Disk" in its name (the full `int<Class>exec<Function>` table is in the image). UnrealScript therefore
  cannot even ask which disc is in the drive.
- **The only disc UI:** `sub_826DBC80` (UE3 `appHandleIOFailure`): sets a flag, calls `XamShowDirtyDiscErrorUI`
  (`sub_82AE64C8`), then exits. Our SDK shows an ImGui "Disc Read Error" box there. It fires on a read failure or a
  SHA-1 mismatch of a verified file (the image holds a SHA-1 table of every file on the disc, e.g. `Coalesced.ini`,
  `UplinkSEQ02.bik`).

So the "change discs" step of the Russian release is a property of its **data**, not of the code: the repack
("RUSSOUND+RUS_TEXT", two DVD5 images) carries one file list on both discs and fills the other disc's files with
placeholders (Disc 1: `BIOA_LOS*`, `BIOA_END*`, `UplinkSEQ02.bik`; Disc 2: `BIOA_WAR*`, see
`installer/js/source.js` and `docs/location-tests.md` 1.5). On a real Xbox 360 the player saves, restarts from the
other disc and loads the save. A single-layer JtagRIP of the 1C release (`Layer0` + `Layer1` on one disc) has no
placeholders at all.

What our port does with that:

- The installer merges both discs, so a correct installation has the whole game and **nothing ever asks for a
  disc**. The Ilos maps and `UplinkSEQ02.bik` come from Disc 2, the Feros maps from Disc 1. Not every placeholder is
  junk: Disc 1's `BIOA_LOS00.xxx` is a complete, valid copy of the Feros map `BIOA_WAR00` (same package GUID) and
  Disc 2's `BIOA_WAR00.xxx` is the Ilos map cut short. So the merge runs the structural package check
  (`installer/js/pkgcheck.js`, the port of `tools/check_packages.py`) on every copy and prefers the one that passes
  and whose GUID no other package name on its disc uses; the earliest disc wins only when both copies are equally
  good. Each decision is in the installer's Details log. A folder or single-disc source gets the same check and a
  warning listing the bad or swapped packages (see `installer/README.md`, "Two discs and the package check").
- With only one disc installed, a map placeholder cannot be loaded at all (the package is junk or cut short); that
  needs the other disc's file (as `WAR00_FIX` for Feros), no code can continue there.
- A **movie** placeholder (or a valid Bink "insert disc" movie, if a repack has one) is where a picture could show.
  That case is now covered by the redirect below.

## 2. Change: placeholder movies play the BioWare logo

`sdk/src/kernel/xboxkrnl/xboxkrnl_io.cpp`, in `NtCreateFile_entry` (all guest opens, both editions, no guest
addresses involved):

1. After a successful read-only open of an existing file (`FileAction::kOpened`, not a directory) whose path is
   `...\Movies\<name>.bik` (any case; `BWLogo` itself excluded),
2. the first 4 bytes are read. A Bink file starts with `BIK` (Bink 1, which Mass Effect uses) or `KB2` (Bink 2).
   Anything else is an other-disc placeholder. A name listed in `masseffect_disc_swap_movies` is replaced even when it
   is a valid Bink file.
3. Then the same open is repeated for `<prefix>Layer0\Movies\BWLogo.bik` (BWLogo is a `Layer0` file;
   `D:\Layer1\Movies\UplinkSEQ02.bik` becomes `D:\Layer0\Movies\BWLogo.bik`; a path without a `LayerN` component keeps
   its own `Movies\` folder), the original file is closed and the game gets the BWLogo handle. The game's Bink player
   reads the movie by its own header, so it plays the logo and the sequence continues.
4. A movie that does **not exist** is not touched: the game probes `<name>_RUS.bik` before `<name>.bik` (see the RU
   logs: `BWLogo_RUS.bik`, `load_f01_RUS.bik`), and that probe has to keep failing.

Log line on every replacement: `[disc] '<path>' is an other-disc placeholder or a listed disc-swap movie: playing
'<BWLogo path>'`. Cost: one 4-byte read per movie open (a handful per load), nothing for other files.

| Cvar | Default | Meaning |
|---|---|---|
| `masseffect_skip_disc_swap` | `true` | Enables the redirect above. |
| `masseffect_disc_swap_movies` | `""` | Comma-separated movie names (no path, `.bik` optional, any case) that always play BWLogo instead, e.g. `"UplinkSEQ02"` if a repack ships a valid Bink "insert disc 2" movie under that name. |

Not changed on purpose: `XamGetExecutionId` (already disc 1 of 1), `XamSwapDisc` (stub returning success, the game
does not import it), `XamShowDirtyDiscErrorUI` (a real read error should stay visible).

Open point: the image's SHA-1 table also lists movies. UE3 verifies files read through its own file reader; the Bink
player opens movies itself, so movies are not expected to be checked. If the console shows "Disc Read Error" right
after a `[disc]` log line, movies are verified after all, and the replacement needs the stored hash of the requested
movie patched to BWLogo's (as `masseffect_coalesced_sha1` does for `Coalesced.ini`).

## 3. How to test on the console

1. Normal play-through data (merged install): expected result is no `[disc]` line at all. Quick route to Ilos:
   `zsh tools/me1_location.sh 15` (trench, `BIOA_LOS00 Start_LOS50_01`) or `16` (archives, `Start_LOS40_01`), see
   `docs/location-tests.md`. The run must reach Ilos without any disc dialog; `grep -i "\[disc\]\|DirtyDisc" console.log`.
2. Forced redirect (proves the path works without the other disc's data): add to the run's toml
   `masseffect_disc_swap_movies = "GLO_Relay_LOAD"` and run any location (e.g. `zsh tools/me1_location.sh 2`). The
   relay loading movie must be replaced by the BioWare logo, loading must finish as usual, and the log must show
   `[disc] 'D:\Layer0\Movies\GLO_Relay_LOAD.bik' ... playing 'D:\Layer0\Movies\BWLogo.bik'`. No "Disc Read Error".
3. Placeholder case (optional): copy a junk file over a movie on the SD in a scratch `game_root` copy (never the
   real one), e.g. `UplinkSEQ02.bik`, and play the end-game route (`17`/`18`); the logo plays instead of a black
   screen or a hang.
4. `masseffect_skip_disc_swap = false` restores the old behaviour for an A/B.
