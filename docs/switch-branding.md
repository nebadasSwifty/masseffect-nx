# Switch branding: "Nintendo Switch" text and Switch-style button glyphs

Status: research done, code proposed (not built, not tested on the console). Everything happens at run time in
our code. No game file is changed. The text swap is on by default (user decision, 2026-10-07); the glyph swap is not done (user decision: the Xbox glyph letters match the Switch buttons with the default mapping; the prototype hook was removed).

| Feature | Cvar | Proposed file (EN) | RU overlay copy |
|---|---|---|---|
| Talk-table text | `masseffect_switch_text` (+ `masseffect_switch_text_log`) | `app/src/native/me_switch_text.cpp` + `me_switch_text_tables.h` | `editions/ru/overlay/app/src/native/me_switch_text.cpp` |
| Button glyphs | `masseffect_switch_glyphs` | `app/src/native/me_switch_glyphs.cpp` | `editions/ru/overlay/app/src/native/me_switch_glyphs.cpp` |
| Decryption mini-game diamond in the Switch layout (section 6) | `masseffect_switch_minigame_layout` (default `true`) | `app/src/native/me_switch_minigame.cpp` + `me_switch_minigame_patch.h` | `editions/ru/overlay/app/src/native/me_switch_minigame.cpp` |

`me_switch_text.cpp` and `me_switch_minigame.cpp` are in `app/CMakeLists.txt`; the glyph file is not (dropped). The `.cpp` files pass `clang++ -std=c++23 -fsyntax-only`
with the SDK headers. The SWF patcher was also built on the Mac against the extracted movies (see "Checks").

## 1. Text

### 1.1 Where the strings come from

All text the game shows (menus, message boxes, tutorials, HUD prompts, subtitles, codex) comes from BioWare talk
tables (`BioTlkFile` objects): `Layer0/MEInit/GlobalTlk.xxx` (exports `GlobalTlk_tlk` and `GlobalTlk_tlk_M`, the
male/female variants) and one `*_tlk` / `*_tlk_M` pair for each map. Every string has a numeric id (a "strref").
The ids are the same in both editions; only the texts differ. `Coalesced.int` (UE3 `Localize()` sections) has no
platform text, and the XEX has no UI text (only system and debug strings). So one hook on the talk-table lookup
covers everything.

Format, as seen in the packages (read-only, Mac): the packages (`.xxx`) are UE3 v391/licensee 92 packages,
LZO1X-compressed in 128 KB blocks (compression flag 2). After the properties (`m_nHashTableSize`), a
`BioTlkFile` holds a hash table of 12-byte buckets `{int32 id, int32 flags, int32 index}`, a Huffman tree
(`count`, then for each node `u8 leaf` + `u16 char` or `i16 left, i16 right`) and `count` bit streams (LSB first
inside each byte; a `1` bit goes to the first child). Characters are UTF-16.

### 1.2 The lookup function

```
bool UBioTlkFile::GetString(UBioTlkFile* this /* r3 */, int32 strref /* r4 */, FString* out /* r5 */)  // r3 = 1 if found
```

| | EN (Rev 1) | RU (XEX 0.0.0.5) |
|---|---|---|
| `UBioTlkFile::GetString` (vtable slot 71, +284) | `sub_82963EB8` | `sub_82963D18` |
| Huffman decode of one string (called by GetString) | `sub_82963CD8` | `sub_82963B80` |
| `FString::operator=(const TCHAR*)` (called by GetString) | `sub_822437F0` | `sub_822D8708` |
| `appRealloc(ptr, size, align)` (GMalloc->Realloc) | `sub_822100F8` | `sub_822100F8` |
| `UBioTlkFile` class registration / constructor | `sub_82964C08` / `sub_82964AB8` | `sub_82964B28` / `sub_829649D8` |
| `UBioTlkFile` vtable | (from the EN constructor) | `0x82065EE8` |
| Debug draw that calls GetString through the vtable (`"<StrRef Not Found>"`, `"No TlkFile"`) | `sub_827876D0` | `sub_82787F58` |

Evidence: in RU, `sub_82964B28` registers class `UBioTlkFile` (wide string at `0x82066124`, object size 108) with
constructor `0x829649D8`, which stores the vtable `0x82065EE8`. Slot 71 of that vtable is `0x82963D18`. Its body:
hash of `r4` (Jenkins-style mix), `mod m_nHashTableSize` (`+68`), linear probe over 12-byte buckets (`+72`),
then either `bl FString::operator=` (bucket flag `0x40`, already decoded) or `bl sub_82963B80` (decode), and returns
1. The debug function `sub_82787F58` calls it as `obj->vtable[284/4](obj, strref, &FString)`. EN addresses come from
`editions/ru/address_map.json` and were checked by comparing the instructions with the numbers masked (identical).
There are no direct `bl` callers of `sub_82963EB8` in the EN generated code (it is only registered in
`masseffect_register.82940000.cpp`), so a `REX_HOOK_RAW` sees every call.

Guest `FString` = `{u32 data (TCHAR*), i32 num (characters including the terminating 0), i32 max}`, TCHAR is 16 bit,
stored big-endian (UTF-16BE).

### 1.3 How the hook rewrites a string

After the original returns 1, the hook reads the FString, looks the strref up in the tables and, if the text
changes, writes it back in place when `num` fits in `max`, otherwise calls the game's `FString::operator=` with a
copy in a system-heap buffer (so the game's own allocator owns the new buffer). See `me_switch_text.cpp`.

Three kinds of rewrite, all keyed by strref (no global search and replace, so dialogue such as "I'll live" or
"pull the trigger" is never touched):

1. Whole replacement (`kWholeEn` / `kWholeRu`): platform messages.
2. Phrase substitution on a listed set of ids (`kPhraseIds*`, `kPhrase*`): controller wording in tutorials.
3. Face-letter swap (`kLetterIds*`), only when `input_xbox_layout = true` (see section 3).

### 1.4 Platform strings found (EN ids; RU text in `me_switch_text_tables.h` and the scratch dump)

| strref | English original | Proposed English | Proposed Russian |
|---|---|---|---|
| 153006 | A profile sign-in change has occurred. Mass Effect will restart now. | The user has changed. Mass Effect will restart now. | Пользователь изменен. Игра будет перезапущена. |
| 153062 | REMOVED: A memory unit containing critical game files. Returning to Main Menu. | REMOVED: The storage device containing critical game files. Returning to Main Menu. | ОШИБКА: Извлечен носитель данныx, содержащий файлы, необxодимые для игры. Выxод в главное меню. |
| 153723 | WARNING: One or more memory units contain Downloadable Content. ... Do not remove memory units ... | WARNING: Downloadable Content is installed. You may experience slight performance problems. | ВНИМАНИЕ: Установлены загруженные файлы. Возможно небольшое уxудшение производительности. |
| 153801 | Initializing Downloadable Content. Please do not remove memory unit(s). | Initializing Downloadable Content. Please wait. | Запуск загруженныx файлов. Подождите. |
| 154280 | ... Downloadable Content missing from XBox. Module(s) in question: | ... Downloadable Content is missing from this system. Module(s) in question: | (RU original has no "Xbox"; kept close) |
| 163473 | Xbox 360 Controller | Nintendo Switch Controller | Контроллер Nintendo Switch |
| 167959 | Gamer Profile | User Profile | (RU "Профиль игрока" kept) |
| 169079 | ... saves your selections to your gamer profile. | ... to your user profile. | (RU has no profile wording) |
| 169456 | ... a valid storage device is connected to your Xbox 360 console ... | ... is available on your Nintendo Switch system ... | ... в консоли Nintendo Switch есть исправный носитель данныx ... |
| 169457 | No Xbox 360 Gamer Profile loaded. ... Please log in to the Xbox 360 ... | No user is selected. ... Please select a user on the Nintendo Switch system ... | Пользователь не выбран. ... Выберите пользователя на консоли Nintendo Switch ... |
| 171439 | Save Game Failed: Gamer Profile must be signed in. | Save Game Failed: a user must be selected. | Ошибка соxранения: пользователь не выбран. |
| 173617 | New downloadable content available. Please check the Xbox Live Marketplace. | New downloadable content available. | Доступны новые файлы для загрузки. |
| 174310 | ... press the Xbox Guide button. ... | No user is selected. To select a user, press the HOME Button. ... | Пользователь не выбран. Чтобы выбрать пользователя, нажмите кнопку HOME ... |
| 174312 | ... not across Gamer Profiles ... | ... not in user profiles ... | (RU kept) |
| 174444 | ... connected to your Xbox 360 console. | ... available on your Nintendo Switch system. | ... в консоли Nintendo Switch есть исправное устройство xранения. |
| 165144-165548 | Achievement texts "Reward: N Gamerscore", "Unlock ... Gamer Picture" | "N points"; the gamer picture part removed | " и аватара" / "Доступна аватара" removed |
| 168194 | Copyright text (Xbox / Xbox LIVE trademarks) | left as is (legal text) | left as is |

Already neutral and left alone: 151383, 156622, 166177 ("Change Storage Device"), 166188, 171877, 172928, 174274,
174513, 163474/163476/163480 ("controller"), 134502/174533 ("Achievements").

### 1.5 Controller wording (phrase table)

Button prompts in text are plain words, not tags: "Press A to Use", "Pull Right Trigger to fire", "Press START",
"Press BACK", "Left Bumper", "D-pad", "RB/RS/LS". There is no inline image markup (the only `<...>` tokens are
`<CUSTOM0..3>`, `<JournalTask>` and `<ip/...>`/`<p/...>` placeholders). Russian uses "левая/правая верxняя кнопка",
"левый/правый курок", "крестовина", "джойстик", "START", "BACK", "НАЗАД".

| Xbox wording (EN) | Switch wording (EN) | Russian original | Switch wording (RU) |
|---|---|---|---|
| Left / Right Bumper | L / R Button | левую/правую верxнюю кнопку | кнопку L / R |
| Left / Right Trigger | ZL / ZR Button | левый курок / правый курок, правого курка | кнопку ZL / ZR, кнопки ZR |
| RB, RS, LS (172597) | R, Right Stick, Left Stick | (already words) | |
| START | + | START | "+" |
| BACK | - | BACK / НАЗАД | "-" |
| D-pad | directional buttons | крестовина | kept |
| A/B/X/Y | unchanged by default | A/B/X/Y (and Cyrillic А in 157774, 159994) | unchanged by default |

The ids are in `kPhraseIdsEn` (EN tutorial and achievement ids 165144-173257) and `kPhraseIdsRu`. The tables were
run against the full EN and RU dumps: every listed phrase occurs, and no "Bumper/Trigger/START/BACK/Gamerscore"
remains in the listed ids. Russian menu strings use the Latin "x" in place of "х" (for example "необxодимые"), and
never use "ё", "Ё" or "Ъ"; the Russian replacements follow that.

## 2. Button glyphs

### 2.1 Where they are

The glyphs are not textures and not font characters. They are vector shapes (`DefineShape3`/`DefineShape4` tags)
inside Flash 8 movies stored as `UBioSWF` objects (a `Data` byte array holding the whole `.swf`, mostly zlib "CWS")
in `Layer0/MEInit/BIOC_Materials.xxx`. The shared library `GUI_SF_SharedAssets.mainController` (Coalesced.ini:
`SharedAssetLibrary=(SharedFile="mainController.swf", ...)`) holds sprites `ControlA/B/X/Y`, `ControlL/R`,
`ControlLB/RB`, `dPadMC`; the other movies hold their own copies (`ButtonA`, `AButton`, ...). GFx tessellates the
shapes into triangle meshes when it loads the movie, so nothing reaches our texture upload code
(`app/src/native/masseffect/*`): a texture-hash replacement cannot work. A search of every package for
`Texture2D`/`Font`/`Material` named like button/xbox/controller/gamepad/glyph found nothing either (only level
textures such as `PannelButton`). The bitmaps embedded in the movies are portraits, logos and map images, no
buttons. The movies are byte-identical in the EN and RU `BIOC_Materials.xxx`.

| Movie (BioSWF in BIOC_Materials.xxx) | Char id | Glyph | Tag | Size (px) | Original colors |
|---|---|---|---|---|---|
| GUI_SF_SharedAssets.mainController | 6 / 8 / 12 / 14 | A / X / B / Y | DefineShape3 | 19.7 x 19.7 | #7b9e40 / #0da2dc / #f03e22 / #ffd403 + 50 % black |
| mainController | 16 / 19 | LT / RT (trigger with arrow, "L"/"R") | DefineShape4 | 40.1 x 33.2 | #ccffff |
| mainController | 39 / 41 | LT / RT inactive | DefineShape4 | 41.1 x 34.2 | #b6bcd2 |
| mainController | 30 / 32 | LB / RB | DefineShape4 | 37.3 x 25.9 | #b2bdd3 |
| mainController | 17 / 46 / 20 | left stick (L) / left stick alt / right stick (R) | DefineShape4 | 24.6 x 24.5 | #ccffff |
| mainController | 23 | D-pad (dPadMC) | DefineShape3 | 93 x 93 | gradient |
| GUI_SF_HUD.ME_HUD | 618 / 622 / 626 | A / X / Y (interaction, squad prompts) | DefineShape4 | 44 x 44 | as above |
| ME_HUD | 430 / 431 | Y tilted (power/medi-gel), active / dim | DefineShape4 | 32.8 x 31.5 | #7dd3e8 / #666666 |
| ME_HUD | 433 / 434 | BACK (grenade), active / dim | DefineShape4 | 36.5 x 35.3 | #7dd3e8 / #666666 |
| ME_HUD | 614 / 615 | LB / RB small | DefineShape4 | 19.4 x 13.4 | #ccffff |
| ME_HUD | 613 | D-pad small | DefineShape4 | 16.5 x 16.4 | #99cccc |
| GUI_SF_HUD.DesignerUI | 278 / 280 / 282 / 284 | A / B / X / Y | DefineShape4 | 46 x 46 | colored, 40 % rim |
| GUI_SF_Utility.WindowPop | 106 / 108 | A / B (message boxes) | DefineShape4 | 44 x 44 | |
| GUI_SF_GameOver.GameOver | 11 / 9 / 7 | A / B / X | DefineShape4 | 44 x 44 | |
| GUI_SF_InGameGui.MainWheel | 129 | B | DefineShape4 | 44 x 44 | |
| GUI_SF_SkillGame.MiniGame | 3 / 5 / 7 / 9 | A / B / X / Y (decryption mini-game) | DefineShape4 | 46 x 46 | |
| GUI_SF_MainMenu.CharacterCreation | 60, 21, 200 | A, right stick, right stick small | DefineShape4 | 46 / 27.4 / 24.6 | |
| GUI_SF_ReplayCharacterSelect | 12 / 14 / 18 / 20, 27 | A / X / B / Y, D-pad | DefineShape3 / DefineShape | 19.7, 32.8 | |
| GUI_SF_CharacterRecord | 172 / 174, 201 | RT / LT, right stick | DefineShape4 | 32 x 39, 27.4 | #99cccc |
| GUI_SF_Inventory | 355 / 357, 363 / 365, 50 | LB / RB, RT / LT, right stick | DefineShape4 | 38 x 27, 32 x 39, 27.4 | #b2bdd3 |
| GUI_SF_Journal.Codex / Journal, GUI_SF_Shop | 72 / 170 / 190 | right stick | DefineShape4 | 27.4 | |

No START or Guide glyph exists; "START" is only text. `DesignerUI` 331/333 are large bitmap-filled X/Y panels
(not glyphs). Sizes are the shape bounds in Flash pixels (twips / 20), before the movie's own scaling.

Previews of the originals (rendered on the Mac from the vector data; for reference only, not for the repo):
`/private/tmp/claude-502/.../scratchpad/branding/glyphs/*.png` and the contact sheet `glyph_sheet.png`; our
replacement drawings: `.../branding/test/patched_sheet.png`.

### 2.2 Where to replace them

Not in the texture upload code: in the SWF bytes, right after a `UBioSWF` object is loaded.

| | EN | RU |
|---|---|---|
| `UObject::Serialize(this, FArchive&)` (UBioSWF does not override it: vtable slot 13 = +52) | `sub_8230BD80` | `sub_8230BCF8` |
| `UBioSWF` vtable (stored by the constructor) | `0x820C6840` (ctor `sub_82241BE8`) | `0x820C6810` (ctor `sub_82241790`) |
| `UBioSWF` class registration (`"UBioSWF"`, size 96, StaticClass cached at `0x820A1E1C` in RU) | `sub_82241AD0` | `sub_82241678` |
| GFx: loader "open file" (`"GFxLoader failed to open '%s'"`) | `sub_82B57B58` | `sub_82B53B70` |
| GFx: read SWF header (`"... does not start with a SWF header"`, accepts FWS/CWS/GFX/CFX) | `sub_82B58720` | `sub_82B54738` |

`me_switch_glyphs.cpp` hooks `UObject::Serialize`, keeps only objects whose vtable is UBioSWF's, finds the
`TArray<BYTE>` holding the SWF (first `{data, num, max}` in the object whose data starts with `CWS`/`FWS`), inflates
it (stb_image's zlib, already compiled into the SDK by `sdk/src/ui/image_decode.cpp`), replaces each known glyph tag
and writes the movie back uncompressed (`FWS`, which GFx accepts) through the game's `appRealloc`
(`sub_822100F8`), so the game frees it normally. A glyph is recognised by character id + tag length + FNV-1a 64 of
the tag body (table `kTargets`), so another movie or a changed edition is never touched. The new tag is a
`DefineShape3` with the same id and the same bounds, so layout and placement stay as they were.

The GFx-side functions above are the fallback if `Serialize` turns out to be the wrong moment (for example if a
movie is loaded through another path): hook the loader's open function and swap the data of the returned memory
file, or the header reader.

### 2.3 Our drawings

Self-drawn, original: a dark round button (#2E2E2E) with a light stroked label (#F4F4F4) for A/B/X/Y and "-"
(BACK); dark rounded rectangles with "L"/"R" for the bumpers and "ZL"/"ZR" for the triggers; inactive variants in
grey. Letters are polylines from a small stroke font written in the file (no font or artwork is copied). Sticks
(already "L"/"R" in a circle) and D-pads are left unchanged: they mean the same on Switch.

## 3. Input mapping and which label each glyph must show

`sdk/src/input/switch/switch_input_driver.cpp`:

- Default (`input_xbox_layout = false`, `kFrontsPerLetter`): Switch A -> Xbox A, B -> B, X -> X, Y -> Y.
- `input_xbox_layout = true` (`kFrontsPerPosition`): by position, Switch B (bottom) -> Xbox A, A (right) -> Xbox B,
  Y (left) -> Xbox X, X (top) -> Xbox Y.
- Always: L -> LB, R -> RB, ZL -> LT, ZR -> RT (digital, 0 or 255), Plus -> Start, Minus -> Back, stick clicks ->
  LS/RS, D-pad -> D-pad. Minus is held back from the game only while `input_record`/`input_play` is active.
  L+R+D-pad/ZL are SDK menu shortcuts. HOME is the system's.

| Xbox glyph / word in the game | Label to show, default mapping | Label with `input_xbox_layout = true` |
|---|---|---|
| A | A | B |
| B | B | A |
| X | X | Y |
| Y | Y | X |
| LB / Left Bumper | L | L |
| RB / Right Bumper | R | R |
| LT / Left Trigger | ZL | ZL |
| RT / Right Trigger | ZR | ZR |
| LS / RS click, Left/Right Stick | Left/Right Stick (press) | same |
| START | + | + |
| BACK | - | - |
| D-pad | directional buttons | same |
| Xbox Guide | HOME Button | same |

So with the default mapping only colors/shapes and the shoulder/trigger/Start/Back names change; the face letters
already match the physical buttons. Both hooks read `input_xbox_layout` and swap A<->B, X<->Y when it is set.

## 4. Implementation plan

1. Add `native/me_switch_text.cpp` and `native/me_switch_glyphs.cpp` to the native sources in
   `app/CMakeLists.txt` (the RU build picks the overlay copies of the same paths). `me_switch_text_tables.h` is shared
   by both editions and stays in `app/src/native/`.
2. Check that `stbi_zlib_decode_malloc_guesssize_headerflag` is exported by the SDK library that the app links
   (it is compiled in `sdk/src/ui/image_decode.cpp` with `STBI_ONLY_PNG`, which keeps zlib). If not, add a tiny
   inflate (or `miniz`) to the app.
3. Build EN, run on the Switch with `masseffect_switch_text = true`, `masseffect_switch_text_log = true`: the log
   lists every changed string. Check: Options menu (163473), main menu without a user (174310), save/load errors,
   the first tutorials on Eden Prime (168886-168899), the decryption mini-game, the power wheel.
4. Same run with `masseffect_switch_glyphs = true`: the log line `[switch_glyphs] BioSWF ...: N glyph(s) replaced`
   must appear for mainController (10), ME_HUD (9), DesignerUI (4), WindowPop (2), GameOver (3), MainWheel (1),
   MiniGame (4), CharacterCreation (1), ReplayCharacterSelect (4), CharacterRecord (2), Inventory (4). Screenshots of
   a door prompt (ME_HUD A), a message box (WindowPop A/B), the HUD grenade (BACK), the inventory (LB/RB/LT/RT).
5. If the Data array offset scan finds nothing, log the 96-byte object once and fix the offset (it is the tagged
   `Data` ArrayProperty; `UObject` ends around +0x3C).
6. Repeat 3-4 on the RU build (`tools/edition.sh ru`).
7. Cold-start cost: one inflate + rewrite per movie (~0.5 MB total), once per load of BIOC_Materials; measure.
8. Later, optional: a `HOME` glyph is not needed (no Guide glyph exists); a "+" glyph would only be needed if a START
   glyph is found in a map package (none in the menus).

## 5. Checks done on the Mac

- Packages unpacked read-only in memory (LZO1X). Every package of both copies was decoded for talk tables: EN
  46,650 ids, RU 43,215 ids (ES/PL tables excluded); every table entry was verified against them. Every EN package
  was scanned for texture/font/material names; the BioSWF movies of both editions were compared (identical).
- SWF patcher (`PatchSwf`, `BuildGlyph`) compiled on the host with zlib in place of stb and run on the extracted
  movies: mainController 10, ME_HUD 9, Inventory 4, MiniGame 4 glyphs replaced; the output parses tag by tag to the
  End tag and renders as intended (also with the swapped layout).
- Not done: anything on the console; GFx's handling of our `DefineShape3` line strokes is assumed (standard Flash 8).

Local data notes: the RU game is at `/Users/kirillsutormin/Downloads/masseffect-nx/game_root` (XEX SHA-256
`4beb5825...`, the retail RU disc), not under `~/Downloads/Mass Effect/...` (that folder no longer exists). In that
copy 236 `Layer0/Maps` packages and `GlobalTlk_ES.xxx` are not valid packages (no `0x9E2A83C1` tag); the RU strings
above come from `GlobalTlk.xxx` and the valid maps, so a few map-only RU strings may be missing. The EN game is at
`work/Mass Effect (USA, Europe) (En,Es,Pl) (Rev 1)` (XEX SHA-256 `db14a72a...`).

## 6. Decryption mini-game: diamond in the Switch layout

Status: code written, host-tested on the extracted movie (both modes); not yet built or run on the console.

### 6.1 How the mini-game works

The bypass/decryption mini-game ("Сложность дешифровки", control panels, lockers, crates) is the Scaleform movie
`GUI_SF_SkillGame.MiniGame` (UBioSWF in `Layer0/MEInit/BIOC_Materials.xxx`, byte-identical in EN and RU; zlib "CWS",
253,964 bytes stored, 279,796 inflated). Everything it does is keyed by the button **label**:

- Sprite 11 (placed as `buttonCluster` in sprite 56 = `modalMiniGameLauncher`) is the diamond: four dim glyphs
  (depths 1/3/5/7, always visible) and four named clips on top (depths 9/11/13/15): `AButton` (glyph sprite 4)
  at (-0.6, 47.8) bottom, `BButton` (6) at (54.95, -1.55) right, `XButton` (8) at (-55.4, -1.55) left, `YButton`
  (10) at (-0.6, -48.35) top.
- UnrealScript drives the sequence by calling the movie's `SetButtonVisible(n)`, n = 0/1/2/3 = A/B/X/Y
  (`buttonSK_ID_*`), which sets `_visible` on that named clip; `ClearAllButtons()` hides them all.
- The movie's `keyListener.onKeyDown` reads `Key.getCode()` (class `com.XInput`: A=65, B=66, X=88, Y=89), plays the
  "correct" sound if the clip of that label is visible, and sends `FSCommand SG_ButtonPress <id>` back to Unreal,
  which decides success. The pre-game menu (`MiniTitle`: A = start, X = omni-gel, B = cancel) is a vertical list.
- No position is read anywhere; only `_visible` of the named clips and the key code matter.

With our default input mapping (by label: Switch A = Xbox A, ...) presses are already correct, but the lit glyph
is at the Xbox position (A bottom), while the Switch A button is on the right.

### 6.2 The fix

`me_switch_minigame.cpp` hooks `UObject::Serialize` (EN `sub_8230BD80`, RU `sub_8230BCF8`; UBioSWF does not override
it, and the tagged `Data` array is read inside it). After the original, for an object whose vtable is UBioSWF's
(EN `0x820C6840`, RU `0x820C6810`) it scans the object (+0x3C..+0x54) for the `{data, num, max}` array holding exactly
the original compressed movie (size + FNV-1a 64 `0x36a776ba56672406`), inflates it (the SDK's
`stbi_zlib_decode_buffer`, compiled in `sdk/src/ui/image_decode.cpp`), rewrites sprite 11 (verified by FNV-1a 64
`0xe3572844573ca721` of its 230-byte tag) and stores the movie back uncompressed ("FWS"; `GUI_SF_Inventory` is
already stored that way, so GFx loads FWS movies) through the game's `appRealloc` (`sub_822100F8`, alignment 8 like
the game's own TArray growth). Same-size byte edits only (12 bytes in default mode): the 2-byte character id of each
of the 8 PlaceObject2 tags and the first letter of the 4 instance names; matrices untouched.

| Input mapping | What changes | Result (top / left / right / bottom) |
|---|---|---|
| default, by label (`input_xbox_layout = false`) | glyphs A<->B, X<->Y **and** clip names swapped, i.e. each named clip moves with its glyph | X / Y / A / B, the lit glyph is the label and the place of the Switch button to press |
| `input_xbox_layout = true` (by position) | only glyphs swapped; names stay (bottom clip is still `AButton`, lit when the game wants Xbox A = Switch bottom button B) | X / Y / A / B letters at the original positions; lit letter = Switch button to press |

The sequence the game asks for is unchanged; only where (and, in the second mode, under which letter) it is drawn.
Any other movie, a changed movie, or a second Serialize of the patched object is left alone. Cost: one inflate of
280 KB when BIOC_Materials loads; every other `UObject::Serialize` call pays one 4-byte vtable compare.

Cvar `masseffect_switch_minigame_layout` (bool, default `true`, init-only); `false` = the Xbox diamond.

Log lines: `[switch_minigame] GUI_SF_SkillGame.MiniGame: diamond set to the Switch layout (glyphs moved), BioSWF
XXXXXXXX` once; `[switch_minigame] ... diamond left as is` (warning, once) if the inflate, the sprite check or
`appRealloc` fails.

### 6.3 Host check (Mac, scratch only)

The movie was taken read-only from the RU `BIOC_Materials.xxx` (identical export in the EN Rev 1 copy, SHA-1
`6a28b2cd...`). `me_switch_minigame_patch.h` + stb inflate on the host: `IsOriginalCws` true, inflate 279,788 bytes,
patch true, a second patch false (idempotent). The patched sprite re-parsed: default mode
`bottom B/BButton, right A/AButton, left Y/YButton, top X/XButton` (12 bytes changed), relabel mode
`bottom B/AButton, right A/BButton, left Y/XButton, top X/YButton` (8 bytes changed); file length unchanged.

### 6.4 Console test

1. Build (EN, then `tools/edition.sh ru`), default toml. Normal log at start (BIOC_Materials loads at boot) must show
   the `[switch_minigame] ... diamond set to the Switch layout (glyphs moved)` line, and no `left as is` warning.
2. Any locked panel/locker (Eden Prime has several; the first is near the start of the dig site). Start the
   mini-game with A: the diamond must read X top, Y left, A right, B bottom. Press the lit button: the "correct" sound
   plays and the bypass succeeds; a wrong button fails as before. Screenshot the diamond with a lit glyph.
3. `masseffect_switch_minigame_layout = false`: the original Xbox diamond (A/B rollback).
4. Optional: `input_xbox_layout = true`: same picture (X/Y/A/B), the lit letter is the Switch button to press.

Risks: not run on the console yet. If `UObject::Serialize` is not where the Data array is filled for UBioSWF, the
hook finds nothing and stays silent (no log line): then hook the GFx loader instead (section 2.2). GFx reading FWS
for this movie and the 26 KB larger guest allocation are the only other new assumptions.

