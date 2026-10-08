# External function maps: public sources of ME1 / UE3 function names

Date: 2026-10-07. Web research only; nothing was downloaded except public source text read through GitHub. No game
binaries, PDBs or game data were fetched. Goal: find public material that helps name the hot guest functions of the
Xbox 360 `default.xex` (title id `4D5307E8`, PowerPC, UE3 2007 build) seen in the Switch profiles (main thread, UE3 render
thread). See [cpu-cost-analysis.md](cpu-cost-analysis.md) for the profiles themselves.

Short version:

- **Nobody has published symbols, a PDB, or a function map for the Xbox 360 ME1 xex.** No ME1 360 prototype/debug build
  is listed on Hidden Palace or TCRF (as of this search). Expect to name functions ourselves.
- The best public material is the **Legendary Edition (LE1) native-modding stack** (ME3Tweaks / llamathings): a
  generated SDK with every UnrealScript class, property and function (with flags), plus x64 RVAs and byte patterns
  for ~30 core engine functions. The x64 patterns cannot be matched against PPC bytes, but the **names, call graph
  shape, string references and object layouts** transfer.
- The **ME1 native table** in Legendary Explorer (index -> name for ~200 UnrealScript natives) lets us label the
  `GNatives[]` array in the xex once it is found, which names every `execXxx` thunk at once.
- **Xenia game patches** for this exact title give a handful of confirmed PPC addresses (frame limiter flag, frametime
  display flag, Coalesced hash check, MSAA tiling flag, post-process buffer size code). Already cited in
  [external-practices.md](external-practices.md); useful here as anchors.
- For automatic name transfer, the only public UE3 + Xbox 360 + PDB combination found is a **later UE3 game prototype on
  Hidden Palace (Fable Anniversary, 2013, release and debug xex with PDBs)**. Existence only; see the legal note in 4.
- Inside the xex itself, UE3 leaves many name strings (native lookup tables, stat names, `check()` file names, log
  categories). These are the most reliable way to name hot functions; section 3 lists them.

## 1. Ranked resources

| # | Resource | URL | What it gives | How it maps to 360 guest addresses | Usefulness |
|---|---|---|---|---|---|
| 1 | **LExSDKv2** (LE1/LE2/LE3 SDK, "Mass Effect 1 (Legendary Edition) (2.0.0.48602) SDK", generated with TheFeckless UE3 SDK Generator) | https://github.com/llamathings/LExSDKv2 (see `Src/LESDK/LE1/*`, `Src/LESDK/Init.hpp`) | Per package (`Core`, `Engine`, `SFXGame`, `SFXStrategicAI`, `BIOC_Materials`, `GFxUI`, `ISACTAudio`, `PlotManager`, ...): all classes with member offsets, all UFunctions with `FunctionFlags` (e.g. `FUNC_Native`) and parameters. `Init.hpp` has named x64 patterns/RVAs for `UObject::CallFunction`, `ProcessEvent`, `ProcessInternal`, `StaticAllocateObject`, `StaticConstructObject`, `UWorld::SpawnActor`, `UWorld::FarMoveActor`, `UEngine::Exec`, `UGameEngine::Tick` (RVA `0x3ca090`), `LoadPackage`, `LoadPackageAsyncTick`, `UTexture2D::Serialize`, `appLogf`, globals `GNatives`, `GObjObjects`, `GMalloc`, `GEngine`, `GWorld`, `GSys`, BioWare name pools (`SFXName`/`GBioNamePools`). | Not byte-compatible (x64, 2021 compiler). Use as a **name dictionary and layout reference**: (a) which UFunctions are native -> their `execXxx` thunk exists in the xex; (b) class sizes / member order to recognise `this`-offset patterns in PPC code; (c) the list of engine entry points worth locating first (Tick, ProcessEvent, CallFunction, SpawnActor). LE1 is the same game code recompiled; ME1-360 is an older UE3 branch, so expect offsets to differ but names and call structure to match. | High |
| 2 | **LExASIs** (successor of LE1-ASI-Plugins, moved Feb 2026) | https://github.com/ME3Tweaks/LExASIs (older: https://github.com/ME3Tweaks/LE1-ASI-Plugins, shared code https://github.com/ME3Tweaks/LEASIMods) | Working hooks with names: `UObject::ProcessEvent/ProcessInternal/CallFunction` (ConvoSniffer, FunctionLogger), `UGameViewportClient::InputKey`, `UBioConversation::StartConversation/EndConversation/SelectReply/QueueReply`, `CalcSceneView`, TLK lookup, `ProcessIni`, `CacheContent`, `RegisterTFC`, plus the LE1/LE2/LE3 RVAs of script VM error sites (Accessed None, divide by zero, sqrt of negative). FunctionLogger / KismetLogger / UnrealscriptDebugger show how the script VM is entered. | Names + "what the function does" for hooks that also exist in ME1-360. The VM error-site list is useful: those code paths contain unique log format strings that also exist in the 360 xex (e.g. "Accessed None"), so the 360 interpreter (`ProcessInternal`, `GNatives` dispatch) can be found via those strings. A FunctionLogger-style trace on PC LE1 tells which UnrealScript functions run every frame, i.e. which script natives are hot. | High |
| 3 | **Legendary Explorer (LEX)** native tables and bytecode readers | https://github.com/ME3Tweaks/LegendaryExplorer — `LegendaryExplorerCore/UnrealScript/Decompiling/ExtendedNativeTable.cs` (tables `ME1NativeTable`, `LE1NativeTable`, `UDKNativeTable`, ...), `LegendaryExplorerCore/ME1/Unreal/UnhoodBytecode/`, `Unreal/ObjectInfo/ME1UnrealObjectInfo.cs` | Native index -> function/operator name for **ME1 (original)** specifically (about 200 entries per game). Package format readers, so all script functions and their `iNative` indices in the 360 packages can be listed with the tool (from the user's own disc data, not downloaded). | Find `GNatives[]` in the xex (a 4096-entry table of function pointers filled by `GRegisterNative(index, &execXxx)` static initialisers, or one PPC table in `.data`). Then each slot index -> name from `ME1NativeTable`. Gives names for all numbered natives (math, string, vector/rotator ops, iterators, `Spawn`, `Trace`, timers). | High (cheap, exact) |
| 4 | **Xenia Canary game patches for ME1** | https://github.com/xenia-canary/game-patches/blob/main/patches/4D5307E8%20-%20Mass%20Effect%20(USA%20Rev%201).patch.toml (also `... Mass Effect.patch.toml` for World/EU/DE/IT xex hashes, and `(Japan)`) | Confirmed PPC addresses for: vsync/frame-limit target byte (`0x82233bc3` Rev 1, `0x822339b3` World), Show Frametime flag (`0x82eaf10c` / `0x82eaf12c`), Coalesced hash check (`0x826de80f` / `0x826df2bf`), skip intro (`0x8223b3f8` / `0x8223b1e8`), anisotropy (`0x826deeac` / `0x826df864`), MSAA tiling flag (`0x82e5dcb7` / `0x82e5dcd7`), post-process buffer size x/y and bloom kernel code (`0x82673870`/`0x826738e0`/`0x826738a4`/`0x826738cc`, World only). | Direct addresses (Rev 1 xex = our English build). The functions that read these bytes are: the engine frame-rate limiter (`UGameEngine::Tick`/`RHI present` area), the stats/frametime HUD, Coalesced loading, the post-process chain setup. Good anchors for naming neighbouring functions. Russian xex: shift by searching the same instruction bytes. | Medium-high (already partly used) |
| 5 | **UELib / UE Explorer** (EliotVU) | https://github.com/EliotVU/Unreal-Library (`src/Branch/UE3/SFX/EngineBranch.SFX.cs`) | Package/bytecode reader with a BioWare "SFX" engine branch (Mass Effect specific serialization and token handling). | Lets us dump all UFunctions with flags and native indices from the 360 `.xxx` packages locally, to cross-check LEX. | Medium |
| 6 | **UE Viewer (umodel)** by Gildor | https://github.com/gildor2/UEViewer (`Unreal/GameDefines.h`, `UnPackage3.cpp`, `UnTexture3.cpp`, `UnMesh3.h` - `MASSEFF` cases) | Exact ME1 package versions, Xbox 360 texture/mesh serialization quirks, compression. | Helps identify serializer functions (`UTexture2D::Serialize`, `USkeletalMesh::Serialize`, `FByteBulkData`) by the version checks they contain (constant compares against ME1 licensee version). Load-time, not per-frame. | Low-medium |
| 7 | **UDK documentation (UE3 "Three" docs)**, especially stat descriptions | https://docs.unrealengine.com/udk/Three/StatsDescriptions.html (mirror https://udn.epicgames.com/Three/StatsDescriptions.html; may return 403 to scripts) | Meaning of every UE3 `STAT_*` counter: e.g. STAT ANIM "SkelComp Tick Time", "Anim Tick Time", "UpdateSkelPose", "Get Bone Atoms", "Anim Decompression"; STAT GAME, PARTICLES, SCENERENDERING, etc. | The 360 xex (if built with stats, typical for UE3 2007) contains these display strings; each `SCOPE_CYCLE_COUNTER(STAT_X)` site references the stat id. Map stat id -> the functions that open the scope -> that names the hot functions (skinning, anim blend, particle tick, visibility). | Medium-high |
| 8 | **UDK UnrealScript sources** (Core/Engine `.uc` in any UDK install, and LEX's `UDKNativeTable`) | UDK installer (Epic, public) / https://github.com/ME3Tweaks/LegendaryExplorer | `native(N)` indices of Object.uc / Actor.uc (stable since UE2), Kismet `SequenceOp` class names, `Actor.Tick`, `ProcessEvent` semantics. | Cross-check of the ME1 native table; tells which `exec` functions are Core natives versus BioWare additions. | Medium |
| 9 | **Public UE2 headers** (UT2004 v3369 public headers; UT99 v432/v436 headers, OldUnreal) and **UE4/UE5 source** (Epic GitHub, account required) | e.g. OldUnreal on GitHub; https://github.com/EpicGames/UnrealEngine (private-by-account) | C++ declarations of `UObject::ProcessEvent`, `FFrame`, `GNatives`, `FName` hash (`appStrihash`, CRC table), `TArray`, `FMemStack`, actor tick loop. UE3 sits between UE2 and UE4. | Structure-level matching: `appStrihash` uses the CRC32 table (`0x04C11DB7`), findable as a constant table -> `FName::Init` / `FName::Hash`. `FFrame::Step` loops are recognisable by `GNatives[*Code++]` indirect calls. (Note: LE1 uses BioWare `SFXName` pools; check whether ME1-360 already does.) | Medium |
| 10 | **ME3 PC native modding** (x86, ME3 is UE3 like ME1 PC 2008) | https://github.com/ME3Tweaks/ME3-ASI-Plugins, https://github.com/Erik-JS/ME3-ASI | x86 patterns for ProcessEvent etc. in a 32-bit BioWare UE3 build. | Only names/structure; not PPC. Closer in era to ME1 than LE1 is. | Low-medium |
| 11 | **ME1 PC (2008) native modding** | https://github.com/Erik-JS/masseffect-binkw32 (ME1 ASI loader) | Only the loader (binkw32 proxy); no ME1 function addresses or patterns are published there. No public ME1-2008 SDK was found. | None directly. | Low |
| 12 | **ME1 PC configuration knowledge** (PCGamingWiki, Mass Effect wiki "PC Tweaks", Nexus MERLIN mod) | https://www.pcgamingwiki.com/wiki/Mass_Effect , https://masseffect.fandom.com/wiki/PC_Tweaks , https://www.nexusmods.com/masseffect/mods/242 | Which `BIOEngine.ini` `[SystemSettings]` switches exist and what they cost: `DynamicShadows`, `DepthBias` (patch 1.02 default 0.030 effectively removes dynamic shadows; 0.012 restores), `MaxShadowResolution`, `ShadowFilterQualityBias`, `DepthOfField`, `MotionBlur`, `Bloom`, decals, light environments. MERLIN restores 360-level dynamic lights on PC, i.e. documents which lighting features the 360 build has. | These keys are config strings in the xex/Coalesced; xrefs to them land in `FSystemSettings` and in the renderer code they gate. Useful for A/B variants (see [engine-settings-ab.md](engine-settings-ab.md)). | Medium |
| 13 | **ME1 Controller (Dybuk)** | https://www.nexusmods.com/masseffect/mods/60 | Reactivates the 360 UI on PC at script/package level. | No native functions. | Low |

## 2. Performance knowledge about ME1 on 360 / PC

- No Digital Foundry (or other) frame-time analysis of the original 2007 360 game was found. Contemporary reports
  (GameBanshee PC preview, https://www.gamebanshee.com/xkwr ; stum.de 2008 article
  https://stum.de/2008/04/26/mass-effect-hiding-rough-edges-under-a-shell-of-awesomeness/) note heavy texture pop-in on
  360 and streaming hitches; BioWare said (Casey Hudson, ME2-era interviews, e.g. PCGH
  https://www.pcgameshardware.de/Mass-Effect-2-Spiel-14067/Tests/Mass-Effect-2-angespielt-Das-ist-neu-bei-der-modifizierten-Unreal-Engine-3-maximal-30-Fps-auch-auf-dem-PC-693316/)
  that ME1 had no time for a final memory/performance budget pass, and ME2 got a new LOD system and better texture streaming.
  Elevators hide level streaming. Implication for us: texture streaming (`UTexture2D` mip updates, `FStreamingManager`
  ticks) and level streaming run on the main thread every frame and are known weak points of this build.
- BenQ retrospective: ME1 360 runs below 720p and reaches 30 fps only in some scenes (the original is frequently below 30).
- ME1 PC patch 1.02 changed the default `DepthBias` so most dynamic shadows disappear, a sign that dynamic shadows were the
  known expensive feature on PC.
- No public "performance mod" for ME1 patches native code; all known PC tweaks are ini switches (section 1, item 12).

## 3. How to use these with the xex (practical recipe)

1. **Strings first.** Search the decrypted xex for: `exec` + name ASCII strings (UE3 console builds link statically and
   register natives through `FNativeFunctionLookup {"execFoo", &AFoo::execFoo}` tables, `GNativeLookupFuncs`; if present
   this names every native thunk); `STAT_`/stat display names (see item 7); `check()`/`appFailAssert` file names such as
   `UnObj.cpp`, `UnActor.cpp`, `UnSkeletalMesh.cpp`, `UnParticleComponents.cpp`, `UnSequence.cpp` (Kismet), `UnLevTic.cpp`
   (actor ticking), `UnStreaming.cpp`, `UnSkeletalRender*.cpp` (CPU skinning on 360 is typically in
   `FSkeletalMeshObjectCPUSkin` / `SkinVertices`); log strings ("Accessed None", "Runaway loop", "Infinite script recursion")
   for the script VM.
2. **GNatives.** From `ProcessInternal`/`FFrame::Step` (found via the VM strings) read the table base; label slots with
   `ME1NativeTable` from LEX (item 3).
3. **Class layout.** Use LExSDKv2 class headers (item 1) to recognise `AActor`, `UPrimitiveComponent`,
   `USkeletalMeshComponent` member offsets in PPC loads (offsets differ, member order mostly the same).
4. **Anchors.** Start from the Xenia patch addresses (item 4) and the vtables of known classes (each UClass has a
   `StaticClass` registration with its name string, so vtables can be named by class).
5. **Optional automated transfer:** BinDiff/Diaphora from a UE3 Xbox 360 binary that has a PDB (section 4) to our xex.
   Expect useful matches only for Core/Engine code that changed little between UE3 2007 and the donor's UE3 build.

## 4. Symbols / debug builds (existence only, nothing downloaded)

- **ME1 (360, PC, LE1): none found.** No PDB, MAP, debug xex or symbol dump is public for any ME1 version. The ME3 Xbox 360
  beta that leaked in Nov 2011 (Xbox LIVE preview program accident) is a different game/engine branch; no report says it
  shipped symbols.
- **Other UE3 Xbox 360 builds with symbols on Hidden Palace:**
  - Fable Anniversary (Apr 27, 2013 prototype), UE3 (build string `UE3-CNSHAW1093`): `WellingtonGame-Xbox360-Debug.xex` and
    `...-Release.xex`, each with an associated PDB. https://hiddenpalace.org/Fable_Anniversary_(Apr_27,_2013_prototype)
    A late UE3 (2012-2013) PPC build with full symbols: the most promising donor for function-signature transfer of
    Core/Engine code, but six years newer than ME1's engine.
  - Dark Void (Nov 4, 2009 prototype, UE3): has `SkyGame-XeReleaseLTCG-DebugConsole.xex`; symbol files not mentioned.
    https://hiddenpalace.org/Dark_Void_(Nov_4,_2009_prototype)
  - Enslaved (May 31, 2010 prototype, UE3 debug build): disc image, symbols not mentioned.
    https://hiddenpalace.org/Enslaved:_Odyssey_to_the_West_(May_31,_2010_prototype)
- Legal note: these prototypes are unreleased copyrighted builds. Using them is the user's decision; this document only
  records that they exist.
- Full UE3 C++ source was licensee-only and is not public; leaked copies are not considered here.

## 5. What did not exist / dead ends

- No ME1-2008 PC SDK, no ME1 PC ASI plugin repo with function patterns (only the loader).
- No Digital Foundry article on ME1 360 performance.
- No ME3Tweaks wiki page on engine internals with native function addresses (the SDK and LEX source are the real references).
- The TCRF page for Mass Effect returned no usable technical content to the fetcher.
