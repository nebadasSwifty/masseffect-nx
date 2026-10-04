# Renderer configuration console check

The v0.1.0 NRO linked an existing Mesa SDK that had stage-cache and fragment-barrier
changes but lacked the project's `nvk_switch_draw` contract. Its log explicitly
reported that the per-draw optimisations were unavailable. The corrected driver
was rebuilt from the pinned source with `mesa/mesa-switch-masseffect.patch`;
the console NRO's SHA-256 matched the locally built v0.1.1 NRO.

The build now checks the actual archive's defined `nvk_switch_draw` symbol before
configuring the application. The older archive fails that check; the rebuilt one
passes and boots on the console, plays startup logos and resumes the Eden Prime save.

The settings change independently of the renderer code:

- `masseffect_native_texture_interval_max = 4`: shorten stale texture content after
  the game reuses a guest-memory address. The old maximum was 32 frames plus jitter.
- `log_async = false`: retain the SDK default to avoid a starved async logger's
  blocking queue. The source documents this failure mode on Horizon.

For the stationary Eden Prime scene, the last six ten-second profiler intervals
were:

| Driver / settings | Game FPS |
| --- | --- |
| Existing driver, four-frame recheck, async logging | 27.3, 30.2, 26.9, 26.1, 26.3, 26.2 |
| Existing driver, four-frame recheck, synchronous logging | 29.9, 27.0, 26.1, 26.2, 26.1, 26.2 |
| Corrected driver, four-frame recheck, synchronous logging | 26.1, 26.2, 26.2, 26.0, 26.0, 26.1 |

These runs used CPU 1785 MHz, GPU 768 MHz and memory 1600 MHz, as recorded by the
profiler; they are not stock-clock performance claims. Initial intervals include
loading. The corrected driver logged its per-draw optimisations as active with
zero guard disagreements. The original unmodified Coalesced.ini remained on SD.

No stationary FPS improvement is established. Consecutive screenshots and a short
camera pan do not establish temporal correctness at the game's frame rate. Texture
flicker and movement-related drops remain open pending a repeatable route test and
the player's observation; the changes do not claim to fix every renderer issue.

## Coordination limitation

After these runs, the user disclosed that a second chat could also operate the
console. A shared exclusive lease was agreed at WORK/.switch-console.lock
(atomic mkdir, owner.txt containing the thread ID and scenario). Earlier runs
preceded that protocol and must not be treated as controlled A/B acceptance or
proof that flicker or frame drops are fixed. v0.1.1 was returned to draft and
Pages restored to v0.1.0 pending an isolated load-and-camera-turn verification.

The first leased rerun reached Press START, but sys-botbase A/PLUS input did
not advance the title screen; HOME input did work. No isolated mission-turn
result is claimed from that attempt. The downloaded Album clip shows motion
smearing and a falling overlay FPS reading, but no specific disappearing
surface has yet been identified conclusively.
