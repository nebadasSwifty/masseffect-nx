# Audio and video on the Switch

How the game's sound and movies reach the Switch. The sound is the part the SDK does for the game; the movies are
not decoded by the port at all. Words you do not know are in the [glossary](glossary.md).

## In short

- The game mixes its sound itself, with its own (recompiled) mixer, into 5.1 frames of 256 samples at 48 kHz. The SDK
  takes those frames, folds them to stereo and plays them through the Switch's audio output (`audout`).
- Compressed sound in the game is [XMA](glossary.md#xma), the Xbox 360's audio format. The SDK decodes it with FFmpeg
  (its XMA frames decoder), on a worker thread that runs above the game's own threads, with NEON where it matters.
- Two optional native versions of the mixer's hottest kernels exist and are bit-exact against the recompiled originals,
  but they are off by default: they did not show a gain in the best profile.
- The movies of Mass Effect are Bink files (`Movies/*.bik`), played by the game's own recompiled code. The SDK has no
  video decoder in the path of this game.

## Output

`sdk/src/audio/switch/switch_audio_system.cpp` and `sdk/src/audio/downmix.cpp`.

- **Format.** The game's mixer hands the SDK one frame at a time: six float channels (5.1), 256 samples, 48 kHz, so one
  frame lasts 5.333 ms. The Switch output is stereo 16-bit: `conversion::sequential_6_BE_to_interleaved_2_LE` folds the
  six channels to two (the fold weights and the master gain are set through `downmix.cpp`).
- **The device.** `audout` takes buffers of four frames (1024 samples, 21.3 ms) and three buffers are kept queued, which
  caps the latency at about 64 ms. The sample memory has to be page aligned. The output thread runs at priority `0x2B`,
  above the game's threads (see [platform-notes.md](platform-notes.md), "Threads").
- **The pump.** The simple design, requesting four frames from the game each time the device releases a buffer, left the
  game's audio server thread without a frame to fill (the sound came out "robotic"). With `audio_switch_pump` (on by
  default) the SDK asks for one frame every 5.333 ms against absolute deadlines (a 187.5 Hz pump) and tops up when the
  queue is short, because the system clock and the audio clock do not match exactly. `audio_switch_frames_in_queue`
  (default 10, about 53 ms) is the cushion of game frames kept ahead of the device: more cushion lets the game stall
  briefly without cutting the sound, at the price of latency. Both are read when the audio opens.
- **Mute.** `audio_mute = true` mutes the output.

## XMA decoding

`sdk/src/audio/xma_decoder.cpp`, `xma_context.cpp`.

- **What decodes it.** Games use XMA through the XMA kernel calls or through XAudio and direct writes to the XMA contexts
  (64-byte structures in memory the game maps). The SDK keeps those contexts and decodes each with FFmpeg's XMA frames
  decoder (libavcodec; `ff_xmaframes_decoder` in `sdk/thirdparty/ffmpeg-overlay/codec_list.c`). The decoded float samples
  are converted to the big-endian 16-bit the game expects.
- **Off the game's thread.** `audio_xma_in_worker` (on by default on the Switch) makes the XMA decoder thread do the
  decoding when a kick arrives, instead of the thread that requested it. On the Switch the game's audio server thread
  runs below the GPU threads, and they preempted it in the middle of a decode. The worker runs at `0x2B`,
  above them. The figures the SDK records for this (17.3 % of a core and peaks of 14.4 ms, then the game thread's wait
  falling from 167-302 ms to 10.7-16.1 ms per 10 s on a PC test, no differences from the recompiled behaviour) were
  measured on the title this layer was first written for; they are **inherited**, not remeasured for Mass Effect.
- **NEON.** The conversion of a decoded frame to big-endian 16-bit integers uses NEON on AArch64, bit-identical to the
  scalar loop (clamp with NaN becoming the lower bound, multiply by 32767, truncate): about 0.3 ms per frame. FFmpeg's own
  NEON paths are on by default; `audio_ffmpeg_simd = false` is a diagnostic that makes it use C only, to compare audio.
- **What it costs.** One FFmpeg decode is about 383 microseconds per context, at about 165 contexts per second, so XMA
  is roughly 6 % of a core. Removing the zero-fill of the contexts would save about a microsecond per frame and was
  rejected ([optimization-paths.md](optimization-paths.md)).
- **Other code paths worth knowing.** Waits of the audio threads park on a wake point instead of polling with 1 ms
  sleeps (`thread_wait_blocking`, on), which removed about a thousand wake-ups per second from the audio worker, and the
  1 microsecond sleep in a volume-mask routine the mixer calls every frame is removed (`audio_volumemask_sleep_us`, 0),
  because a Horizon sleep can give the core away for a whole time slice.

## Native versions of the mixer

`app/src/native/me_audio_hooks.cpp` and `me_audio_dsp.h`. The XAudio mixer is part of the game's own code, so it is
recompiled like everything else. Two of its hottest kernels have NEON replacements (the ramped mix `sub_82AA53C0` and a
smoother `sub_82B4D580`), selected by `masseffect_audio_dsp_native`: 0 = the recompiled code (the default), 1 = native,
2 = validate (run both on a copy, compare byte by byte, keep the recompiled result, count and log the mismatches).
`masseffect_audio_dsp_mask` picks the functions. In validate mode 23,411 calls gave no mismatch, and the host test in
`tests/audio_dsp/` checks bit-exactness against the recompiled originals. The expected saving was 3 to 4 core-ms per
frame but the combined host-overhead runs showed about 3 core-ms in all, within noise of the main thread, so they stay
off. A native resampler and the reverb-like routine are listed as open ideas.

## Movies

- **Format.** Mass Effect's movies are Bink video (`Movies/*.bik`: logos, loading screens, attract movies). They are copied
  with the rest of the disc into `game_root/`. The game plays them with its own player, which is part of the recompiled
  program, so they need nothing from the SDK. The logo movies play (about 30 fps in the first, emulated-GPU builds). How
  the movies' sound reaches the output was not investigated.
- **The FFmpeg build has a WMV3 decoder that this game does not use.** The SDK's FFmpeg configuration
  (`config_masseffect_wmv3.h`) was extended with the WMV3 (VC-1) decoder, for games whose cutscenes are WMV. No code of the
  SDK calls a video decoder for Mass Effect, so it is dead weight kept for other games.
- **Resolution.** The movies are not scaled by the internal resolution change (the "Bink movies" note in
  `app/src/native/me_resolution.cpp`).
- **The startup logos.** They cost start-up time. A modified `Coalesced.ini` without them can be made with
  `tools/coalesced.py no-startup-movies`; the game verifies that file against a SHA-1 stored in the executable, so the new
  hash has to be given in `masseffect_coalesced_sha1`. It is a testing aid, and the original file is what the installer
  copies. Skipping the loading movie `GLO_Relay_LOAD.bik` is a listed idea worth about 0.3 s, not done.
