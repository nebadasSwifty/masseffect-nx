/**
 * @file        input/switch/switch_input_driver.cpp
 * @brief       Input driver for Nintendo Switch pads through libnx HID
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/switch/switch_input_driver.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <tuple>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>

#include <switch.h>

// Face buttons (1.0.1). By default each Switch button acts as the Xbox 360 button with the same
// letter: A accepts and B goes back, as in other Switch games. With true they go by position, as on
// an Xbox pad: the bottom one (B on the Switch) acts as A, the mapping of 1.0.0. It applies at once;
// the settings menu reads it too (window_switch.cpp).
REXCVAR_DEFINE_BOOL(input_xbox_layout, false, "Input",
                    "Face buttons by position, as on an Xbox pad (the bottom button, B, acts as A). "
                    "false = by letter: A accepts and B goes back");

// Route recording for unattended test runs: the game's own pad input, written and played back here.
// MINUS on pad 1 marks the start (and, while recording, the end), so the load time before the route does not
// matter; the game does not see MINUS while either is set. Lines: "ms<TAB>npad buttons (hex)<TAB>lx ly rx ry".
REXCVAR_DEFINE_STRING(input_record, "", "Input",
                      "Record pad 1 to this file (e.g. sdmc:/switch/masseffect-nx/route.tsv) between two MINUS presses");
REXCVAR_DEFINE_STRING(input_play, "", "Input",
                      "Play this recorded file (input_record) back as pad 1, starting at the next MINUS press");

// Touch screen (experimental, docs/touchscreen.md). The panel only reports touches in handheld mode;
// docked it reports nothing, so all of this is idle there. Everything is turned into pad 1 XInput
// state in Poll: a one-finger drag turns the camera (right stick), a short tap inside the dialogue
// band (option texts) pushes the left stick towards the tapped slot and then presses A.
REXCVAR_DEFINE_BOOL(input_touch, true, "Input",
                    "Experimental touch screen input (handheld only). false = touches are ignored");
REXCVAR_DEFINE_BOOL(input_touch_camera, true, "Input",
                    "One-finger drag moves the right stick (camera); band touches become drags once they move");
REXCVAR_DEFINE_BOOL(input_touch_dialogue, true, "Input",
                    "Short tap in the dialogue band: left stick to the tapped slot (side + row), then A");
REXCVAR_DEFINE_BOOL(input_touch_dialogue_confirm, true, "Input",
                    "Dialogue tap also presses A after the stick push. false = only the stick push");
REXCVAR_DEFINE_BOOL(input_touch_debug, false, "Input",
                    "Log every touch gesture and dialogue tap (position, angle, slot)");
REXCVAR_DEFINE_BOOL(input_rumble_dynamic, true, "Input",
                    "Rumble: the Joy-Con frequency follows the Xbox motor speed (and weak values are lifted), as an "
                    "eccentric motor does; false = fixed 160/320 Hz, only the strength changes");
REXCVAR_DEFINE_DOUBLE(input_touch_camera_sensitivity, 2.5, "Input",
                      "Touch camera: full right stick at 1000/sensitivity screen pixels per second of finger speed");
REXCVAR_DEFINE_DOUBLE(input_touch_camera_deadzone, 20.0, "Input",
                      "Touch camera: finger speeds below this many pixels per second give no stick");
REXCVAR_DEFINE_INT32(input_touch_camera_min_stick, 8000, "Input",
                     "Touch camera: smallest stick value sent once past the deadzone (jumps the game's own deadzone)");
REXCVAR_DEFINE_DOUBLE(input_touch_camera_smoothing_ms, 40.0, "Input",
                      "Touch camera: time constant of the finger speed smoothing in ms (0 = none)");
REXCVAR_DEFINE_BOOL(input_touch_camera_invert_y, false, "Input",
                    "Touch camera: drag up looks down instead of up");
// Dialogue tap band (1280x720 touch coordinates, measured on a console screenshot of the wheel: wheel
// graphic at about (650, 640), reply lines right of it at y ~600/636/670, investigate/charm/intimidate
// lines left of it at the same heights). Players tap the option text, so the band covers the lines.
REXCVAR_DEFINE_INT32(input_touch_dialogue_left, 0, "Input",
                     "Dialogue tap band: left edge (inclusive) in 1280x720 touch coordinates");
REXCVAR_DEFINE_INT32(input_touch_dialogue_top, 580, "Input",
                     "Dialogue tap band: top edge (inclusive)");
REXCVAR_DEFINE_INT32(input_touch_dialogue_right, 1280, "Input",
                     "Dialogue tap band: right edge (exclusive)");
REXCVAR_DEFINE_INT32(input_touch_dialogue_bottom, 700, "Input",
                     "Dialogue tap band: bottom edge (exclusive)");
REXCVAR_DEFINE_INT32(input_touch_dialogue_x, 650, "Input",
                     "Dialogue wheel center X: taps at or right of it pick the right-side (reply) slots");
REXCVAR_DEFINE_INT32(input_touch_dialogue_row1, 618, "Input",
                     "Dialogue tap: y below this is the upper slot row");
REXCVAR_DEFINE_INT32(input_touch_dialogue_row2, 653, "Input",
                     "Dialogue tap: y below this (and not upper) is the middle row, else the lower row");
REXCVAR_DEFINE_DOUBLE(input_touch_dialogue_diagonal_deg, 50.0, "Input",
                      "Stick angle above/below horizontal sent for the upper and lower wheel slots");
REXCVAR_DEFINE_INT32(input_touch_tap_max_ms, 500, "Input",
                     "A touch counts as a tap if it lasts at most this many ms");
REXCVAR_DEFINE_INT32(input_touch_tap_max_move, 70, "Input",
                     "A touch counts as a tap if the finger moves at most this many pixels");
REXCVAR_DEFINE_INT32(input_touch_dialogue_stick_ms, 150, "Input",
                     "Dialogue tap: how long the left stick is held towards the slot before A");
REXCVAR_DEFINE_INT32(input_touch_dialogue_a_ms, 100, "Input",
                     "Dialogue tap: how long A is held (the stick stays pushed meanwhile)");

namespace rex::ui {
// rex/ui/overlay/debug_overlay.h (not included here because it pulls in ImGui).
bool DebugOverlayOpen();
}  // namespace rex::ui

namespace rex::input::nx {

namespace {

constexpr int16_t kThumbThreshold = 0x4E00;
constexpr uint8_t kTriggerThreshold = 0x1F;
constexpr uint32_t kRepeatDelayMs = 400;
constexpr uint32_t kRepeatRateMs = 100;

// Low byte of a DeviceId is the slot, the upper half a per-run connection
// counter, so a pad that reconnects never gets an id it had before.
constexpr uint64_t kDeviceIdTag = 0x4E5800;  // "NX"

struct ButtonMapping {
  uint64_t npad;
  uint16_t xinput;
};

// Face buttons by letter (the default): the Switch's A is the Xbox A.
constexpr std::array<ButtonMapping, 4> kFrontsPerLetter = {{
    {HidNpadButton_A, X_INPUT_GAMEPAD_A},
    {HidNpadButton_B, X_INPUT_GAMEPAD_B},
    {HidNpadButton_X, X_INPUT_GAMEPAD_X},
    {HidNpadButton_Y, X_INPUT_GAMEPAD_Y},
}};

// By position (input_xbox_layout), as on an Xbox pad: the bottom one is A.
// On Switch pads that button is labelled B.
constexpr std::array<ButtonMapping, 4> kFrontsPerPosition = {{
    {HidNpadButton_B, X_INPUT_GAMEPAD_A},
    {HidNpadButton_A, X_INPUT_GAMEPAD_B},
    {HidNpadButton_Y, X_INPUT_GAMEPAD_X},
    {HidNpadButton_X, X_INPUT_GAMEPAD_Y},
}};

// The other buttons are the same with both layouts.
constexpr std::array<ButtonMapping, 10> kButtonMappings = {{
    {HidNpadButton_Up, X_INPUT_GAMEPAD_DPAD_UP},
    {HidNpadButton_Down, X_INPUT_GAMEPAD_DPAD_DOWN},
    {HidNpadButton_Left, X_INPUT_GAMEPAD_DPAD_LEFT},
    {HidNpadButton_Right, X_INPUT_GAMEPAD_DPAD_RIGHT},
    {HidNpadButton_Plus, X_INPUT_GAMEPAD_START},
    {HidNpadButton_Minus, X_INPUT_GAMEPAD_BACK},
    {HidNpadButton_L, X_INPUT_GAMEPAD_LEFT_SHOULDER},
    {HidNpadButton_R, X_INPUT_GAMEPAD_RIGHT_SHOULDER},
    {HidNpadButton_StickL, X_INPUT_GAMEPAD_LEFT_THUMB},
    {HidNpadButton_StickR, X_INPUT_GAMEPAD_RIGHT_THUMB},
}};

// The order of this list is also the order in which events are sent if
// multiple buttons change at once. Same table as the SDL driver.
constexpr std::array<rex::ui::VirtualKey, 34> kVkLookup = {
    // 00 - True buttons from xinput button field
    rex::ui::VirtualKey::kXInputPadDpadUp,
    rex::ui::VirtualKey::kXInputPadDpadDown,
    rex::ui::VirtualKey::kXInputPadDpadLeft,
    rex::ui::VirtualKey::kXInputPadDpadRight,
    rex::ui::VirtualKey::kXInputPadStart,
    rex::ui::VirtualKey::kXInputPadBack,
    rex::ui::VirtualKey::kXInputPadLThumbPress,
    rex::ui::VirtualKey::kXInputPadRThumbPress,
    rex::ui::VirtualKey::kXInputPadLShoulder,
    rex::ui::VirtualKey::kXInputPadRShoulder,
    rex::ui::VirtualKey::kNone, /* Guide has no VK */
    rex::ui::VirtualKey::kNone, /* Unknown */
    rex::ui::VirtualKey::kXInputPadA,
    rex::ui::VirtualKey::kXInputPadB,
    rex::ui::VirtualKey::kXInputPadX,
    rex::ui::VirtualKey::kXInputPadY,
    // 16 - Fake buttons generated from analog inputs
    rex::ui::VirtualKey::kXInputPadLTrigger,
    rex::ui::VirtualKey::kXInputPadRTrigger,
    // 18
    rex::ui::VirtualKey::kXInputPadLThumbUp,
    rex::ui::VirtualKey::kXInputPadLThumbDown,
    rex::ui::VirtualKey::kXInputPadLThumbRight,
    rex::ui::VirtualKey::kXInputPadLThumbLeft,
    rex::ui::VirtualKey::kXInputPadLThumbUpLeft,
    rex::ui::VirtualKey::kXInputPadLThumbUpRight,
    rex::ui::VirtualKey::kXInputPadLThumbDownRight,
    rex::ui::VirtualKey::kXInputPadLThumbDownLeft,
    // 26
    rex::ui::VirtualKey::kXInputPadRThumbUp,
    rex::ui::VirtualKey::kXInputPadRThumbDown,
    rex::ui::VirtualKey::kXInputPadRThumbRight,
    rex::ui::VirtualKey::kXInputPadRThumbLeft,
    rex::ui::VirtualKey::kXInputPadRThumbUpLeft,
    rex::ui::VirtualKey::kXInputPadRThumbUpRight,
    rex::ui::VirtualKey::kXInputPadRThumbDownRight,
    rex::ui::VirtualKey::kXInputPadRThumbDownLeft,
};

enum class RepeatState {
  kIdle,       // no buttons pressed or repeating has ended
  kWaiting,    // a button is held and the delay is awaited
  kRepeating,  // actively repeating at a rate
};

int16_t ClampStickAxis(s32 value) {
  return static_cast<int16_t>(std::clamp<s32>(value, INT16_MIN, INT16_MAX));
}

// Check if the analog inputs exceed their thresholds to become a button press
// and build the bitfield.
uint64_t AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) {
  uint64_t f = 0;

  f |= static_cast<uint64_t>(gamepad.left_trigger > kTriggerThreshold) << 16;
  f |= static_cast<uint64_t>(gamepad.right_trigger > kTriggerThreshold) << 17;

  auto thumb_x = static_cast<int16_t>(gamepad.thumb_lx);
  auto thumb_y = static_cast<int16_t>(gamepad.thumb_ly);
  for (size_t i = 0; i <= 8; i = i + 8) {
    uint64_t u = thumb_y > kThumbThreshold;
    uint64_t d = thumb_y < ~kThumbThreshold;
    uint64_t r = thumb_x > kThumbThreshold;
    uint64_t l = thumb_x < ~kThumbThreshold;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);

    thumb_x = static_cast<int16_t>(gamepad.thumb_rx);
    thumb_y = static_cast<int16_t>(gamepad.thumb_ry);
  }
  return f;
}

}  // namespace

struct SwitchInputDriver::Slot {
  HidNpadIdType npad_id = HidNpadIdType_No1;
  PadState pad{};
  bool connected = false;
  DeviceId id = DeviceId::kInvalid;

  X_INPUT_STATE state{};
  bool state_changed = false;
  bool is_active = true;

  /*
   * One bit per menu shortcut, so it fires only on press and not while held.
   * See DispatchMenuShortcuts.
   */
  uint64_t menu_shortcuts = 0;

  // Keystroke synthesis, per pad rather than per guest user.
  uint64_t key_buttons = 0;
  RepeatState repeat_state = RepeatState::kIdle;
  uint8_t repeat_index = 0;
  uint32_t repeat_time = 0;

  // Vibration devices are initialized for one controller layout; redone only
  // when the attached layout changes.
  HidNpadIdType vibration_npad_id = HidNpadIdType_No1;
  uint32_t vibration_style = 0;
  int vibration_count = 0;
  std::array<HidVibrationDeviceHandle, 2> vibration_handles{};
  // The last value sent to the controller. Stack sampling put 5.8 % of the thread that prepares
  // each frame inside SetState: the game calls XInputSetState very often, and every call ended
  // in hidSendVibrationValues, an IPC request that blocks the thread. With vibration off
  // (input_rumble = false) it kept sending "zero" over and over. If the requested value is
  // the same as the controller already has, nothing is sent: the controller is already in that
  // state.
  bool vibration_sent = false;
  uint16_t vibration_left = 0;
  uint16_t vibration_right = 0;
};

SwitchInputDriver::SwitchInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

SwitchInputDriver::~SwitchInputDriver() {
  if (!initialized_) {
    return;
  }
  // XInput vibration holds until changed, so a pad left rumbling would keep
  // rumbling after the process exits.
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& slot : slots_) {
    if (slot && slot->vibration_count) {
      std::array<HidVibrationValue, 2> stop{};
      for (auto& value : stop) {
        value.freq_low = 160.0f;
        value.freq_high = 320.0f;
      }
      hidSendVibrationValues(slot->vibration_handles.data(), stop.data(), slot->vibration_count);
    }
  }
}

X_STATUS SwitchInputDriver::Setup() {
  // Reference counted in libnx. MarathonRecomp-NX found that without it the
  // first padUpdate can abort with 2345-0008 when HID was not brought up by
  // __appInit.
  Result rc = hidInitialize();
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchInputDriver: hidInitialize failed: 0x{:08X}", rc);
    return X_STATUS_UNSUCCESSFUL;
  }
  padConfigureInput(kSlotCount, HidNpadStyleSet_NpadStandard);

  static constexpr std::array<HidNpadIdType, kSlotCount> kNpadIds = {
      HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4};
  for (size_t i = 0; i < kSlotCount; ++i) {
    auto slot = std::make_unique<Slot>();
    slot->npad_id = kNpadIds[i];
    if (i == 0) {
      padInitialize(&slot->pad, HidNpadIdType_No1, HidNpadIdType_Handheld);
    } else {
      padInitialize(&slot->pad, kNpadIds[i]);
    }
    slots_[i] = std::move(slot);
  }
  hidPermitVibration(true);
  // Always activated (cheap, and the settings menu does it too) so input_touch can be switched on
  // while running; Poll reads it only while input_touch is true.
  hidInitializeTouchScreen();
  REXLOG_INFO(
      "[touch] input_touch={} camera={} (sensitivity {:.2f}, deadzone {:.0f} px/s, min stick {}, smoothing "
      "{:.0f} ms, invert_y={}) dialogue={} (band x {}..{} y {}..{}, center x {}, rows {}/{}, diagonal {:.0f} "
      "deg, tap <= {} ms / "
      "{} px, stick {} ms, A {} ms, confirm={}) debug={}",
      REXCVAR_GET(input_touch), REXCVAR_GET(input_touch_camera), REXCVAR_GET(input_touch_camera_sensitivity),
      REXCVAR_GET(input_touch_camera_deadzone), REXCVAR_GET(input_touch_camera_min_stick),
      REXCVAR_GET(input_touch_camera_smoothing_ms), REXCVAR_GET(input_touch_camera_invert_y),
      REXCVAR_GET(input_touch_dialogue), REXCVAR_GET(input_touch_dialogue_left),
      REXCVAR_GET(input_touch_dialogue_right), REXCVAR_GET(input_touch_dialogue_top),
      REXCVAR_GET(input_touch_dialogue_bottom), REXCVAR_GET(input_touch_dialogue_x),
      REXCVAR_GET(input_touch_dialogue_row1), REXCVAR_GET(input_touch_dialogue_row2),
      REXCVAR_GET(input_touch_dialogue_diagonal_deg), REXCVAR_GET(input_touch_tap_max_ms),
      REXCVAR_GET(input_touch_tap_max_move), REXCVAR_GET(input_touch_dialogue_stick_ms),
      REXCVAR_GET(input_touch_dialogue_a_ms), REXCVAR_GET(input_touch_dialogue_confirm),
      REXCVAR_GET(input_touch_debug));
  initialized_ = true;
  return X_STATUS_SUCCESS;
}

namespace {
struct RouteSample {
  uint32_t ms;
  uint64_t buttons;
  int32_t lx, ly, rx, ry;
};
struct RouteState {
  bool configured = false;
  std::string record_path, play_path;
  FILE* record = nullptr;
  bool recording = false, playing = false, minus_before = false;
  uint64_t start_tick = 0, last_flush_tick = 0;
  RouteSample last{};
  std::vector<RouteSample> samples;
  size_t next = 0;
  RouteSample current{};
};
RouteState route_;
uint32_t RouteMs(uint64_t start) { return uint32_t(armTicksToNs(armGetSystemTick() - start) / 1000000); }
}  // namespace

// input_record / input_play (see the cvars above). Pad 1 only, on the polling thread.
static void RouteRecording(uint64_t& held, HidAnalogStickState& left, HidAnalogStickState& right) {
  RouteState& r = route_;
  if (!r.configured) {
    r.configured = true;
    r.record_path = REXCVAR_GET(input_record);
    r.play_path = REXCVAR_GET(input_play);
    if (!r.play_path.empty()) {
      if (FILE* f = std::fopen(r.play_path.c_str(), "r")) {
        RouteSample m{};
        unsigned long long buttons = 0;
        while (std::fscanf(f, "%u\t%llx\t%d %d %d %d", &m.ms, &buttons, &m.lx, &m.ly, &m.rx, &m.ry) == 6) {
          m.buttons = buttons;
          r.samples.push_back(m);
        }
        std::fclose(f);
      }
      REXLOG_INFO("input_play: {} samples from {}", r.samples.size(), r.play_path);
    }
  }
  if (r.record_path.empty() && r.play_path.empty()) {
    return;
  }
  const bool minus = (held & HidNpadButton_Minus) != 0;
  const bool minus_pressed = minus && !r.minus_before;
  r.minus_before = minus;
  held &= ~uint64_t(HidNpadButton_Minus);  // the marker is not game input
  if (!r.record_path.empty()) {
    if (minus_pressed && !r.recording) {
      r.record = std::fopen(r.record_path.c_str(), "w");
      r.recording = r.record != nullptr;
      r.start_tick = r.last_flush_tick = armGetSystemTick();
      r.last = RouteSample{~0u, ~0ull, 0, 0, 0, 0};
      REXLOG_INFO("input_record: recording to {} ({})", r.record_path, r.recording ? "open" : "FAILED");
    } else if (minus_pressed && r.recording) {
      std::fclose(r.record);
      r.record = nullptr;
      r.recording = false;
      REXLOG_INFO("input_record: recording closed");
    }
    if (r.recording) {
      const RouteSample m{RouteMs(r.start_tick), held, left.x, left.y, right.x, right.y};
      if (m.buttons != r.last.buttons || m.lx != r.last.lx || m.ly != r.last.ly || m.rx != r.last.rx ||
          m.ry != r.last.ry) {
        std::fprintf(r.record, "%u\t%llx\t%d %d %d %d\n", m.ms, static_cast<unsigned long long>(m.buttons), m.lx,
                     m.ly, m.rx, m.ry);
        r.last = m;
      }
      if (armTicksToNs(armGetSystemTick() - r.last_flush_tick) > 1000000000ull) {
        std::fflush(r.record);
        r.last_flush_tick = armGetSystemTick();
      }
    }
  }
  if (!r.play_path.empty() && !r.samples.empty()) {
    if (minus_pressed && !r.playing) {
      r.playing = true;
      r.next = 0;
      r.current = RouteSample{};
      r.start_tick = armGetSystemTick();
      REXLOG_INFO("input_play: playback started");
    }
    if (r.playing) {
      const uint32_t now = RouteMs(r.start_tick);
      while (r.next < r.samples.size() && r.samples[r.next].ms <= now) {
        r.current = r.samples[r.next++];
      }
      if (r.next >= r.samples.size() && now > r.samples.back().ms + 500) {
        r.playing = false;
        r.current = RouteSample{};
        REXLOG_INFO("input_play: playback finished");
      }
      // The recording replaces the pad completely (sys-botbase's own presses included).
      held = r.current.buttons;
      left.x = r.current.lx;
      left.y = r.current.ly;
      right.x = r.current.rx;
      right.y = r.current.ry;
    }
  }
}

/*
 * Touch screen (input_touch*, docs/touchscreen.md)
 *
 * Read from HID shared memory in Poll, on the guest thread that asks for pad 1, under mutex_: no
 * thread of its own. Poll runs several times per game frame (state, keystrokes, vibration), so
 * everything here is driven by the system tick, not by the number of calls.
 *
 * Gestures (one finger only; a second finger cancels until all fingers are lifted):
 *  - touch starts inside the dialogue band (input_touch_dialogue): candidate tap. On release, if it
 *    was short and still, the left stick is pushed towards the tapped slot (side of the wheel center,
 *    row by y), then A is pressed. Once the finger moves more than input_touch_tap_max_move it
 *    becomes a camera drag instead (if input_touch_camera).
 *  - touch starts anywhere else (input_touch_camera): camera drag, finger speed -> right stick.
 */
namespace {

constexpr int16_t kPhysicalStickActive = 6000;  // physical right stick beyond this wins over touch
constexpr double kVelocityWindowMs = 8.0;       // finger speed is measured over at least this long
constexpr double kStaleSampleMs = 200.0;        // longer gaps between polls do not count as motion
constexpr int16_t kTapStick = 30000;            // left stick magnitude of a dialogue tap
constexpr uint32_t kTapMinReads = 2;            // the game must read each tap phase at least twice
constexpr double kTapPhaseTimeoutMs = 2000.0;   // ...but a phase never lasts longer than this
constexpr double kPi = 3.14159265358979323846;

enum class Gesture { kNone, kCamera, kWheel, kIgnored };
enum class TapPhase { kIdle, kStick, kConfirm };

struct TouchInput {
  bool down = false;
  Gesture gesture = Gesture::kNone;
  uint32_t finger_id = 0;
  float start_x = 0, start_y = 0;
  float max_move = 0;  // farthest distance from the start point, in pixels
  uint64_t start_tick = 0;
  // Camera: last point used for the speed, and the smoothed speed in pixels per second.
  float last_x = 0, last_y = 0;
  uint64_t last_tick = 0;
  double vel_x = 0, vel_y = 0;
  // Dialogue tap playback.
  TapPhase phase = TapPhase::kIdle;
  uint64_t phase_tick = 0;
  uint32_t phase_reads = 0;
  int16_t tap_lx = 0, tap_ly = 0;
};
TouchInput touch_;

double TicksToMs(uint64_t ticks) { return double(armTicksToNs(ticks)) / 1000000.0; }

const char* WheelSlotName(int slot) {
  static const char* const kNames[6] = {"right",     "upper-right", "upper-left",
                                        "left",      "lower-left",  "lower-right"};
  return kNames[slot % 6];
}

// True if (x, y) is inside the dialogue tap band.
bool InDialogueBand(float x, float y) {
  return x >= REXCVAR_GET(input_touch_dialogue_left) && x < REXCVAR_GET(input_touch_dialogue_right) &&
         y >= REXCVAR_GET(input_touch_dialogue_top) && y < REXCVAR_GET(input_touch_dialogue_bottom);
}

// A finished tap at (x, y) inside the band: starts the stick + A playback for the tapped slot.
void StartDialogueTap(float x, float y, uint64_t now) {
  const bool debug = REXCVAR_GET(input_touch_debug);
  if (touch_.phase != TapPhase::kIdle) {
    if (debug) {
      REXLOG_INFO("[touch] tap at ({:.0f}, {:.0f}) ignored: previous tap still playing", x, y);
    }
    return;
  }
  // Slots, counter-clockwise from the right: 0 right, 1 upper-right, 2 upper-left, 3 left,
  // 4 lower-left, 5 lower-right. Side from the wheel center x, row from the two y boundaries.
  // The stick goes exactly along the slot direction: right side +D / 0 / -D, left side
  // 180-D / 180 / -(180-D), with D = input_touch_dialogue_diagonal_deg (50).
  const bool right_side = x >= REXCVAR_GET(input_touch_dialogue_x);
  const int row = y < REXCVAR_GET(input_touch_dialogue_row1) ? 0 : (y < REXCVAR_GET(input_touch_dialogue_row2) ? 1 : 2);
  static const int kRightSlots[3] = {1, 0, 5};
  static const int kLeftSlots[3] = {2, 3, 4};
  const int slot = right_side ? kRightSlots[row] : kLeftSlots[row];
  const double diag = std::clamp(REXCVAR_GET(input_touch_dialogue_diagonal_deg), 5.0, 85.0);
  const double kSlotAngles[6] = {0.0, diag, 180.0 - diag, 180.0, -180.0 + diag, -diag};
  const double angle = kSlotAngles[slot];
  const double rad = angle * kPi / 180.0;
  touch_.tap_lx = int16_t(std::lround(std::cos(rad) * kTapStick));
  touch_.tap_ly = int16_t(std::lround(std::sin(rad) * kTapStick));
  touch_.phase = TapPhase::kStick;
  touch_.phase_tick = now;
  touch_.phase_reads = 0;
  if (debug) {
    REXLOG_INFO("[touch] dialogue tap at ({:.0f}, {:.0f}): slot {} ({}), angle {:.0f} deg, stick ({}, {})", x, y,
                slot, WheelSlotName(slot), angle, touch_.tap_lx, touch_.tap_ly);
  }
}

// Turns the touch screen into pad 1 input. lx..ry and buttons hold the physical pad on entry.
void ApplyTouch(int16_t& lx, int16_t& ly, int16_t& rx, int16_t& ry, uint16_t& buttons) {
  TouchInput& t = touch_;
  const uint64_t now = armGetSystemTick();
  const bool enabled = REXCVAR_GET(input_touch);
  const bool camera = enabled && REXCVAR_GET(input_touch_camera);
  const bool dialogue = enabled && REXCVAR_GET(input_touch_dialogue);
  const bool debug = REXCVAR_GET(input_touch_debug);

  HidTouchScreenState state{};
  int count = 0;
  if (enabled && hidGetTouchScreenStates(&state, 1) != 0) {
    count = state.count;
  }

  if (count == 0) {
    // Release: a short, still touch inside the wheel is a dialogue tap.
    if (t.down && t.gesture == Gesture::kWheel && dialogue) {
      const double held_ms = TicksToMs(now - t.start_tick);
      if (held_ms <= REXCVAR_GET(input_touch_tap_max_ms) && t.max_move <= REXCVAR_GET(input_touch_tap_max_move)) {
        StartDialogueTap(t.start_x, t.start_y, now);
      } else if (debug) {
        REXLOG_INFO("[touch] band touch not a tap ({:.0f} ms, moved {:.0f} px)", held_ms, t.max_move);
      }
    }
    t.down = false;
    t.gesture = Gesture::kNone;
    t.vel_x = t.vel_y = 0;
  } else {
    const HidTouchState& f = state.touches[0];
    const float x = float(f.x);
    const float y = float(f.y);
    if (!t.down) {
      t.down = true;
      t.finger_id = f.finger_id;
      t.start_x = t.last_x = x;
      t.start_y = t.last_y = y;
      t.start_tick = t.last_tick = now;
      t.max_move = 0;
      t.vel_x = t.vel_y = 0;
      if (count > 1) {
        t.gesture = Gesture::kIgnored;
      } else if (dialogue && InDialogueBand(x, y)) {
        t.gesture = Gesture::kWheel;
      } else if (camera) {
        t.gesture = Gesture::kCamera;
      } else {
        t.gesture = Gesture::kIgnored;
      }
      if (debug) {
        static const char* const kNames[] = {"none", "camera", "dialogue band", "ignored"};
        REXLOG_INFO("[touch] down at ({:.0f}, {:.0f}), {} finger(s): {}", x, y, count,
                    kNames[static_cast<int>(t.gesture)]);
      }
    } else if (count > 1 || f.finger_id != t.finger_id) {
      // Second finger, or the first one lifted while another stays: cancel until all are lifted.
      t.gesture = Gesture::kIgnored;
      t.vel_x = t.vel_y = 0;
    }
    const float mx = x - t.start_x, my = y - t.start_y;
    t.max_move = std::max(t.max_move, std::sqrt(mx * mx + my * my));
    // A band touch that moves past the tap limit is no tap: it turns into a camera drag, measured
    // from here on (speed starts at 0, so there is no jump).
    if (t.gesture == Gesture::kWheel && t.max_move > REXCVAR_GET(input_touch_tap_max_move)) {
      t.gesture = camera ? Gesture::kCamera : Gesture::kIgnored;
      t.last_x = x;
      t.last_y = y;
      t.last_tick = now;
      t.vel_x = t.vel_y = 0;
      if (debug) {
        REXLOG_INFO("[touch] band touch moved {:.0f} px: {}", t.max_move, camera ? "camera drag" : "ignored");
      }
    }

    if (t.gesture == Gesture::kCamera) {
      const double dt_ms = TicksToMs(now - t.last_tick);
      if (dt_ms >= kStaleSampleMs) {
        t.vel_x = t.vel_y = 0;
        t.last_x = x;
        t.last_y = y;
        t.last_tick = now;
      } else if (dt_ms >= kVelocityWindowMs) {
        const double inst_x = double(x - t.last_x) * 1000.0 / dt_ms;
        const double inst_y = double(y - t.last_y) * 1000.0 / dt_ms;
        const double tau = REXCVAR_GET(input_touch_camera_smoothing_ms);
        const double alpha = tau > 0.0 ? 1.0 - std::exp(-dt_ms / tau) : 1.0;
        t.vel_x += (inst_x - t.vel_x) * alpha;
        t.vel_y += (inst_y - t.vel_y) * alpha;
        t.last_x = x;
        t.last_y = y;
        t.last_tick = now;
      }
    }
  }

  // Camera: finger speed -> right stick, only while the physical right stick is at rest.
  if (t.down && t.gesture == Gesture::kCamera) {
    const bool physical = std::abs(int(rx)) > kPhysicalStickActive || std::abs(int(ry)) > kPhysicalStickActive;
    if (physical) {
      t.vel_x = t.vel_y = 0;  // no jump when the physical stick is released again
    } else {
      const double speed = std::sqrt(t.vel_x * t.vel_x + t.vel_y * t.vel_y);
      const double dz = std::max(0.0, REXCVAR_GET(input_touch_camera_deadzone));
      const double sens = std::max(0.01, REXCVAR_GET(input_touch_camera_sensitivity));
      const double full = 1000.0 / sens;  // finger speed for full deflection
      double out_x = 0, out_y = 0;
      if (speed > dz) {
        const double norm = full > dz ? std::clamp((speed - dz) / (full - dz), 0.0, 1.0) : 1.0;
        const double min_stick = std::clamp(double(REXCVAR_GET(input_touch_camera_min_stick)), 0.0, 32767.0);
        const double mag = min_stick + (32767.0 - min_stick) * norm;
        // Finger right -> stick right. Screen Y grows downwards and stick Y grows upwards, so a
        // drag up (negative screen Y speed) is stick up = look up.
        out_x = t.vel_x / speed * mag;
        out_y = -t.vel_y / speed * mag;
        if (REXCVAR_GET(input_touch_camera_invert_y)) {
          out_y = -out_y;
        }
      }
      rx = int16_t(std::clamp(std::lround(out_x), -32767L, 32767L));
      ry = int16_t(std::clamp(std::lround(out_y), -32767L, 32767L));
    }
  }

  // Dialogue tap playback: stick towards the slot, then stick + A, then release. Each phase lasts its
  // time and at least kTapMinReads reads by the game, so slow frames do not skip the stick push.
  if (t.phase != TapPhase::kIdle) {
    const double phase_ms = TicksToMs(now - t.phase_tick);
    const bool timed_out = phase_ms >= kTapPhaseTimeoutMs;
    if (t.phase == TapPhase::kStick &&
        ((phase_ms >= REXCVAR_GET(input_touch_dialogue_stick_ms) && t.phase_reads >= kTapMinReads) || timed_out)) {
      t.phase = REXCVAR_GET(input_touch_dialogue_confirm) ? TapPhase::kConfirm : TapPhase::kIdle;
      t.phase_tick = now;
      t.phase_reads = 0;
    } else if (t.phase == TapPhase::kConfirm &&
               ((phase_ms >= REXCVAR_GET(input_touch_dialogue_a_ms) && t.phase_reads >= kTapMinReads) || timed_out)) {
      t.phase = TapPhase::kIdle;
    }
    if (t.phase != TapPhase::kIdle) {
      lx = t.tap_lx;
      ly = t.tap_ly;
      if (t.phase == TapPhase::kConfirm) {
        buttons |= X_INPUT_GAMEPAD_A;
      }
    }
  }
}

// GetDeviceState handed pad 1 to the game: counts reads for the tap phases.
void NoteTouchRead() {
  if (touch_.phase != TapPhase::kIdle) {
    ++touch_.phase_reads;
  }
}

}  // namespace

void SwitchInputDriver::Poll(size_t index) {
  Slot& slot = *slots_[index];
  padUpdate(&slot.pad);

  const bool connected = padIsConnected(&slot.pad);
  if (connected != slot.connected) {
    slot.connected = connected;
    if (connected) {
      slot.id = DeviceId((uint64_t(++connection_counter_) << 32) | (kDeviceIdTag << 8) | index);
      slot.state = {};
      slot.state_changed = true;
      slot.key_buttons = 0;
      slot.repeat_state = RepeatState::kIdle;
      slot.vibration_count = 0;
    } else {
      slot.id = DeviceId::kInvalid;
    }
  }
  if (!connected) {
    return;
  }

  uint64_t held = padGetButtons(&slot.pad);
  HidAnalogStickState left = padGetStickPos(&slot.pad, 0);
  HidAnalogStickState right = padGetStickPos(&slot.pad, 1);
  if (index == 0) {
    RouteRecording(held, left, right);
  }
  DispatchMenuShortcuts(slot, held);
  X_INPUT_GAMEPAD gamepad{};
  uint16_t buttons = 0;
  const auto& fronts =
      REXCVAR_GET(input_xbox_layout) ? kFrontsPerPosition : kFrontsPerLetter;
  for (const ButtonMapping& mapping : fronts) {
    if (held & mapping.npad) {
      buttons |= mapping.xinput;
    }
  }
  for (const ButtonMapping& mapping : kButtonMappings) {
    if (held & mapping.npad) {
      buttons |= mapping.xinput;
    }
  }
  // HID reports up as positive Y, like XInput, so no inversion is needed.
  int16_t thumb_lx = ClampStickAxis(left.x);
  int16_t thumb_ly = ClampStickAxis(left.y);
  int16_t thumb_rx = ClampStickAxis(right.x);
  int16_t thumb_ry = ClampStickAxis(right.y);
  // The touch screen belongs to the console itself, so it feeds pad 1 (handheld) only.
  if (index == 0) {
    ApplyTouch(thumb_lx, thumb_ly, thumb_rx, thumb_ry, buttons);
  }
  gamepad.buttons = buttons;
  // ZL and ZR are digital.
  gamepad.left_trigger = (held & HidNpadButton_ZL) ? 0xFF : 0;
  gamepad.right_trigger = (held & HidNpadButton_ZR) ? 0xFF : 0;
  gamepad.thumb_lx = thumb_lx;
  gamepad.thumb_ly = thumb_ly;
  gamepad.thumb_rx = thumb_rx;
  gamepad.thumb_ry = thumb_ry;
  /*
   * With the debug overlay open, L+R and the right stick move it (debug_overlay.cpp,
   * on the UI thread). While L and R are held, the game sees neither those two
   * buttons nor the right stick.
   */
  constexpr uint64_t kMoveOverlay = HidNpadButton_L | HidNpadButton_R;
  if ((held & kMoveOverlay) == kMoveOverlay && rex::ui::DebugOverlayOpen()) {
    // The XInput state fields are stored big-endian: they are rewritten whole.
    gamepad.buttons = static_cast<uint16_t>(
        buttons & ~(X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_RIGHT_SHOULDER));
    gamepad.thumb_rx = static_cast<int16_t>(0);
    gamepad.thumb_ry = static_cast<int16_t>(0);
  }

  if (std::memcmp(&gamepad, &slot.state.gamepad, sizeof(gamepad)) != 0) {
    slot.state.gamepad = gamepad;
    slot.state_changed = true;
  }
}


/* From the profiler (switch_perf.cpp): turns the GPU A/B test lap on and off. */
extern "C" void RexSwitchPerfToggleAb(void);
/*
 * The SDK menus, on the controller
 *
 * On the PC they are function keys (see RegisterBind in rex_app.cpp): F3 debug,
 * F4 settings, F7 achievements and the tilde key for the log console. The Switch
 * has no keyboard, so they go to combinations with L+R, which no game uses
 * together with the D-pad:
 *
 *   L + R + Up    -> debug overlay (counters, statistics)
 *   L + R + Right -> settings
 *   L + R + Down  -> log console
 *   L + R + Left  -> achievements
 *   L + R + ZL    -> GPU A/B test lap (for measuring, not for playing)
 *   L + R + right stick -> moves the debug overlay if it is open
 *                          (its position is saved; see Poll and debug_overlay.cpp)
 *
 * It fires once per press, not while held: each shortcut stores its bit in
 * slot.menu_shortcuts with the state from the previous poll.
 */
void SwitchInputDriver::DispatchMenuShortcuts(Slot& slot, uint64_t held) {
  struct Shortcut {
    uint64_t button;
    rex::ui::VirtualKey keystroke;
  };
  constexpr uint64_t kModifier = HidNpadButton_L | HidNpadButton_R;
  static const Shortcut kShortcuts[] = {
      {HidNpadButton_Up, rex::ui::VirtualKey::kF3},
      {HidNpadButton_Right, rex::ui::VirtualKey::kF4},
      {HidNpadButton_Down, rex::ui::VirtualKey::kOem3},
      {HidNpadButton_Left, rex::ui::VirtualKey::kF7},
  };

  const bool modifier = (held & kModifier) == kModifier;
  for (size_t i = 0; i < std::size(kShortcuts); ++i) {
    const uint64_t bit = uint64_t(1) << i;
    const bool now = modifier && (held & kShortcuts[i].button) != 0;
    const bool before = (slot.menu_shortcuts & bit) != 0;
    if (now && !before) {
      /*
       * Each shortcut creates or destroys an ImGui menu, and the UI thread
       * draws them continuously while any is open. From this game thread a
       * menu could be deleted in the middle of its Draw, so it is done on the
       * UI thread.
       */
      const rex::ui::VirtualKey vk = kShortcuts[i].keystroke;
      auto press = [vk] {
        rex::ui::KeyEvent keystroke(nullptr, vk, 1, false, false, false, false, false);
        rex::ui::ProcessKeyEvent(keystroke);
      };
      rex::ui::Window* const window = window_ui_.load(std::memory_order_acquire);
      if (!window || !window->app_context().CallInUIThread(press)) {
        press();
      }
      REXLOG_INFO("menu shortcut: {}", rex::ui::VirtualKeyToString(vk));
    }
    slot.menu_shortcuts = now ? (slot.menu_shortcuts | bit) : (slot.menu_shortcuts & ~bit);
  }

  /*
   * L+R+ZL starts the A/B test lap (switch_perf.cpp). It sends no key to the
   * game: it talks directly to the profiler. It is kept apart from the loop
   * above because that one translates buttons into keys and this is not a key.
   */
  const uint64_t kBitAb = uint64_t(1) << 8;
  const bool ab_now = modifier && (held & HidNpadButton_ZL) != 0;
  const bool ab_before = (slot.menu_shortcuts & kBitAb) != 0;
  if (ab_now && !ab_before) {
    RexSwitchPerfToggleAb();
    REXLOG_INFO("menu shortcut: GPU A/B tests");
  }
  slot.menu_shortcuts = ab_now ? (slot.menu_shortcuts | kBitAb)
                                 : (slot.menu_shortcuts & ~kBitAb);
}

SwitchInputDriver::Slot* SwitchInputDriver::FindSlot(DeviceId id) {
  const size_t index = static_cast<uint64_t>(id) & 0xFF;
  if (id == DeviceId::kInvalid || index >= kSlotCount) {
    return nullptr;
  }
  Poll(index);
  Slot* slot = slots_[index].get();
  return slot->connected && slot->id == id ? slot : nullptr;
}

void SwitchInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  if (!initialized_) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < kSlotCount; ++i) {
    Poll(i);
    const Slot& slot = *slots_[i];
    if (!slot.connected) {
      continue;
    }
    DeviceInfo info;
    info.id = slot.id;
    const uint32_t style = padGetStyleSet(&slot.pad);
    if (i == 0 && slot.pad.active_handheld) {
      info.name = "Nintendo Switch (handheld)";
    } else if (style & HidNpadStyleTag_NpadFullKey) {
      info.name = "Pro Controller";
    } else if (style & HidNpadStyleTag_NpadJoyDual) {
      info.name = "Joy-Con (L/R)";
    } else if (style & HidNpadStyleTag_NpadJoyLeft) {
      info.name = "Joy-Con (L)";
    } else if (style & HidNpadStyleTag_NpadJoyRight) {
      info.name = "Joy-Con (R)";
    } else {
      info.name = "Nintendo Switch controller";
    }
    info.guid = "switch-npad-" + std::to_string(i + 1);
    info.synthetic = false;
    out.push_back(std::move(info));
  }
}

X_RESULT SwitchInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  std::lock_guard<std::mutex> lock(mutex_);
  Slot* slot = FindSlot(id);
  if (!slot) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (slot == slots_[0].get()) {
    NoteTouchRead();
  }

  // Make sure packet_number is only incremented by 1, even if there have been
  // multiple updates between GetState calls. Also track `is_active` to
  // increment the packet number if it changed.
  const bool is_active = this->is_active();
  if (is_active != slot->is_active || (is_active && slot->state_changed)) {
    slot->state.packet_number = uint32_t(slot->state.packet_number) + 1;
    slot->is_active = is_active;
    slot->state_changed = false;
  }
  std::memcpy(out_state, &slot->state, sizeof(*out_state));
  if (!is_active) {
    // Simulate an "untouched" controller.
    std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SwitchInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                                  X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  if (!out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  Slot* slot = FindSlot(id);
  if (!slot) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  const bool handheld = slot->npad_id == HidNpadIdType_No1 && slot->pad.active_handheld;
  out_caps->flags = handheld ? 0 : X_INPUT_CAPS_WIRELESS;
  out_caps->gamepad.buttons = 0xF3FF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  out_caps->gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  out_caps->gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  out_caps->gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  out_caps->vibration.left_motor_speed = 0xFFFFu;
  out_caps->vibration.right_motor_speed = 0xFFFFu;
  return X_ERROR_SUCCESS;
}

X_RESULT SwitchInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  std::lock_guard<std::mutex> lock(mutex_);
  Slot* slot = FindSlot(id);
  if (!slot) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  HidNpadIdType npad_id = slot->npad_id;
  uint32_t style = 0;
  int count = 0;
  if (slot->npad_id == HidNpadIdType_No1 && slot->pad.active_handheld) {
    npad_id = HidNpadIdType_Handheld;
    style = HidNpadStyleTag_NpadHandheld;
    count = 2;
  } else {
    const uint32_t attached = hidGetNpadStyleSet(npad_id);
    if (attached & HidNpadStyleTag_NpadFullKey) {
      style = HidNpadStyleTag_NpadFullKey;
      count = 2;
    } else if (attached & HidNpadStyleTag_NpadJoyDual) {
      style = HidNpadStyleTag_NpadJoyDual;
      count = 2;
    } else if (attached & HidNpadStyleTag_NpadJoyLeft) {
      style = HidNpadStyleTag_NpadJoyLeft;
      count = 1;
    } else if (attached & HidNpadStyleTag_NpadJoyRight) {
      style = HidNpadStyleTag_NpadJoyRight;
      count = 1;
    } else {
      // Nothing that can rumble.
      return X_ERROR_SUCCESS;
    }
  }

  if (!slot->vibration_count || slot->vibration_npad_id != npad_id ||
      slot->vibration_style != style) {
    Result rc = hidInitializeVibrationDevices(slot->vibration_handles.data(), count, npad_id,
                                              static_cast<HidNpadStyleTag>(style));
    if (R_FAILED(rc)) {
      slot->vibration_count = 0;
      return X_ERROR_FUNCTION_FAILED;
    }
    slot->vibration_npad_id = npad_id;
    slot->vibration_style = style;
    slot->vibration_count = count;
    slot->vibration_sent = false;  // new or reassigned controller: it needs the state sent
    REXLOG_INFO("[rumble] vibration devices for npad {} style 0x{:X}: {} handle(s) {:08X} {:08X}", int(npad_id),
                style, count, slot->vibration_handles[0].type_value,
                count > 1 ? slot->vibration_handles[1].type_value : 0u);
  }

  // See the vibration_sent comment in Slot: same value as the last one sent, nothing to do.
  const uint16_t left = uint16_t(vibration->left_motor_speed);
  const uint16_t right = uint16_t(vibration->right_motor_speed);
  if (slot->vibration_sent && slot->vibration_left == left &&
      slot->vibration_right == right) {
    return X_ERROR_SUCCESS;
  }

  // XInput's left motor is the heavy low-frequency one, the right motor the
  // light high-frequency one.
  // An Xbox rumble motor is an eccentric mass: a faster motor shakes harder AND at a higher pitch. With a fixed
  // frequency the Joy-Cons only change loudness and every effect feels the same (user, 2026-10-08), so with
  // input_rumble_dynamic the frequency follows the motor speed too (low band 80-160 Hz, high band 200-320 Hz, inside
  // what the Joy-Con actuator plays well), and weak values are lifted (square root) so light effects are felt.
  HidVibrationValue value{};
  const float l = float(uint16_t(vibration->left_motor_speed)) / 65535.0f;
  const float r = float(uint16_t(vibration->right_motor_speed)) / 65535.0f;
  if (REXCVAR_GET(input_rumble_dynamic)) {
    value.amp_low = std::sqrt(l);
    value.freq_low = 80.0f + 80.0f * l;
    value.amp_high = std::sqrt(r) * 0.8f;
    value.freq_high = 200.0f + 120.0f * r;
  } else {
    value.amp_low = l;
    value.freq_low = 160.0f;
    value.amp_high = r;
    value.freq_high = 320.0f;
  }
  std::array<HidVibrationValue, 2> values = {value, value};
  if (R_FAILED(hidSendVibrationValues(slot->vibration_handles.data(), values.data(),
                                      slot->vibration_count))) {
    slot->vibration_sent = false;  // retried on the next call
    return X_ERROR_FUNCTION_FAILED;
  }
  if (!slot->vibration_sent || (left && !slot->vibration_left) || (right && !slot->vibration_right)) {
    static int logged = 0;
    if (logged < 20) {
      ++logged;
      REXLOG_INFO("[rumble] sent low {:.2f} high {:.2f} to {} handle(s)", value.amp_low, value.amp_high,
                  slot->vibration_count);
    }
  }
  slot->vibration_sent = true;
  slot->vibration_left = left;
  slot->vibration_right = right;
  return X_ERROR_SUCCESS;
}

X_RESULT SwitchInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                               X_INPUT_KEYSTROKE* out_keystroke) {
  (void)flags;
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  Slot* slot = FindSlot(id);
  if (!slot) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // If input is not active (e.g. due to a dialog overlay), force buttons to
  // "unpressed". The algorithm will automatically send UP events when
  // 'is_active()' goes low and DOWN events when it goes high again.
  const bool is_active = this->is_active();
  const uint64_t curr_butts =
      is_active ? (static_cast<uint64_t>(static_cast<uint16_t>(slot->state.gamepad.buttons)) |
                   AnalogToKeyfield(slot->state.gamepad))
                : uint64_t(0);

  // Handle repeating
  const auto guest_now = static_cast<uint32_t>(rex::chrono::Clock::QueryGuestUptimeMillis());
  if (slot->repeat_state == RepeatState::kWaiting &&
      (slot->repeat_time + kRepeatDelayMs < guest_now)) {
    slot->repeat_state = RepeatState::kRepeating;
  }
  if (slot->repeat_state == RepeatState::kRepeating &&
      (slot->repeat_time + kRepeatRateMs < guest_now)) {
    slot->repeat_time = guest_now;
    rex::ui::VirtualKey vk = kVkLookup.at(slot->repeat_index);
    out_keystroke->virtual_key = uint16_t(vk);
    out_keystroke->unicode = 0;
    // InputSystem stamps the guest user this device is assigned to.
    out_keystroke->user_index = 0;
    out_keystroke->hid_code = 0;
    out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
    return X_ERROR_SUCCESS;
  }

  const uint64_t butts_changed = curr_butts ^ slot->key_buttons;
  if (!butts_changed) {
    return X_ERROR_EMPTY;
  }

  // First try to clear buttons with up events. This is to match xinput
  // behavior when transitioning thumb sticks, e.g. so that THUMB_UPLEFT is
  // up before THUMB_LEFT is down.
  for (auto [clear_pass, pass] = std::tuple{true, 0}; pass < 2; clear_pass = false, pass++) {
    for (uint8_t i = 0; i < uint8_t(std::size(kVkLookup)); i++) {
      const uint64_t fbutton = uint64_t(1) << i;
      if (!(butts_changed & fbutton)) {
        continue;
      }
      rex::ui::VirtualKey vk = kVkLookup.at(i);
      if (vk == rex::ui::VirtualKey::kNone) {
        continue;
      }

      out_keystroke->virtual_key = uint16_t(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = 0;
      out_keystroke->hid_code = 0;

      const bool is_pressed = curr_butts & fbutton;
      if (clear_pass && !is_pressed) {
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
        slot->key_buttons &= ~fbutton;
        slot->repeat_state = RepeatState::kIdle;
        return X_ERROR_SUCCESS;
      }
      if (!clear_pass && is_pressed) {
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
        slot->key_buttons |= fbutton;
        slot->repeat_state = RepeatState::kWaiting;
        slot->repeat_index = i;
        slot->repeat_time = guest_now;
        return X_ERROR_SUCCESS;
      }
    }
  }
  return X_ERROR_EMPTY;
}

}  // namespace rex::input::nx
