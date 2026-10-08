# Engine settings A/B without editing Coalesced.ini

Date: 2026-10-07. Covers items 3, 4, 5, 6, 7, 9, 10 and 12 of [external-practices-2.md](external-practices-2.md)
(sections 1.3-1.7, 1.9, 1.11). Everything here is off by default. Console results go to
`docs/optimization-backlog.md` (mass-effect-recomp) and the status table of [best-config.md](best-config.md).

## Part A: `masseffect_ini_override` (any value of the game config)

```toml
masseffect_ini_override = "XeD3D:RBSecondarySize=8388608|XeD3D:RBSegmentCount=128"
```

`Section:Key=Value` items separated by `|`. Section and key compare case-insensitively, as the engine does. Only keys
that exist in `Coalesced.ini` can be changed (a missing key is logged once as `[ini] ... is not in the game config` and
nothing is added). The log shows every change: `[ini] [XeD3D] RBSecondarySize: "3145728" -> "8388608"`.

### How it works

File: `app/src/native/me_ini_override.cpp` (RU: `editions/ru/overlay/app/src/native/me_ini_override.cpp`).

The game's config cache is UE3's `FConfigCacheIni` (`GConfig`: EN `0x82EA2030`, RU `0x82EA2050`; vtable EN
`0x82108A58`). All config reads pass through one function, `TMap<FString,FConfigSection>::Find` (EN `sub_826DEAD8`,
RU `sub_826DF588`):

| Reader | EN | RU | Path |
|---|---|---|---|
| `GetString(FString&)` (vtable slot 6) | `sub_8238F240` | `sub_8238F940` | section Find, then key Find |
| `GetInt` / `GetFloat` / `GetDouble` (slots 4, 3, 2) | `sub_8238FB18` ... | | call slot 6 |
| `GetSectionPrivate` (slot 8) | `sub_8238F408` | `sub_8238FB08` | section Find |
| `UObject::LoadConfig` (script `config` properties) | `sub_8230E210` | `sub_8230E188` | slot 8, then key Find (`"%s[%i]"` for arrays) |

The hook calls the original section Find. When the returned section belongs to a file held by `GConfig` (the map
pointer must be `Data + 48*i + 16` of the `TMap<FString,FConfigFile>` at `GConfig+4`) and its name matches an override,
it looks each overridden key up with the section's own `TMultiMap<FString,FString>::Find` (EN `sub_826269A0`, RU
`sub_826275C8`) and replaces the value `FString` in place: the game's `FString(const TCHAR*)` (EN `sub_82210490`, RU
`sub_82210488`) builds the new string with the game allocator, the two `FString`s are swapped, and `~FString` (EN
`sub_822105C8`, RU `sub_82210510`) frees the old text. From then on every reader sees the new text, exactly as if the
ini had it. It is re-applied on every lookup of that section (a string compare), so a value the parser writes again
later is overridden again. A host mutex serialises the replacement (config reads come from several guest threads).
With the cvar empty the hook only calls the original.

Which reader uses which key (all verified in the EN image, RU functions are instruction-identical):

| Key | Read by | When |
|---|---|---|
| `[XeD3D] RBSecondarySize`, `RBSegmentCount` | `sub_826E5E78` (RU `sub_826E6670`), `GetInt`; must be a power of two; reallocates the ring through `sub_8222CAC8` if it changed | D3D init |
| `[TextureStreaming] PoolSize` (and the other 10 keys) | `sub_82300880`, `GetInt`/`GetFloat` | streaming init |
| `[Core.System] MaxObjectsNotConsideredByGC` | `sub_823105B8`, `GetInt` | startup |
| `[Engine.Physics] NxTimeStep` | `sub_823206A8`, `GetFloat` | PhysX init |
| `[Engine.Engine] TimeBetweenPurgingPendingKillObjects` | `LoadConfig` of `Engine.Engine` | engine init |
| `[ISACTAudio.ISACTAudioDevice] TimeBetweenHWUpdates`, `MaxChannels` | `LoadConfig` (the native code only builds the `FName`) | audio init |
| `[Engine.Client]`, `[XeDrv.XenonClient] MinDesiredFrameRate` | `LoadConfig` | client init |

Why not a modified `Coalesced.ini`: it still works (tools/coalesced.py `set`, `masseffect_coalesced_sha1`, upload to
`game_root/Layer0/MEInit/` on the console and restore the original afterwards), but every variant needs its own file and
SHA-1, and a forgotten file stays on the console. The cvar needs none of that.

### Variants (tools/me1_ini_ab.sh in mass-effect-recomp)

| Name | toml | Expectation | Image change |
|---|---|---|---|
| `base` | none | reference | |
| `ring8` | `XeD3D:RBSecondarySize=8388608\|XeD3D:RBSegmentCount=128` | fewer render-thread waits for free ring segments | none |
| `ring16` | `16777216`, `256` | same, more | none |
| `gc120` | `Engine.Engine:TimeBetweenPurgingPendingKillObjects=120` | fewer periodic GC hitches | none (memory between collections) |
| `pool140` | `TextureStreaming:PoolSize=140` | less mip churn | sharper (guest memory: watch `guest N/M MB`) |
| `audio30` | `ISACTAudio.ISACTAudioDevice:TimeBetweenHWUpdates=30` | less audio-thread work | audio parameters update every 30 ms |
| `audio30ch32` | plus `MaxChannels=32` | fewer voices mixed | quiet sounds dropped in busy scenes |
| `phys30` | `Engine.Physics:NxTimeStep=0.0333` | one PhysX step per frame instead of 1.67 | physics behaviour (Mako, ragdolls) |
| `detail100` / `detail1` | `Engine.Client:` and `XeDrv.XenonClient:MinDesiredFrameRate=100` / `=1` | always / never drop detail | yes |
| `gcroot95` (not in the default list) | `Core.System:MaxObjectsNotConsideredByGC=95000` | cheaper mark phase | risky: objects created after the initial load with an index below the value are never collected; only useful after the log tells the real root-set count |

## Part B: skeletal meshes and light environments

### What the 2007 engine already does

Found from the class registration (`StaticClass` calls of the `UClass` constructor `sub_82308A20` with the class name)
and the vtables:

| | EN | RU |
|---|---|---|
| `USkeletalMeshComponent` (1152 bytes), vtable | `0x8213E748` | |
| `USkeletalMeshComponent::Tick` (slot 77) | `sub_824C1058` | `sub_824C1F48` |
| TickAnimNodes / TickSkelControls / UpdateSkelPose | `sub_824C0E68` / `sub_824C3110` / `sub_824C5B50` | `sub_824C1D58` / `sub_824C4000` / `sub_824C6A40` |
| `UDynamicLightEnvironmentComponent` (224 bytes), vtable | `0x821564E8` | |
| `UDynamicLightEnvironmentComponent::Tick` (slot 77) | `sub_82558D30` | `sub_825599C8` |
| `FDynamicLightEnvironmentState::Tick` / dynamic update / proxy update | `sub_825579C8` / `sub_82557188` / `sub_825583A8` | `sub_82558660` / `sub_82557E20` / `sub_82559040` |

- **Skeletal Tick.** Ticks the anim tree and the skel controls every frame (this version has no
  `bTickAnimNodesWhenNotRendered`), then updates the pose unless `bUpdateSkelWhenNotRendered` (bit `0x80000000` of
  `+868`) is FALSE and `LastRenderTime` (`+608`) is older than `WorldInfo.TimeSeconds - 1.0`. So UE3's "skip the pose
  when not rendered" exists; it is a per-component flag.
- **ME1 data** (Engine.u / BIOC_Base.u / map packages, decompressed locally, read only): the class default is
  `bUpdateSkelWhenNotRendered=True`; `Default__BioPawn` does not change it; the Normandy packages set **False** on about
  two thirds of the placed BioPawn mesh components (for example 31 of 47 in `BIOA_NOR10_03_DS1`, 22 of 40 in
  `BIOA_NOR10_02_DS1`); the rest (heads, hair and attachments that follow the body, pawns spawned from archetypes) use
  True. None of these properties is `config` (flags: plain / editable), so the ini mechanism cannot set them.
- **DLE.** The engine already throttles: when the owner was not rendered for 1 s the environment updates only every
  `InvisibleUpdateTime` (times 0.8-1.2); full (static, line-check) updates happen every `MinTimeBetweenFullUpdates`
  (times 0.8-1.2); a visible environment runs the dynamic update and the proxy update every frame. Class defaults:
  `InvisibleUpdateTime=2.0`, `MinTimeBetweenFullUpdates=0.15`, `LightVisibilityRefreshTime=0.2`. BioPawn's template
  (`Default__BioPawn.LightEnvComponent0`): 10.0 / 1.0 / 2.0. Doors and inert props: `MinTimeBetweenFullUpdates=10`, and
  most door environments in the Normandy maps have `bEnabled=False`.
- Profile (RU 960x544, `prof960`, game thread self time): TickAnimNodes 2.1 %, skeletal Tick 1.2 %, slot 115 of the
  skeletal vtable (`sub_824C29F0` RU) 1.2 %. The DLE functions do not show in the flat top list.

### Property overrides (preferred): the game's `SET` command

`SET <class> <property> <value>` (UE3 `UObject::StaticExec`, the strings `Unrecognized class %s` / `Unrecognized
property %s` are in the image) changes the class default object and every loaded object of the class, archetypes
included; objects loaded later start from the changed defaults. It runs through `masseffect_exec`
([location-tests.md](location-tests.md)):

| Name | toml |
|---|---|
| `skelset` | `masseffect_exec = "set SkeletalMeshComponent bUpdateSkelWhenNotRendered False"`, `masseffect_exec_world = 0`, `masseffect_exec_frames = 60` |
| `dleset` | `masseffect_exec = "set DynamicLightEnvironmentComponent MinTimeBetweenFullUpdates 3\|set DynamicLightEnvironmentComponent InvisibleUpdateTime 20"`, same world/frames |

Check the log for `[exec] commands: "set ..." -> handled`. If `SET` is compiled out of this build the line says
`not handled`; then use the hooks below. A value serialized in a map package (the explicit False above) is not
touched by a later load, so the property route can only make more components skip.

### Code hooks (fallback), `app/src/native/me_crowd_cpu.cpp`

- `masseffect_skel_skip_unrendered = true`: around the original skeletal Tick, clears `bUpdateSkelWhenNotRendered` on
  every component not owned by the local player's pawn and restores the bit afterwards. The engine's own rule then
  skips the pose of meshes not rendered for 1 s. Log every 10 s: `[crowd] 10 s: N skeletal ticks, M with
  bUpdateSkelWhenNotRendered cleared, K of them not rendered (pose update skipped)`.
- `masseffect_dle_visible_every = N` (2-8): each light environment runs its tick only every N-th call (components
  spread over the frames, delta time times N), immediately when `bForceFullUpdate` is set. Log: `[crowd] 10 s: light
  environment ticks R run, S skipped`.

### Risks

- **Pose not updated off screen.** Bones (`SpaceBases`) of a pawn not rendered for 1 s are stale. Code that reads
  them: socket locations for weapon muzzles and effects (`GetSocketWorldLocationAndRotation`), AI aiming at a bone,
  attachments, physics bodies driven from animation (`bUpdateKinematicBonesFromAnimation`), ragdoll start. A squad
  member shooting behind the camera may fire from an old muzzle position; an enemy that dies off screen starts its
  ragdoll from an old pose. The designers already set False for most ambient NPCs, so the remaining risk is mostly the
  squad and combat NPCs. The player's pawn is excluded in the hook. Anim nodes keep ticking, so notifies, sounds and
  `FinishAnim` keep working (that is why the hook does not skip TickAnimNodes, which would freeze latent script
  functions).
- **DLE throttle.** Character lighting reacts N-1 frames late to light changes and movement between lights (N=2:
  66 ms at 30 fps), and full updates every 3 s in `dleset` can make a lighting change visible as a step when a
  character walks from a dark into a lit area. No gameplay effect.
- **Config values.** Bigger ring and texture pool cost guest physical memory (512 MB); `phys30` changes the
  simulation; `audio30ch32` can drop sounds.

## Running it

```sh
cd mass-effect-recomp
zsh tools/me1_ini_ab.sh                      # default list: base ring8 ring16 gc120 pool140 audio30 audio30ch32 phys30 skelhook dle2 skelset dleset
zsh tools/me1_ini_ab.sh base ring8 base ring8   # any subset / order; ROUNDS=2 repeats the whole list
zsh tools/me1_ini_ab.sh list                 # the variants
```

The script uploads `run/me1/ru_ini.nro` once (SHA verified; `NRO=skip` keeps the console's), writes
`run/me1/ini_<name>.toml` from `BASE` (default `run/me1/ringpart_ab.toml`), runs `LONG=1 tools/me1_anderson.sh` for each
and appends to `run/me1/ini_ab_results.txt`: Swaps of every 10 s window, the heavy windows (draws/frame > 400, the
Normandy walk), hitch count and worst hitch, and the `[ini]` / `[crowd]` / `[exec]` lines that prove the variant was
active. One run takes about 6-7 minutes.
