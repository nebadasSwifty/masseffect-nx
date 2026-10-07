# External practices, part 2: engine settings, UE3 port techniques, heavy locations

Date: 2026-10-07. Desk research (web) plus two local, read-only checks: the decoded console `Coalesced.ini`
(`out/disc-error/Coalesced-console-before.ini`) and a loaded guest image dump of our edition (`MASSEFFECT_DUMP_IMAGE`,
16.5 MB, made 2026-10-05; UTF-16BE string scan). Nothing was built or run on the console. Benefits marked
"unmeasured" are guesses used only to rank the work.

Part 1 ([external-practices.md](external-practices.md)) covers Xenia's render target cache, XenonRecomp /
UnleashedRecomp, and Xenia's ME1 patches (MSAA off, blur radius, 30/60 fps byte, frametime display). This file does not
repeat them. It also skips what the backlog already measured: `ShadowFilterRadius`, `MaxShadowResolution`, decals,
`MeshLODRange`, `m_nDefaultMotionBlur`, `bUsePostProcessEffects`, `StartupResolution` (t175-t298, S13).

## 0. Two facts that decide which settings can work at all

**The console build of 2007 has no `[SystemSettings]`.** The settings PC players tune (`DynamicShadows`,
`LightEnvironmentShadows`, `CompositeDynamicLights`, `SkeletalMeshLODBias`, `ParticleLODBias`, `DetailMode`,
`OneFrameThreadLag`, ...) live in the PC `BIOEngine.ini` `[SystemSettings]` section
([Steam guide](https://steamcommunity.com/app/17460/discussions/0/540739405440339556),
[Tech Enhanced Mass Effect](https://stepmodifications.org/forum/topic/8278-tech-enhanced-mass-effect/)). In later UE3,
`OneFrameThreadLag` is a `FSystemSettings` entry, "Whether to allow the rendering thread to lag one frame behind the game
thread" ([UDK SystemSettings](https://docs.unrealengine.com/udk/Three/SystemSettings.html)). None of these strings exist
in our guest image: no `SystemSettings`, `OneFrameThreadLag`, `SkeletalMeshLODBias`, `ParticleLODBias`, `DetailMode`,
`LightEnvironmentShadows`, `CompositeDynamicLights`. The console `Coalesced.ini` has no such section either. Adding the
section to `Coalesced.ini` will do nothing. The same effects need code hooks (sections 1.2, 2).

**Which `Coalesced.ini` keys are dead.** Keys of non-UObject sections are read by native code with a literal name, so a
key whose UTF-16 name is not in the image is never read. Keys of class sections (`[Engine.Engine]`, `[BIOC_Base.*]`)
are script `config` properties: their names live in the script packages, so this test says nothing about them.

| Section (Xe-BIOEngine) | Read natively (name in image) | Not read |
|---|---|---|
| `[Engine.RenderDevice]` | none | `MeshLODRange`, `bUsePostProcessEffects`, `bForceCPUSkinning`, `bUseTextureLOD`, `UserMaxTextureSize`: matches t175 and t298 (no effect) |
| `[D3DDrv.D3DRenderDevice]` | none | `MaxTranslucencyLayers`, `DisableHWShadowMaps`, `MaxTextureSize`, shader versions (PC leftovers) |
| `[WinDrv.WindowsClient]` | `AudioDeviceClass` | `StartupResolutionX/Y` (matches t212) |
| `[XeD3D]` | `RBSecondarySize`, `RBSegmentCount`, `DrawUPVertexCheckCount`, `DrawUPIndexCheckCount`; also `RBPrimarySize`, `DefaultBB` (not in the ini, defaults used) | |
| `[TextureStreaming]` | all 11 keys | |
| `[Engine.Physics]` | `NxTimeStep`, `NxMaxDeltaTime`, `NxMaxSubSteps`, `ForceEnableCCD` | |
| `[Core.System]` | `MaxObjectsNotConsideredByGC`, `SizeOfPermanentObjectPool`, `AsyncIOBandwidthLimit` | |
| `[Engine.Client]`, `[XeDrv.XenonClient]` | `MinDesiredFrameRate` | |
| `[ISACTAudio.ISACTAudioDevice]` | `TimeBetweenHWUpdates` | `MaxChannels` may still be a script config property |

Script-side keys that look relevant and have not been tested yet: `[Engine.Engine] TimeBetweenPurgingPendingKillObjects=30`,
`bEnableBranchingPCFShadows=True`, `BranchingPCFQuality=0`, `MinShadowResolution=32`;
`[BIOC_Base.BioWorldInfo] m_fMaxVFXBudget=100.0`; `[BIOC_Base.BioVisualEffectPool] m_fDecayPeriod=60.0`; the corpse
cleanup keys in `[BIOC_Base.BioPawn]`-type sections (`m_fCorpseCleanupFirstAttemptTime=5.0`,
`m_fMaxCorpseCleanupScreenSizeThreshold`).

A useful by-product: the image holds the predicated-tiling mode table strings `1280x720 1xAA, 1 tile`,
`1280x720 2xAA, 2 tiles`, `2xAA, 3 tiles`, `4xAA, 3 tiles` at `0x82191550..0x821915DC`, next to the `[XeD3D]` keys.
Its code references are a direct way to confirm what the Xenia "Black Shading Fix" byte (part 1, #1) selects.

## 1. Ranked list

| # | What | Where | Benefit | Image change | Risk |
|---|---|---|---|---|---|
| 1 | Make occlusion queries real, or at least stop drawing the query boxes | native renderer `PM4_EVENT_WRITE_ZPD` (`app/src/native/me_native_system.cpp:1194`) | **high** in interiors (Citadel, Normandy, Noveria labs): fewer draws on the render, ring and GPU side | none if exact; one-frame pop-in if results lag | medium |
| 2 | One frame of lag between the game thread and the render thread | the frame-end wait `sub_822FE760` and its callers | **high** if the frame-end call does a full sync today: game and render work overlap | none (input latency +1 frame) | medium |
| 3 | Larger D3D ring buffer | `[XeD3D] RBSecondarySize=3145728`, `RBSegmentCount=48` | medium if the render thread waits for ring segments | none | low |
| 4 | The game's own drop-detail switch | `[Engine.Client]` and `[XeDrv.XenonClient] MinDesiredFrameRate=35` | low to medium (particles, effects, LOD) | yes | low |
| 5 | GC hitches: interval and root-set size | `[Engine.Engine] TimeBetweenPurgingPendingKillObjects=30`, `[Core.System] MaxObjectsNotConsideredByGC=89500` | removes a periodic hitch; fewer objects to mark | none | low (memory) |
| 6 | Texture pool size | `[TextureStreaming] PoolSize=100` | less mip churn and pop-in, less streaming CPU | better | guest memory |
| 7 | Audio update rate and voice count | `[ISACTAudio.ISACTAudioDevice] TimeBetweenHWUpdates=15`, `MaxChannels=64` | low (audio thread ~4.6 core-ms/frame) | none (audio changes) | low |
| 8 | Native particle distribution lookup | hot function `824DD848` (3.6 % of the main thread) | low to medium (main thread) | none if bit-exact | low |
| 9 | No skeletal update for meshes not rendered | `USkeletalMeshComponent` tick (find by xref) | medium in crowds (Citadel, Wards) | none on screen | medium (gameplay reads bones) |
| 10 | Throttle dynamic light environment updates | light environment tick (find by xref) | medium if it shows in the profile | slight (character lighting lags) | medium |
| 11 | Memory clock (EMC) | Horizon OC profile, user decision | medium on GPU-bound views | none | stability |
| 12 | PhysX step 50 Hz to 30 Hz | `[Engine.Physics] NxTimeStep=0.02` | low (simulation is ~0.85 ms on its own thread) | physics behaviour | medium (Mako) |

### 1.1 Occlusion queries are faked today (top priority)

- **What we do now.** On `EVENT_WRITE_ZPD` the native renderer writes 1000 samples for every finished query
  (`me_native_system.cpp:1203-1207`, the same fallback as the SDK's `query_occlusion_fake_sample_count = 1000`). The
  game sees every occlusion-tested primitive as visible.
- **Why it matters.** UE3 uses hardware occlusion queries to skip primitives hidden behind walls. The image carries
  `FOcclusionQueryVertexShader`, the bounding-box shader UE3 draws the queries with. The render thread polls them
  (`sub_826E7C98`, the poll `masseffect_wait_occlusion_us` already shortens). Epic's UE3 profiling pages name
  visibility and draw submission as the two big render thread costs, both driven by the number of visible primitives
  ([UDK render thread profiling](https://docs.unrealengine.com/udk/Three/RenderThreadProfilingHome.html)). With fake
  results we pay twice: for the query box draws, and for every hidden primitive drawn behind them. The ring thread
  costs about 35 us per draw (backlog), and the scene is vertex-bound (M7, M8).
- **Step A, no image change, cheap.** Count the draws between a ZPD begin and end (colour and depth writes off) per
  frame. While results stay fake, the host can skip those draws: nothing reads their output.
- **Step B, upper bound.** A diagnostic that reports 0 samples (everything occluded) breaks the image but shows the
  most real queries could save.
- **Step C, real results.** Begin and end a `VK_QUERY_TYPE_OCCLUSION` query (pool indexed by guest address), and write
  `ZPass_A`/`Total_A` byte-swapped when the slot's fence signals. The SDK's Vulkan command processor already has an
  occlusion query path (`occlusion_query_enable`, `sdk/src/graphics/vulkan/command_processor.cpp:659, 4909, 4940`) to
  borrow from. The render thread blocks on results, so if the GPU runs a frame behind, answer with the previous
  result for the same address and treat new queries as visible. That gives one-frame pop-in when the camera turns
  fast. Measure both.
- **Benefit / effort / risk.** High in closed spaces, unmeasured / A: low, C: medium to high / A: none; C: pop-in.

### 1.2 Game-thread / render-thread sync (one frame of lag)

- **Background.** UE3 syncs the threads at the end of each frame. With `OneFrameThreadLag` the game thread may run one
  frame ahead, which raises frame rate at the cost of input latency
  ([Epic, low latency frame syncing](https://dev.epicgames.com/documentation/unreal-engine/low-latency-frame-syncing-in-unreal-engine),
  [Blur Busters on UE3 OneFrameThreadLag](https://forums.blurbusters.com/viewtopic.php?p=1978)). The 2007 console build
  has no setting for it (section 0), so whatever it does is hard-coded.
- **What to check.** `sub_822FE760` is `while (*counter > n) Sleep(0)`, the shape of UE3's
  `FRenderCommandFence::Wait(NumFencesLeft)`. The main thread spent 19 to 44 % of its samples there (t220, t223). Read
  the `n` argument at each call site. If the frame-end call passes 0, it is a full sync. Passing 1 (wait for the
  previous frame's fence) gives the one-frame lag later UE3 games ship with by default.
- **Benefit / effort / risk.** Potentially the largest CPU-side gain left: the main thread (~31 ms) and the render
  thread would overlap instead of taking turns / low (one hooked argument) / medium: 2007 code may touch render data
  from the game thread on the assumption of a full sync. Watch for flicker on particles and dynamic meshes, and
  check the occlusion poll still finds its results.

### 1.3 `[XeD3D]` ring buffer size

- `RBSecondarySize=3145728` (3 MB) and `RBSegmentCount=48` (64 KB segments) size the D3D command ring the render thread
  writes and our ring thread drains. When the render thread runs out of free segments it waits for the GPU (the ring
  thread). C23 found the game "waits on something else (fences/semaphores)" in `sub_8222FA98`. Ring segment
  exhaustion is a candidate.
- Test: 3 MB to 8 MB with 128 segments, then 16 MB. It only helps if the ring thread has idle time (it does at 960x544,
  where the GPU idles 22-40 ms). The memory comes from guest physical memory.
- **Image change:** none. **Risk:** low (guest memory; up to one more frame of latency).

### 1.4 `MinDesiredFrameRate=35`: the game's own detail switch

- In Unreal, the world sets `bDropDetail` ("frame rate is below DesiredFrameRate, so drop high detail actors") and
  `bAggressiveLOD` ("frame rate is well below DesiredFrameRate, so make LOD more aggressive") every frame
  ([UDK WorldInfo.uc](https://github.com/npruehs/hostile-worlds/blob/master/src/Hostile%20Worlds/Development/Src/Engine/Classes/WorldInfo.uc)).
  The threshold is `UClient::MinDesiredFrameRate`. From UE2 memory (to verify in our code by an xref to the
  `MinDesiredFrameRate` read), drop-detail applies below `MinDesiredFrameRate` and aggressive LOD below
  `MinDesiredFrameRate - 5`. With 35, the 360 already ran in drop-detail mode at 30 fps, and aggressive LOD switches on
  whenever a frame takes longer than 33.3 ms. Script and particle code reads these flags (for example
  `ParticleModuleCollision.bDropDetail`, emitter pools, gibs:
  [UDK code search](https://github.com/search?q=repo%3Anpruehs%2Fhostile-worlds+bDropDetail&type=code)).
- Test both directions: `MinDesiredFrameRate=100` (always aggressive, cheapest) and `=1` (never: the image the
  designers meant, the cost reference).
- **Image change:** yes (fewer effects, coarser LOD). **Risk:** low. The flag can flip mid-frame-rate today, which can
  also cause visible LOD flipping between 28 and 34 fps. Pinning it may look steadier.

### 1.5 Garbage collection

- `TimeBetweenPurgingPendingKillObjects=30`: a full mark and purge every 30 s of game time. In UE GC this is the classic
  periodic hitch ([Lars Tofus on UE GC spikes](https://larstofus.com/2024/07/21/unreals-garbage-collection-spikes-and-how-to-fight-them/),
  [UE forum: GC hitching 30-60 s after play](https://forums.unrealengine.com/t/garbage-collection-causes-hitching-stuttering-30-to-60-seconds-after-play/449000)).
- `MaxObjectsNotConsideredByGC=89500` and `SizeOfPermanentObjectPool=14050918` were copied from a debug print (the ini
  comment says so). Objects below that index are never marked. If our startup creates a different root set, the value is
  stale: either GC marks objects it could skip, or (if too high) objects that should be collected are not.
- **Do first:** time `CollectGarbage` (find it by the `TagGarbage` / `COLLECTGARBAGE` / `GCONLY` strings at
  `0x820E7104`, `0x8206072C`, `0x820E79F4`) and log its duration. Count the `[hitch]` events in a 120 s walk.
- Options: interval 30 to 90-120 s; a GC on level transitions only (the game already forces one through
  `USeqAct_ForceGarbageCollection`); correct the root-set count.
- **Image change:** none. **Risk:** memory growth between collections.

### 1.6 Texture pool and streaming

- Console `PoolSize=100` (MB; the comment says Final Release adds 15 MB in code). The PC default is 140, and PC players
  raise it to remove pop-in and stutter ([Steam ME1 thread](https://steamcommunity.com/app/17460/discussions/0/540735426610360573),
  [Tech Enhanced ME](https://stepmodifications.org/forum/topic/8278-tech-enhanced-mass-effect/)). Digital Foundry's PS3
  face-off credits the PS3 port's install for "substantially reduced texture pop-in", and BioWare says the PS3 port
  "tinkered with the audio engine" and changed "the way that memory was allocated", with "huge gains in performance -
  especially with regard to texture popping" ([PlayStation Blog](https://blog.playstation.com/2012/12/05/back-to-the-beginning-bringing-mass-effect-to-ps3/),
  [GameBanshee summary of DF](http://www.gamebanshee.com/k3qpk)).
- Raising the pool lowers the fudge-factor churn (the streamer stops dropping and re-requesting mips) and the upload
  traffic on the ring thread. Test 100 to 140, then 180, and watch guest physical memory (512 MB).
- `[TextureLODSettings] TEXTUREGROUP_World/WorldNormalMap ... LODBias=2` already drops two mips on world textures on the
  360. Going down to 1 would sharpen the image at more memory and bandwidth cost. Not a speed knob.
- **Image change:** better (less pop-in). **Risk:** guest out-of-memory.

### 1.7 ISACT audio

- `TimeBetweenHWUpdates=15` ms (natively read) sets how often the audio device updates the hardware voices.
  `MaxChannels=64`. The audio mixer guest thread costs ~4.6 core-ms per frame at 1785 MHz
  ([cpu-cost-analysis.md](cpu-cost-analysis.md)). A 2009 PC report found ME1's worst slowdowns came from the
  Creative audio driver path, not the GPU ([AnandTech forum](https://forums.anandtech.com/threads/mass-effect-1-horrible-performance.2045354)).
- Test 15 to 30 ms, and 64 to 48 or 32 channels.
- **Image change:** none (audio: fewer simultaneous sounds, slower parameter updates). **Risk:** low.

### 1.8 Hot main-thread natives the profile already names

The main thread's flat profile (cpu-cost-analysis.md, t265) lists:

- `824DD848` (3.56 %): looks like UE3's `FRawDistribution` lookup, the table every particle module samples per particle
  per frame. It is a good native NEON target (bit-exact lerp).
- `82B5F0E8` "volume overlap" (2.1 %): a likely candidate for light environment or trigger volume checks.
- `wcscmp` `82AC4520` (~1 %): string compares in a per-frame path, most likely `FName` or config lookups. Find the callers.

The UE3 stat budgets shipped in the same ini show what the 360 team expected per frame: tick 10 ms, skeletal component
tick 8 ms, anim tick 4 ms, skinning 2 ms, script 4 ms, Kismet 4 ms, particles 2 ms each for tick, update and spawn
(`[Stats.Perf]`, `[Stats.Vfx]`). They make a good checklist for the stack profile.

### 1.9 Skeletal meshes and light environments that are not on screen

- In UE3 `USkeletalMeshComponent` defaults to `bUpdateSkelWhenNotRendered=TRUE` and `bTickAnimNodesWhenNotRendered=TRUE`
  ([UDK SkeletalMeshComponent.uc](https://github.com/npruehs/hostile-worlds/blob/master/src/Hostile%20Worlds/Development/Src/Engine/Classes/SkeletalMeshComponent.uc)).
  Characters behind the camera are still animated. A hook in the component tick that skips the pose update when
  `LastRenderTime` is older than about 0.5 s cuts game-thread animation cost in crowds (Citadel wards, Presidium).
  Keep an allow-list: the player, squad, conversation actors, and anything with attachments the AI aims at.
- Dynamic light environments update with line checks to every relevant light, and Epic's guidance is "probably
  shouldn't be more than 50 of these active at any given time" with the default `bDynamic=True` setting. Later UE3 adds
  `InvisibleUpdateTime=5.0` and `MinTimeBetweenFullUpdates=1.0` to throttle them
  ([UDK PerfDebug_DynamicLightEnvironments](https://docs.unrealengine.com/udk/Three/PerfDebug_DynamicLightEnvironments.html),
  [UDK DynamicLightEnvironmentComponent.uc](https://github.com/npruehs/hostile-worlds/blob/master/src/Hostile%20Worlds/Development/Src/Engine/Classes/DynamicLightEnvironmentComponent.uc)).
  The 2007 code may predate these throttles. If the light environment tick shows in the main thread's stack profile,
  throttle invisible ones in a hook. The image changes slightly: character lighting updates late.
- **Effort.** Medium for each: find the tick functions by their class names (`ULightEnvironmentComponent` at
  `0x8211BD4C`, the skeletal component exec table) and the vtables.

### 1.10 Clocks: memory

- Memory is 1600 MHz docked, and the docs say "the port never raises it" ([platform-notes.md](platform-notes.md)). The
  overclock decision of 2026-10-04 covers CPU 1785 and GPU 768 only. The Switch OC community calls memory "the single
  largest bottleneck" and runs Erista at 1862 MHz (some at 1996) with little extra power
  ([sys-clk](https://github.com/CTCaer/sys-clk), [Nintendo Life OC thread](https://nintendolife.com/forums/nintendo-switch/official_switch_overclocking_thread),
  [GBAtemp Switch OC suite](https://gbatemp.net/threads/switch-oc-suite.631519/post-10152501)).
  Chips and Cheese measured 25.6 GB/s docked and early spills to DRAM
  ([Chips and Cheese](https://chipsandcheese.com/p/nintendo-switchs-igpu-maxwell-nerfed-edition)).
- This is the user's call. A 1862 MHz A/B on Eden Prime grass and on the 1280x720 mode would show how much of the GPU
  side is bandwidth.

### 1.11 PhysX step

`NxTimeStep=0.02` (50 Hz), `NxMaxSubSteps=5`, `NxMaxDeltaTime=0.333`. At 30 fps the engine runs 1 or 2 steps per frame
(about 1.67 on average). Setting 0.0333 makes it exactly one. The simulation is ~0.85 ms per frame on its own thread
(t225-t227), so the gain is small. The Mako, ragdolls and grenades behave differently. Low priority.

## 2. UE3 console-port practice (what other ports changed)

- **BioShock: The Collection on Switch (Virtuos / Blind Squirrel, UE2.5 and UE3).** Dynamic resolution (Infinite down
  to ~810p docked), shorter LOD distances for geometry and grass, lower-resolution reflections and effects, and
  **texture fade-in instead of pop-in** to hide the shorter distances
  ([Nintendo Everything on DF](https://nintendoeverything.com/bioshock-the-collection-switch-tech-analysis/)).
  Virtuos' biggest XCOM 2 cost was memory (from 7 GB to 3.2 GB), and they wrote a rendering library that renders the
  original UE3 module on Switch unchanged ([GoNintendo](https://gonintendo.com/stories/374912-virtuos-talks-about-the-biggest-hurdles-in-bringing-xcom-2-collec)).
- **Borderlands Legendary Collection, Batman Arkham (Turn Me Up Games).** A proprietary layer runs UE2.5/UE3 titles
  natively on Switch. Their hardest spots were "character models heavy with particle effects"
  ([GoNintendo](https://gonintendo.com/stories/364417-turn-me-up-games-created-a-proprietary-tool-to-bring-unreal-engin)).
  Reviews note almost no AA and visible texture pop-in ([TheSixthAxis](https://www.thesixthaxis.com/2020/06/04/borderlands-legendary-collection-review-nintendo-switch/)).
- **Edge of Reality's ME1 PS3 port (2012).** Audio engine and memory allocation changes, texture pop-in "dramatically
  improved". Chora's Den and Noveria's cliffs still ran around 15 fps on both consoles (sources in 1.6).
- **What this means here.** Commercial ports cut LOD, grass distance, effects resolution and AA, and fix the texture
  streaming. On the CPU side they owned the renderer (as UnleashedRecomp does, part 1). For a recompilation the
  equivalent CPU levers are the ones above: fewer draws (1.1), thread overlap (1.2), and fewer per-frame updates of
  things nobody sees (1.9). Skinning is already on the GPU (`bForceCPUSkinning=False`; the 360 path uses
  vertex-shader skinning), so "move skinning to the GPU" does not apply. What remains is the bone matrix upload per
  draw (`skin rebind 8264ADA0` in the render thread).
- **Precomputed visibility** came to UE3 after 2007 and is not in ME1, so occlusion queries (1.1) and cull distances
  are the only visibility culling the game has.

## 3. NVK / Maxwell notes not covered in part 1 or mesa.md

- NVK on desktop Maxwell is "stuck forever at boot clocks" because nouveau cannot reclock it
  ([Collabora, NVK enabled for Maxwell](https://www.collabora.com/news-and-blog/news-and-events/nvk-enabled-for-maxwell,-pascal,-and-volta-gpus.html)).
  That limit does not apply here: Horizon sets the clocks. Linux/L4T reclocking advice (switchroot, nouveau DVFS) is
  irrelevant for a Horizon homebrew.
- NAK only gained real instruction scheduling for Kepler in Mesa 25.2
  ([Phoronix](https://www.phoronix.com/news/Nouveau-NAK-Faster-Kepler)). Dual issue and functional-unit tracking are
  "still to be implemented". Maxwell's control codes also encode dual issue
  ([Chips and Cheese, Maxwell](https://chipsandcheese.com/p/maxwell-nvidias-silver-28nm-hammer)). The local patch
  already raised the texture latency assumption (mesa.md). Dual issue on SM50 is a further, unexplored compiler item
  for the ALU-heavy post shaders.
- NVK routes draws and dynamic state through MME macros, with a separate Fermi-MME builder for pre-Turing
  ([mesa MR 30703](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/30703),
  [nouveau MR 178](https://gitlab.freedesktop.org/nouveau/mesa/-/merge_requests/178)). If the GPU front end ever shows
  up as the limit at 1500+ draws, look at the MME path per draw. Fewer draws (1.1) is the cheaper fix.

## 4. Location test points

The game's own debug travel list in `Coalesced.ini` names every map and start point (`AT <map> <start>` entries). Our
`--mapa` sets `LocalMap`; a start point may be reachable as `LocalMap=BIOA_STA00#start_STA20_01` (UE URL portal syntax,
unverified on this build). Known heavy spots come from the DF PS3 face-off (Chora's Den, Noveria cliffs at ~15 fps on
both consoles) and from PC reports (the Normandy bridge and the Presidium are CPU-bound even on fast PCs:
[EA forum, Citadel FPS drops](https://forums.ea.com/discussions/mass-effect-franchise-discussion-en/me1-citadel-fps-drops/9119911),
[AnandTech](https://forums.anandtech.com/threads/mass-effect-1-horrible-performance.2045354)).

| Order | Map / start | Place | Why it is heavy | Earlier sweep |
|---|---|---|---|---|
| 1 | `BIOA_PRO00` / `Start_PRO10_01` | Eden Prime landing zone | grass, terrain, long view, combat, the black-terrain bug | PRO00 28.1 fps; PRO10 did not start |
| 2 | `BIOA_NOR00` / `Start_NOR10_01` | Normandy, CIC and cockpit | many characters, interior lights, CPU-bound on PC | NOR00 27.4 |
| 3 | `BIOA_STA00` / `start_STA20_01` | Citadel Presidium | long open interior, crowds, water, CPU-bound on PC | STA00 23.6 (lowest) |
| 4 | `BIOA_STA00` / `Start_STA60_01` | Wards, Chora's Den | DF's 15 fps spot: dancers, translucency, lights | |
| 5 | `BIOA_STA00` / `Start_STA70_01` | Council tower and chamber | big hall, cutscene DOF | |
| 6 | `BIOA_WAR00` / `Start_WAR20_01` | Feros, Zhu's Hope colony | NPCs, lighting, skyway view | WAR00 28.8 |
| 7 | `BIOA_WAR00` / `Start_WAR40_01` | Feros war zone (Mako on the skyway) | vehicle physics, long view, geth combat | |
| 8 | `BIOA_ICE00` / `Start_ICE25_01` | Noveria, Aleutsk Valley (Mako) | DF's other 15 fps spot: snow cliffs, particles, wind | ICE00 29.3 |
| 9 | `BIOA_ICE00` / `Start_ICE20_01` | Port Hanshan | interior hub, NPCs | |
| 10 | `BIOA_ICE00` / `Start_ICE70_01` | Binary Helix hot labs | interior combat, effects | |
| 11 | `BIOA_JUG00` / `Start_JUG20_01` | Virmire beach (Mako) | open terrain, foliage, AA turrets | JUG00 32.5 |
| 12 | `BIOA_JUG00` / `Start_JUG80_07` | Virmire central trench, bomb site | heavy combat, effects, Saren | |
| 13 | `BIOA_JUG00` / `Start_JUG70_10` | Virmire ocean view facade | long view, water | |
| 14 | `BIOA_LAV00` / `Start_LAV60_01` | Therum, Ring of Fire | lava, heat-haze distortion, Mako | LAV00 31.4 |
| 15 | `BIOA_LOS00` / `Start_LOS50_01` | Ilos trench run (Mako) | fast camera, streaming, many geth | |
| 16 | `BIOA_LOS00` / `Start_LOS40_01` | Ilos archives | large interior | |
| 17 | `BIOA_END00` / `Start_END70_00` | Final Citadel plaza | final battle, Sovereign, effects | END00 did not start |
| 18 | `BIOA_END00` / `Start_END80_00` | Citadel tower space walk | open sky, heavy effects | |
| 19 | an uncharted world, for example `BIOA_UNC21` (sand) | Mako on open terrain | terrain LOD, streaming during fast travel | |

What to record per point: fps per 10 s, main/render/ring core-ms per frame, draws per frame (the number item 1.1 would
cut), `[hitch]` count (GC and streaming, item 1.5), and a contact sheet for textures and lighting. Each point gets one
cold run (no caches) as the acceptance run, and stays above 25 fps.

## 5. Suggested order

1. Read-only checks, no console time: the `n` argument of the `sub_822FE760` call sites (1.2), and the per-frame
   count of ZPD-wrapped draws on the Mac build (1.1 step A).
2. Coalesced-only A/B runs, one console session: `[XeD3D]` ring size (1.3), `MinDesiredFrameRate` 100 and 1 (1.4),
   `PoolSize` 140 (1.6), `TimeBetweenHWUpdates` 30 (1.7), `TimeBetweenPurgingPendingKillObjects` 120 (1.5). Each needs
   its SHA-1 in `masseffect_coalesced_sha1`.
3. Code: occlusion query proxy skip (1.1 A), then the one-frame lag hook (1.2), then real queries (1.1 C).
4. The location sweep (section 4) with the best config, then cold acceptance runs.
