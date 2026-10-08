# Engine settings that cut draws per frame (or per-draw CPU)

Date: 2026-10-07. Offline research only, nothing was built or run on the console. Goal: list every UE3/ME1 setting
that lowers the number of draws the game issues per frame, or the CPU cost per draw, with as little visible change
as possible, and prepare the top six as console A/B variants. Image-changing variants are measured and offered as
choices only; the user decides ([best-config.md](best-config.md), "Postponed").

Related: [engine-settings-ab.md](engine-settings-ab.md) (the `masseffect_ini_override` and `SET` mechanisms, the
`me1_ini_ab.sh` runner), [external-practices-2.md](external-practices-2.md) section 0 (which ini keys are dead),
[location-tests.md](location-tests.md) (`masseffect_exec`, `AT`), [ring-cpu-per-draw.md](ring-cpu-per-draw.md).

## 0. How this was checked, and the two ways to change a setting

Sources (read only, nothing copied out of the game data):

- the decoded console `Coalesced.ini` (`Xe-BIOEngine.ini`, `Xe-BIOGame.ini`);
- the script packages `Engine`, `BIOC_Base`, `GameFramework`, `BIOG_*`, `BIOC_WorldResources` from
  `game_root/Layer0/MEInit`, LZO chunks decompressed locally (the `Lzo()` routine of `tools/ue3_shader_scan.cpp`),
  then the export table read: owner class, type and `PropertyFlags` of every property, and the tagged default
  properties of the class defaults and component templates;
- the map packages of Normandy (`BIOA_NOR10_0*`), Eden Prime (`BIOA_PRO10_*`) and the Presidium (`BIOA_STA20_*`),
  194 packages: per-component property tags (what the level designers changed from the archetype).

Two mechanisms can change a value at run time:

| Mechanism | Reaches | Limits |
|---|---|---|
| `masseffect_ini_override = "Section:Key=Value\|..."` | script properties with the `config` flag (`0x4000`) and the native ini reads | the key must already be in `Coalesced.ini`; values are read when the class loads (boot), so the cvar must be set at launch |
| `masseffect_exec = "set <Class> <Property> <Value>"` (UE3 `SET`) | **any** property of any loaded class: the class default, every loaded object of the class and its subclasses, and the component templates (archetypes) | unverified on the console (see 5.1); class-wide, no per-object filter; objects loaded later take the new value only through their archetype |

Almost every draw-related property is **not** `config` in this 2007 build: `PrimitiveComponent.CullDistance`,
`bUseAsOccluder`, `CastShadow`, `bCastDynamicShadow`, all `DynamicLightEnvironmentComponent` and `LightComponent`
fields, `SkeletalMeshComponent.ForcedLodModel` / `MinAutoLODLevel`, the particle LOD fields and the terrain
tessellation fields are plain (`0x1` editable or `0x0`). So `SET` is the only settings route for them; the ini
route only reaches the `[Engine.Engine]` shadow/decal keys, `MinDesiredFrameRate` and a few BioWare keys.

**Not in this build at all** (no name in the Engine/BIOC_Base name tables): `[SystemSettings]` and everything in it
(`DetailMode`, `SkeletalMeshLODBias`, `ParticleLODBias`, `ShadowFilterQuality`, `DynamicShadows`,
`LightEnvironmentShadows`, `bAllowLightShafts`), `CullDistanceVolume`, `MaxDrawDistance` / `LDMaxDrawDistance` /
`CachedMaxDrawDistance` (the 2007 name is `CullDistance`), `MinScreenRadiusForDepthPrepass`,
`InstancedStaticMesh`, `StaticMeshCollectionActor`, light shafts. Adding them to the ini does nothing.

## 1. Every candidate

"Draws" = draws the game issues to D3D (each costs the UE3 render thread plus the ring thread's PM4 parse and
translation, ~20-40 us on the ring at 1785 MHz). Estimates are marked; the map numbers are counts of placed objects
in all sublevels of an area, not all loaded or in view at once.

| # | Setting | Where it lives | Runtime change | Expected draw effect | Visual impact | 360 state |
|---|---|---|---|---|---|---|
| 1 | `PrimitiveComponent.bUseAsOccluder` | script property (edit), default True; `StaticMeshActor` template keeps True; designers set False on 308 of 1246 Normandy static meshes | `SET` | removes the guest depth prepass: every occluder's depth-only draw. The port already throws these away (`masseffect_native_skip_prepass`, default on), but only after the UE3 render thread built them and the ring parsed them. 35-45 % of ring draws in the heavy Normandy windows never reach Vulkan (prepass skips plus rejections), so the saving is up to roughly a third of the ring draws (estimate) | **none** while `skip_prepass` is on (the prepass is not rendered today) | prepass on |
| 2 | `DynamicLightEnvironmentComponent.bCastShadows` | script property, default True; BioPawn template keeps True; doors and inert props already False | `SET` | removes each character's composite modulated shadow: shadow-depth draws for every section of the pawn and its attachments (body, head, hair, weapon) plus the projection/stencil draws, and the shadow-map EDRAM traffic (E35/E39 shadow-slot clears). About 6-10 draws per lit pawn in view (estimate); Normandy CIC and Presidium have 10-20 pawns | characters lose their soft modulated shadow (most visible in sunlight, Eden Prime) | on |
| 3 | `PrimitiveComponent.CullDistance` on `StaticMeshComponent` | script property, default 0 (= never cull). Designers almost never set it: 0 of 6100 static meshes in the three areas, 2 particle systems on Normandy | `SET` | distance cull of every static mesh beyond N units (UE3 tests the distance from the view to the bounds origin). Outdoors (Eden Prime, Presidium vista, Virmire, Mako worlds): many of the instanced rocks/props that make up 65-70 % of draws (t240) are far away. Indoors (Normandy): almost none | far props pop out; large far meshes (cliffs, buildings, the Citadel arms) can vanish if their bounds origin is beyond N: needs a contact sheet per location | not used |
| 4 | `BioSunActor.HideAllFlaresOverride` (or `BioSunFlareComponent.HiddenGame`) | BioWare lens-flare actor: 1 occlusion sprite, 1 halo, 10 flare ghosts, 6 streaks per actor; 95 actors on Normandy, 103 on the Presidium, 24 on Eden Prime. The templates mark the flare sprites `HiddenGame`, so the native actor code decides what is drawn | `SET` | unknown until a capture shows whether flare sprites are drawn: up to ~11 sprite draws per sun actor in view if they are | lens-flare ghosts gone (light glows keep the core sprite) | on |
| 5 | `PrimitiveComponent.CullDistance` on `ParticleSystemComponent` | as #3; 36 systems on Normandy, 191 on Eden Prime | `SET` | each emitter is a draw; culls distant steam, sparks, fires | distant effects vanish | not used (2 systems) |
| 6 | `SkeletalMeshComponent.MinAutoLODLevel` (or `ForcedLodModel`) | script property (flags 0); 617 of 935 skeletal meshes in the three areas have 3 LODs | `SET` | no fewer draws (LOD1 keeps its sections), but fewer vertices to skin, fewer required bones (game thread `UpdateSkelPose`) and fewer bone constants uploaded per skinned draw (per-draw ring cost) | characters lower-poly up close (conversations, cutscenes) | full LOD |
| 7 | `MinDesiredFrameRate` (`bDropDetail`, `bAggressiveLOD`) | `[Engine.Client]`, `[XeDrv.XenonClient]` = 35 | ini override (variant `detail100`/`detail1` already in `me1_ini_ab.sh`) | no script reads `bDropDetail`/`bAggressiveLOD` (checked: no bytecode reference in the script packages), so only native readers; effect unknown | fewer effects / coarser LOD | **already active**: with 35 the game is in drop-detail below 35 fps and aggressive LOD below ~30 fps, i.e. most of the time at our 25-30 fps. `detail100` changes little; `detail1` is the costlier reference |
| 8 | `BioWorldInfo.m_fMaxVFXBudget` | `Xe-BIOGame.ini [BIOC_Base.BioWorldInfo] m_fMaxVFXBudget=100` (config); costs in `[BIOC_Base.BioVFXTemplate] m_afCostTable` 1-5 | ini override | caps concurrent BioWare VFX (weapon/power effects): fewer particle draws in combat only | fewer simultaneous combat effects | default |
| 9 | Corpse cleanup | `[BIOC_Base.BioPawn]` / `[BIOC_Base.BioArtPlaceable]` `m_fCorpseCleanupFirstAttemptTime=5.0`, `m_fMaxCorpseCleanupScreenSizeThreshold=0.10/0.20` (config) | ini override | dead bodies removed sooner: fewer skeletal draws after fights | bodies disappear sooner / while larger on screen | default |
| 10 | `PlayerController.LODDistanceFactor` | script property, `const`, default 1.0; read by the renderer's view and by `Actor.CheckMaxEffectDistance` | `SET` (may refuse a const property) | scales LOD distances and `CullDistance`: >1 picks lower LODs nearer; draws change only together with #3/#5 | coarser LOD everywhere | 1.0 |
| 11 | `Engine.bForceStaticTerrain` | `[Engine.Engine] bForceStaticTerrain=False` (config) | ini override | same draw count; skips the per-frame dynamic tessellation index rebuilds on the render thread (terrain areas only: 313 terrain components on Eden Prime, Mako worlds) at the price of more terrain triangles (unverified UE3 semantics) | terrain at fixed tessellation (no LOD popping) | off |
| 12 | `Terrain.MaxTesselationLevel` / `TesselationDistanceScale` | script properties, defaults 4 / 1.0 on all Eden Prime terrains | `SET` | triangles only, not draws | coarser terrain silhouettes | default |
| 13 | `bStaticDecalsEnabled` / `bDynamicDecalsEnabled` | `[Engine.Engine]` (config) | ini override | few decals placed (12 Normandy, 37 Presidium) | decals gone | **measured t236: no effect** |
| 14 | `ShadowFilterRadius`, `MaxShadowResolution`, `MinShadowResolution` | `[Engine.Engine]` (config) | ini override | GPU only, not draws | shadow edges | **measured t186/t208/S13: no gain**; `BranchingPCFQuality=0` is already the lowest |
| 15 | `MeshLODRange`, `bUsePostProcessEffects`, `bForceCPUSkinning` | `[Engine.RenderDevice]` in the ini (the properties are `Engine` config properties, the section is a PC leftover) | ini override cannot reach them (the keys are not in the section that is read) | | | **measured t175/t298: no effect** |
| 16 | `StaticMeshComponent.ForcedLodModel` | script property (flags 0) | `SET` | triangles only, and only for meshes with LODs | coarser props | |
| 17 | `ParticleSystem.LODDistances` / `LODMethod` | per asset: 156 of 160 systems have 2 LOD levels; `BioLockLowestLODToHighest` = True on 144 | `SET` | none expected: the lowest LOD is locked to the highest | | **already "low"** (no LOD to drop to) |
| 18 | DLE `eQuality` / `nNumMultiLights` | defaults `ELEQ_NonFlat` / 2; no map overrides | `SET` | shader cost; `nNumMultiLights` only matters for `ELEQ_MultiLight` (unused) | lighting of characters | already the cheap mode |
| 19 | DLE throttles (`MinTimeBetweenFullUpdates`, `InvisibleUpdateTime`), `bUpdateSkelWhenNotRendered` | | `SET` / hooks | game-thread CPU, not draws | | covered by `dleset`, `skelset` in engine-settings-ab.md |
| 20 | `LightComponent.CastDynamicShadows` / `bCastCompositeShadow` | defaults True; Normandy: 339 point lights feed composite (character) shadows; Eden Prime: all 19 directional lights `CastDynamicShadows=False` | `SET` | same draws as #2 (the composite shadow is the only dynamic shadow characters cast) | as #2 | world dynamic shadows already off |
| 21 | `HeightFogComponent` | 1 on the Presidium | `SET` `bEnabled False` | 1 full-screen draw | no fog | |
| 22 | Occlusion queries | renderer, not a setting | `masseffect_native_query_mode` | | | **measured: rejected** (2-4 % culled, slower) |

## 2. Ranking and the six A/B variants

Ranked by expected draws (or per-draw CPU) saved per unit of visible change, weighted to the reference scenes
(Normandy walk 700-850 draws/frame, the Presidium, Eden Prime):

| Rank | Variant | Image | Where to measure |
|---|---|---|---|
| 1 | `occl` (#1) | none expected | Normandy route (`me1_ini_ab.sh`), then any location |
| 2 | `charshadow` (#2) | characters without shadows | Normandy route (crew), Presidium |
| 3 | `smcull15k` (#3) | far props pop out beyond 150 m | Eden Prime, Presidium, a Mako world (`me1_location.sh`) |
| 4 | `flares` (#4) | lens-flare ghosts gone | Normandy route, Presidium |
| 5 | `pscull` (#5) | far particle effects vanish beyond 50 m | Eden Prime |
| 6 | `skellod1` (#6) | lower-poly characters up close | Normandy route, Presidium |

### 2.1 Ready lines

Each block is one variant, appended to the base toml (the runner drops earlier `masseffect_exec*` keys).
`masseffect_exec_world = 0` / `frames = 60` follows the existing `skelset`/`dleset` variants.

```toml
# occl: no guest depth prepass (the port already skips its output)
masseffect_exec = "set PrimitiveComponent bUseAsOccluder False"
masseffect_exec_world = 0
masseffect_exec_frames = 60
```

```toml
# charshadow: no composite (character) shadows
masseffect_exec = "set DynamicLightEnvironmentComponent bCastShadows False"
masseffect_exec_world = 0
masseffect_exec_frames = 60
```

```toml
# smcull15k: static meshes culled beyond 15000 units (~150 m); also try 8000 in a second run
masseffect_exec = "set StaticMeshComponent CullDistance 15000"
masseffect_exec_world = 0
masseffect_exec_frames = 60
```

```toml
# flares: BioWare lens-flare ghosts off (second command covers the case that the override is not read)
masseffect_exec = "set BioSunActor HideAllFlaresOverride True|set BioSunFlareComponent HiddenGame True"
masseffect_exec_world = 0
masseffect_exec_frames = 60
```

```toml
# pscull: particle systems culled beyond 5000 units (~50 m)
masseffect_exec = "set ParticleSystemComponent CullDistance 5000"
masseffect_exec_world = 0
masseffect_exec_frames = 60
```

```toml
# skellod1: characters never use LOD0
masseffect_exec = "set SkeletalMeshComponent MinAutoLODLevel 1"
masseffect_exec_world = 0
masseffect_exec_frames = 60
```

The same as `V[...]` lines for `mass-effect-recomp/tools/me1_ini_ab.sh` (not added there; paste when running):

```zsh
V[occl]=$'masseffect_exec = "set PrimitiveComponent bUseAsOccluder False"\nmasseffect_exec_world = 0\nmasseffect_exec_frames = 60'
V[charshadow]=$'masseffect_exec = "set DynamicLightEnvironmentComponent bCastShadows False"\nmasseffect_exec_world = 0\nmasseffect_exec_frames = 60'
V[smcull15k]=$'masseffect_exec = "set StaticMeshComponent CullDistance 15000"\nmasseffect_exec_world = 0\nmasseffect_exec_frames = 60'
V[flares]=$'masseffect_exec = "set BioSunActor HideAllFlaresOverride True|set BioSunFlareComponent HiddenGame True"\nmasseffect_exec_world = 0\nmasseffect_exec_frames = 60'
V[pscull]=$'masseffect_exec = "set ParticleSystemComponent CullDistance 5000"\nmasseffect_exec_world = 0\nmasseffect_exec_frames = 60'
V[skellod1]=$'masseffect_exec = "set SkeletalMeshComponent MinAutoLODLevel 1"\nmasseffect_exec_world = 0\nmasseffect_exec_frames = 60'
```

For location runs (`me1_location.sh` owns `masseffect_exec` for `AT`), pass the command through `ARRIVAL`, e.g.
`ARRIVAL="God|set StaticMeshComponent CullDistance 15000" zsh tools/me1_location.sh eden <base>.toml`. That runs
after the map is loaded, so it relies on `SET` re-attaching the components (5.1).

Combat-only ini knobs (#8, #9), for a combat location, not for the Normandy route:

```toml
masseffect_ini_override = "BIOC_Base.BioWorldInfo:m_fMaxVFXBudget=40|BIOC_Base.BioPawn:m_fCorpseCleanupFirstAttemptTime=2.0|BIOC_Base.BioPawn:m_fMaxCorpseCleanupScreenSizeThreshold=0.30"
```

### 2.2 What to read in the results

- Proof the variant ran: `[exec] commands: "set ..." -> handled`, and no `Unrecognized class` /
  `Unrecognized property` line from the game's own log output right after it.
- Effect: the `10.0 s: N Swaps, M draws` windows (draws per frame), the ring partition and the render-thread core-ms.
  `occl` should lower draws per frame on the Normandy route with an identical contact sheet; if the draw count does
  not move, `SET` did not re-attach the components (5.1).
- Image: contact sheets against `base`; for `smcull15k` and `pscull` compare long views per location.

## 3. What the Xbox 360 configuration already has at the low end (nothing to gain)

- No `[SystemSettings]` tier exists; detail is fixed by content and the few `[Engine.Engine]` keys.
- Shadows: `BranchingPCFQuality=0`, `bEnableVSMShadows=False`, `AllowShadowVolumes=False`; world static meshes do not
  cast dynamic shadows (`StaticMeshActor` template `bCastDynamicShadow=False`); Eden Prime's directional lights have
  `CastDynamicShadows=False`; doors and inert props have DLE shadows off. The only dynamic shadows left are the
  character composite shadows (#2). Shadow resolution and filter keys were measured without gain.
- Drop detail: `MinDesiredFrameRate=35` already keeps `bDropDetail` on at our frame rates (#7).
- Particles: `BioLockLowestLODToHighest` on 90 % of systems: no lower particle LOD to switch to (#17).
- Terrain: no foliage or decoration layers on the Eden Prime terrains (`FoliageMeshes` empty, `DecoLayers` unset),
  default tessellation; grass and rocks are ordinary static meshes (that is what #3 targets).
- Textures: `TEXTUREGROUP_World/WorldNormalMap LODBias=2` already drops two mips (not a draw knob anyway).
- Decals: few placed, measured without effect (t236).
- Light environments: 551 of ~1290 placed DLEs are disabled, the BioPawn ones throttled (10 s invisible, 1 s full).
- Skeletal updates: about two thirds of placed BioPawn meshes already skip pose updates when not rendered.

What the 360 does **not** use, so there is room: distance culling (`CullDistance` is unset on all ~6100 static
meshes in the three areas), and the depth prepass is fully on (#1) although the port no longer renders it.

## 4. Beyond settings (if the variants confirm the direction)

- **Size-aware cull distance** (a code hook, not a setting): UE3's later `CullDistanceVolume` sets `CullDistance` by
  bounds size (small props near, large meshes never). A hook at component attach (`UPrimitiveComponent::Attach` /
  scene-info creation) could do the same for `StaticMeshComponent` with a radius table, avoiding #3's risk of
  vanishing large meshes.
- **Drop the prepass at the source** in the guest (patch the prepass call in the scene renderer) if `SET` cannot
  reach the static draw lists: same effect as #1 without relying on `SET`.

## 5. Open points

### 5.1 `SET` on the shipped build (unverified)

- The strings `Unrecognized class %s` / `Unrecognized property %s` are in the image, so `SET` is compiled in.
- In UE3, `SET` imports the value into the class default object and every object of the class, calling
  `PreEditChange` / `PostEditChange` per object; for components `PostEditChange` re-attaches them, which rebuilds
  the scene proxy and the static draw lists (`bUseAsOccluder`, `CullDistance`, `HiddenGame` and the DLE shadow are
  all copied into the render-side primitive info at attach). If this build skips the notification, values only take
  effect for components attached after the command: run it before the map loads (`masseffect_exec_world = 1`) so the
  map's components are created from the changed templates.
- Map components store only the properties that differ from their archetype (`Default__StaticMeshActor.
  StaticMeshComponent0`, `Default__BioPawn.LightEnvComponent0`, ...). An explicit value in the map (for example the
  308 Normandy meshes with `bUseAsOccluder=False`) is kept; the change reaches everything else through the archetype.
- `LODDistanceFactor` is `const`; `SET` may refuse it.

### 5.2 Unknowns per variant

- `occl`: the share of prepass draws in the guest's draw stream (the 35-45 % "never reach Vulkan" includes other
  rejections). Check also that `skip_prepass` stays on; with it off, `occl` changes the GPU work (no early depth).
- `flares`: whether the native `BioSunActor` draws its flare sprites at all in the shipped data (templates are
  `HiddenGame`); a GPU mark dump of a Normandy frame answers it.
- `smcull15k`: whether the 2007 distance test includes the bounds radius; check large meshes on the contact sheet.
- `MinDesiredFrameRate`: which native code reads `bDropDetail` / `bAggressiveLOD` (an xref to the two
  `WorldInfo` field offsets in the recompiled code).
