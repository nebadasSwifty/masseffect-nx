# Touch screen input (experimental)

Code: `sdk/src/input/switch/switch_input_driver.cpp` (`ApplyTouch`, `StartDialogueTap`, cvars `input_touch*`).
Settings: `app/masseffect.toml`, section "Input and audio" (all touch keys are commented out = code defaults).

## Where it runs

- The touch panel is read with libnx (`hidInitializeTouchScreen` once in `SwitchInputDriver::Setup`,
  `hidGetTouchScreenStates(&state, 1)` in `Poll`) on the guest thread that asks for pad 1, under the
  driver mutex. There is no extra thread.
- The result is merged into the XInput state of pad 1 only (the handheld / first controller). The game
  sees ordinary stick and button input; nothing in the game is patched.
- The panel only reports touches in handheld mode. Docked, `count` is always 0 and the code does nothing.
- `Poll` runs several times per game frame, so all timing uses the system tick, not call counts.
- At startup one log line starts with `[touch]` and lists every setting.

## Gestures

Only one finger is used. A second finger (or the first one lifting while another stays) cancels the
gesture until all fingers are lifted.

Where the finger first touches decides the gesture:

| Touch starts | `input_touch_dialogue` | `input_touch_camera` | Gesture |
|---|---|---|---|
| inside the dialogue band | true | any | dialogue tap candidate |
| outside the band (or dialogue off) | any | true | camera drag |
| otherwise | | | ignored |

A touch that starts in the band and moves more than `input_touch_tap_max_move` (70 px) from its start
point stops being a tap candidate and becomes a camera drag from that moment (speed starts at 0, so the
camera does not jump), or is ignored if `input_touch_camera` is false. So the camera can be dragged from
anywhere on the screen.

A tap outside the band does nothing: there is no generic "tap = A", to avoid accidental actions.

### Camera drag -> right stick

Each poll at least 8 ms after the previous measurement:

```
inst_v   = (finger_pos - last_pos) / dt                       # pixels per second, x and y
alpha    = 1 - exp(-dt / input_touch_camera_smoothing_ms)     # 1 if smoothing is 0
vel     += (inst_v - vel) * alpha                             # exponential smoothing
```

Gaps over 200 ms between polls reset the speed to 0 (no jump after a hitch). Then:

```
speed = |vel|
if speed <= deadzone:           stick = 0
else:
  full  = 1000 / sensitivity                                  # px/s for full deflection
  norm  = clamp((speed - deadzone) / (full - deadzone), 0, 1)
  mag   = min_stick + (32767 - min_stick) * norm
  sRX   =  vel.x / speed * mag                                # finger right  -> stick right
  sRY   = -vel.y / speed * mag                                # finger up (screen y falls) -> stick up = look up
  (sRY negated again if input_touch_camera_invert_y)
  clamp both to +-32767
```

- It works like a mouse: the camera turns while the finger moves and stops when the finger stops or
  lifts (lifting returns the stick to 0 at once).
- `min_stick` (default 8000) is where the output starts once past the deadzone, so slow drags are not
  swallowed by the game's own stick deadzone. Lower it if slow drags jump; raise it if they do nothing.
- The game's turn rate is capped at full stick, so very fast swipes saturate.
- Physical right stick wins: if either physical right-stick axis is beyond 6000, touch output is
  dropped and the smoothed speed reset.

### Dialogue tap -> left stick + A

A touch that started inside the band counts as a tap on release if it lasted at most
`input_touch_tap_max_ms` (500) and never moved more than `input_touch_tap_max_move` (70 px). The start
point picks the slot:

```
side = x >= input_touch_dialogue_x (650) ? right : left
row  = y <  input_touch_dialogue_row1 (618) ? upper
     : y <  input_touch_dialogue_row2 (653) ? middle
     : lower
D    = input_touch_dialogue_diagonal_deg (50)

             upper        middle     lower
right side   +D  (slot 1)  0  (slot 0) -D        (slot 5)
left side    180-D (slot 2) 180 (slot 3) -(180-D) (slot 4)

stick = 30000 * (cos(angle), sin(angle))         # stick convention: 0 = right, 90 = up
```

Playback (overrides the physical left stick while it runs):

1. left stick at the slot direction for `input_touch_dialogue_stick_ms` (150 ms);
2. same stick plus A for `input_touch_dialogue_a_ms` (100 ms);
3. release (stick back to the physical value, A up).

Each phase also waits until the game has read pad 1 at least twice (`XInputGetState`), so at low frame
rates the game still sees the stick before A. A phase never lasts longer than 2 s. A new tap while one
is playing is ignored. With `input_touch_dialogue_confirm = false` step 2 is skipped (aim only, press A
on the pad).

ME1 slot meaning (standard layout): right side = replies (upper usually the Paragon line, middle the
neutral line, lower the Renegade line); left side = upper Charm, middle Investigate, lower Intimidate.
Which slots are present differs per line of dialogue; tapping an empty slot selects nothing.

## Coordinates and tuning

- Touch coordinates are always 1280x720 screen pixels, independent of the internal render resolution
  (960x544) and of `video_mode_*`: the HUD is scaled to the full screen.
- Defaults come from a console screenshot of the RU dialogue wheel (1280x720 output): the wheel graphic
  is small, centered near (650, 640), radius about 40 px. The reply texts are right of it, at about
  y = 600 (upper), 636 (middle), 670 (lower), x ~700..1260. Left-side options are right-aligned text
  ending near x ~600 at the same heights. Players tap the text, not the wheel, so:
  - band `input_touch_dialogue_left/top/right/bottom` = 0, 580, 1280, 700 (the whole bottom strip);
  - side split `input_touch_dialogue_x` = 650 (wheel center);
  - row boundaries `input_touch_dialogue_row1/row2` = 618, 653 (midway between the text lines).
- To retune (other language, other HUD scale): take a Switch screenshot (Capture button) with the wheel
  open, read the y of each text line and set the row boundaries midway between them; set the band to
  cover the lines. With `input_touch_debug = true` every touch logs `[touch] down at (x, y)` and every
  tap its slot and stick angle.
- If the game picks the wrong upper/lower option, change `input_touch_dialogue_diagonal_deg`
  (e.g. 45 or 60).

## Limits

- The code cannot know whether a conversation is open. Outside conversations a short, still tap in the
  bottom band is a 250 ms left-stick nudge (Shepard takes a step) followed by A, which can use or open a
  nearby object. Set `input_touch_dialogue_confirm = false` or `input_touch_dialogue = false` if that
  is a problem.
- The bottom band also covers HUD elements shown outside conversations; taps there behave as above.
- Menus (main menu, inventory, galaxy map) are not mapped; they are not point-and-click on the Xbox 360.
- No multi-touch gestures (pinch, two-finger actions).
- Only pad 1 receives touch input; touch does nothing while pad 1 is disconnected.
- Route recording (`input_record`) stores the physical pad only, touches are not recorded.

## Switching off

`input_touch = false` in `masseffect.toml` (or only `input_touch_camera = false` /
`input_touch_dialogue = false`). The cvars are hot-reloadable: they are read on every poll.

## Console tuning 2026-10-08 (user, handheld)
Log of 38 touches: still taps moved 31-44 px between touch-down and lift (finger roll), so almost every tap became a
camera drag; one took 566 ms. Defaults changed: tap_max_move 30 -> 70 px, tap_max_ms 300 -> 500 ms. A 180 degree turn
needed too many swipes: camera_sensitivity 1.0 -> 2.5 (full stick at 400 px/s of finger speed).
Second session (same day, new defaults): taps on the reply lines select the right reply (user confirmed); camera speed OK.
