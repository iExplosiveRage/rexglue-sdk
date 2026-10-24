/**
 * @file        input/mnk/mnk_input_driver.cpp
 * @brief       Keyboard/mouse input driver implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_input_driver.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

// On by default: the keyboard plays as a controller next to the pads (both
// drive player 1, or the keyboard is player keyboard_player), so the game is
// playable without one.
REXCVAR_DEFINE_BOOL(mnk_mode, true, "Input",
                    "Keyboard play: the keyboard works as a controller (with or without a pad)");
REXCVAR_DEFINE_BOOL(mnk_mouse, false, "Input",
                    "Use the mouse for the right stick. Off means the right stick comes "
                    "from the keybind_rstick_* keys only");
REXCVAR_DEFINE_DOUBLE(mnk_sensitivity, 1.0, "Input", "Mouse sensitivity for right stick")
    .range(0.01, 10.0);
REXCVAR_DEFINE_UINT32(keyboard_player, 1, "Input",
                      "The player the keyboard controls: 1 (together with the first "
                      "controller) or 2-4 (for example player 2 of a local Versus next to a pad)")
    .range(1, 4);

// The defaults suit a fighting game on one keyboard: move with WASD or the
// arrows, the right hand on J / K / L / I / U / O. Each value is a
// comma-separated list of keys, a key optionally with Shift+ / Ctrl+ / Alt+.
REXCVAR_DEFINE_STRING(keybind_a, "Space,L", "Input/Keybinds/Controller", "A button");
REXCVAR_DEFINE_STRING(keybind_b, "K,Backspace", "Input/Keybinds/Controller", "B button");
REXCVAR_DEFINE_STRING(keybind_x, "J", "Input/Keybinds/Controller", "X button");
REXCVAR_DEFINE_STRING(keybind_y, "I", "Input/Keybinds/Controller", "Y button");
REXCVAR_DEFINE_STRING(keybind_left_trigger, "U", "Input/Keybinds/Controller", "Left trigger");
REXCVAR_DEFINE_STRING(keybind_right_trigger, "O", "Input/Keybinds/Controller", "Right trigger");
REXCVAR_DEFINE_STRING(keybind_left_shoulder, "Q", "Input/Keybinds/Controller", "Left shoulder");
REXCVAR_DEFINE_STRING(keybind_right_shoulder, "E", "Input/Keybinds/Controller", "Right shoulder");
REXCVAR_DEFINE_STRING(keybind_lstick_up, "W,Up", "Input/Keybinds/Controller", "Left stick up");
REXCVAR_DEFINE_STRING(keybind_lstick_down, "S,Down", "Input/Keybinds/Controller",
                      "Left stick down");
REXCVAR_DEFINE_STRING(keybind_lstick_left, "A,Left", "Input/Keybinds/Controller",
                      "Left stick left");
REXCVAR_DEFINE_STRING(keybind_lstick_right, "D,Right", "Input/Keybinds/Controller",
                      "Left stick right");
REXCVAR_DEFINE_STRING(keybind_lstick_press, "F", "Input/Keybinds/Controller", "Left stick press");
REXCVAR_DEFINE_STRING(keybind_rstick_up, "Numpad8", "Input/Keybinds/Controller", "Right stick up");
REXCVAR_DEFINE_STRING(keybind_rstick_down, "Numpad2", "Input/Keybinds/Controller",
                      "Right stick down");
REXCVAR_DEFINE_STRING(keybind_rstick_left, "Numpad4", "Input/Keybinds/Controller",
                      "Right stick left");
REXCVAR_DEFINE_STRING(keybind_rstick_right, "Numpad6", "Input/Keybinds/Controller",
                      "Right stick right");
REXCVAR_DEFINE_STRING(keybind_rstick_press, "G", "Input/Keybinds/Controller", "Right stick press");
REXCVAR_DEFINE_STRING(keybind_dpad_up, "Shift+Up,Shift+W", "Input/Keybinds/Controller",
                      "D-pad up");
REXCVAR_DEFINE_STRING(keybind_dpad_down, "Shift+Down,Shift+S", "Input/Keybinds/Controller",
                      "D-pad down");
REXCVAR_DEFINE_STRING(keybind_dpad_left, "Shift+Left,Shift+A", "Input/Keybinds/Controller",
                      "D-pad left");
REXCVAR_DEFINE_STRING(keybind_dpad_right, "Shift+Right,Shift+D", "Input/Keybinds/Controller",
                      "D-pad right");
REXCVAR_DEFINE_STRING(keybind_back, "Tab", "Input/Keybinds/Controller", "Back button");
REXCVAR_DEFINE_STRING(keybind_start, "Return", "Input/Keybinds/Controller", "Start button");
REXCVAR_DEFINE_STRING(keybind_guide, "", "Input/Keybinds/Controller", "Guide button");
// The menu keys: what they press depends on the screen (SetMenuKeyButtons).
REXCVAR_DEFINE_STRING(keybind_menu_confirm, "Return", "Input/Keybinds/Controller",
                      "Menu confirm (A, Start at the title)");
REXCVAR_DEFINE_STRING(keybind_menu_back, "Escape", "Input/Keybinds/Controller",
                      "Menu back (B, pause in a fight)");

REXCVAR_DEFINE_BOOL(mnk_passthrough, false, "Input",
                    "Surface raw keystrokes to the title instead of emulating a pad, for "
                    "titles that read a USB keyboard. Enables the device on its own");

namespace rex::input::mnk {

using rex::ui::VirtualKey;

namespace {

// A single device, so its handle is a constant.
constexpr rex::input::DeviceId kMnkDevice = static_cast<rex::input::DeviceId>(0x4D4E4B00);

// Bounds the queue for titles that never call XamInputGetKeystroke.
constexpr size_t kMaxQueuedKeystrokes = 256;

// Bind values are a comma-separated list of alternatives, each optionally
// carrying modifier prefixes: "Q,I" or "Shift+W". Modifier matching is exact,
// so "Up" stays silent while Shift is held and "Shift+Up" stays silent without
// it. That is what lets the D-pad share the arrow keys. A consequence is
// that binding a bare modifier name ("Shift") can never fire, since holding it
// makes the live mask non-zero while the bind wants zero.
constexpr uint8_t kModShift = 1u << 0;
constexpr uint8_t kModCtrl = 1u << 1;
constexpr uint8_t kModAlt = 1u << 2;

uint8_t LiveModifiers(const bool (&key_down)[256]) {
  uint8_t mods = 0;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kShift)])
    mods |= kModShift;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kControl)])
    mods |= kModCtrl;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kMenu)])
    mods |= kModAlt;
  return mods;
}

// Strips leading modifier prefixes off 'token', advancing it to the bare key
// name and returning the mask they require.
uint8_t TakeModifiers(std::string_view& token) {
  uint8_t mods = 0;
  for (;;) {
    size_t plus = token.find('+');
    if (plus == std::string_view::npos || plus == 0) {
      break;
    }
    std::string_view head = token.substr(0, plus);
    uint8_t bit = 0;
    if (head == "Shift") {
      bit = kModShift;
    } else if (head == "Ctrl" || head == "Control") {
      bit = kModCtrl;
    } else if (head == "Alt") {
      bit = kModAlt;
    } else {
      break;
    }
    mods |= bit;
    token.remove_prefix(plus + 1);
  }
  return mods;
}

std::string_view TrimSpaces(std::string_view s) {
  while (!s.empty() && s.front() == ' ') {
    s.remove_prefix(1);
  }
  while (!s.empty() && s.back() == ' ') {
    s.remove_suffix(1);
  }
  return s;
}

bool TokenPressed(const bool (&key_down)[256], std::string_view token, uint8_t live_mods) {
  uint8_t want = TakeModifiers(token);
  if (want != live_mods) {
    return false;
  }
  rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(token);
  if (vk == rex::ui::VirtualKey::kNone) {
    return false;
  }
  uint16_t idx = static_cast<uint16_t>(vk);
  return idx < 256 && key_down[idx];
}

std::atomic<bool> mouse_look_active{true};

// SetMenuKeyButtons: confirm in the low 16 bits, back in the high.
std::atomic<uint32_t> menu_key_buttons{uint32_t(X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_START) |
                                       (uint32_t(X_INPUT_GAMEPAD_B) << 16)};

// ui::VirtualKey follows Windows VK_ numbering; the guest wants USB HID usage
// page 0x07.
uint8_t VirtualKeyToHIDUsage(rex::ui::VirtualKey vk_enum) {
  const uint32_t vk = static_cast<uint32_t>(vk_enum);

  // The runs below are contiguous in both numbering schemes.
  if (vk >= 'A' && vk <= 'Z') {
    return static_cast<uint8_t>(vk - 'A' + 0x04);
  }
  // 0 is irregular in both digit runs; handled in the switch.
  if (vk >= '1' && vk <= '9') {
    return static_cast<uint8_t>(vk - '1' + 0x1E);
  }
  if (vk >= static_cast<uint32_t>(VirtualKey::kF1) &&
      vk <= static_cast<uint32_t>(VirtualKey::kF12)) {
    return static_cast<uint8_t>(vk - static_cast<uint32_t>(VirtualKey::kF1) + 0x3A);
  }
  if (vk >= static_cast<uint32_t>(VirtualKey::kF13) &&
      vk <= static_cast<uint32_t>(VirtualKey::kF24)) {
    return static_cast<uint8_t>(vk - static_cast<uint32_t>(VirtualKey::kF13) + 0x68);
  }
  if (vk >= static_cast<uint32_t>(VirtualKey::kNumpad1) &&
      vk <= static_cast<uint32_t>(VirtualKey::kNumpad9)) {
    return static_cast<uint8_t>(vk - static_cast<uint32_t>(VirtualKey::kNumpad1) + 0x59);
  }
  if (vk >= static_cast<uint32_t>(VirtualKey::kLShift) &&
      vk <= static_cast<uint32_t>(VirtualKey::kLMenu)) {
    return static_cast<uint8_t>(vk - static_cast<uint32_t>(VirtualKey::kLShift) + 0xE1);
  }

  switch (vk_enum) {
    case VirtualKey::k0:
      return 0x27;
    case VirtualKey::kReturn:
      return 0x28;
    case VirtualKey::kEscape:
      return 0x29;
    case VirtualKey::kBack:
      return 0x2A;
    case VirtualKey::kTab:
      return 0x2B;
    case VirtualKey::kSpace:
      return 0x2C;
    case VirtualKey::kOemMinus:
      return 0x2D;
    case VirtualKey::kOemPlus:
      return 0x2E;
    case VirtualKey::kOem4:
      return 0x2F;
    case VirtualKey::kOem6:
      return 0x30;
    case VirtualKey::kOem5:
      return 0x31;
    case VirtualKey::kOem1:
      return 0x33;
    case VirtualKey::kOem7:
      return 0x34;
    case VirtualKey::kOem3:
      return 0x35;
    case VirtualKey::kOemComma:
      return 0x36;
    case VirtualKey::kOemPeriod:
      return 0x37;
    case VirtualKey::kOem2:
      return 0x38;
    case VirtualKey::kCapital:
      return 0x39;
    case VirtualKey::kSnapshot:
      return 0x46;
    case VirtualKey::kScroll:
      return 0x47;
    case VirtualKey::kPause:
      return 0x48;
    case VirtualKey::kInsert:
      return 0x49;
    case VirtualKey::kHome:
      return 0x4A;
    case VirtualKey::kPrior:
      return 0x4B;
    case VirtualKey::kDelete:
      return 0x4C;
    case VirtualKey::kEnd:
      return 0x4D;
    case VirtualKey::kNext:
      return 0x4E;
    case VirtualKey::kRight:
      return 0x4F;
    case VirtualKey::kLeft:
      return 0x50;
    case VirtualKey::kDown:
      return 0x51;
    case VirtualKey::kUp:
      return 0x52;
    case VirtualKey::kNumLock:
      return 0x53;
    case VirtualKey::kDivide:
      return 0x54;
    case VirtualKey::kMultiply:
      return 0x55;
    case VirtualKey::kSubtract:
      return 0x56;
    case VirtualKey::kAdd:
      return 0x57;
    case VirtualKey::kNumpad0:
      return 0x62;
    case VirtualKey::kDecimal:
      return 0x63;
    case VirtualKey::kApps:
      return 0x65;
    default:
      return 0x00;
  }
}

// Sticks and triggers are included: XInput reports those as directional pad
// keys, which is what menu navigation reads.
struct PadBind {
  const std::string& keys;
  VirtualKey pad_key;
};

// Rebuilt per call because the cvars are live-editable.
std::array<PadBind, 24> PadBinds() {
  return {{
      {REXCVAR_GET(keybind_a), VirtualKey::kXInputPadA},
      {REXCVAR_GET(keybind_b), VirtualKey::kXInputPadB},
      {REXCVAR_GET(keybind_x), VirtualKey::kXInputPadX},
      {REXCVAR_GET(keybind_y), VirtualKey::kXInputPadY},
      {REXCVAR_GET(keybind_left_shoulder), VirtualKey::kXInputPadLShoulder},
      {REXCVAR_GET(keybind_right_shoulder), VirtualKey::kXInputPadRShoulder},
      {REXCVAR_GET(keybind_left_trigger), VirtualKey::kXInputPadLTrigger},
      {REXCVAR_GET(keybind_right_trigger), VirtualKey::kXInputPadRTrigger},
      {REXCVAR_GET(keybind_lstick_press), VirtualKey::kXInputPadLThumbPress},
      {REXCVAR_GET(keybind_rstick_press), VirtualKey::kXInputPadRThumbPress},
      {REXCVAR_GET(keybind_back), VirtualKey::kXInputPadBack},
      {REXCVAR_GET(keybind_start), VirtualKey::kXInputPadStart},
      {REXCVAR_GET(keybind_dpad_up), VirtualKey::kXInputPadDpadUp},
      {REXCVAR_GET(keybind_dpad_down), VirtualKey::kXInputPadDpadDown},
      {REXCVAR_GET(keybind_dpad_left), VirtualKey::kXInputPadDpadLeft},
      {REXCVAR_GET(keybind_dpad_right), VirtualKey::kXInputPadDpadRight},
      {REXCVAR_GET(keybind_lstick_up), VirtualKey::kXInputPadLThumbUp},
      {REXCVAR_GET(keybind_lstick_down), VirtualKey::kXInputPadLThumbDown},
      {REXCVAR_GET(keybind_lstick_left), VirtualKey::kXInputPadLThumbLeft},
      {REXCVAR_GET(keybind_lstick_right), VirtualKey::kXInputPadLThumbRight},
      {REXCVAR_GET(keybind_rstick_up), VirtualKey::kXInputPadRThumbUp},
      {REXCVAR_GET(keybind_rstick_down), VirtualKey::kXInputPadRThumbDown},
      {REXCVAR_GET(keybind_rstick_left), VirtualKey::kXInputPadRThumbLeft},
      {REXCVAR_GET(keybind_rstick_right), VirtualKey::kXInputPadRThumbRight},
  }};
}

}  // namespace

void SetMenuKeyButtons(uint16_t confirm, uint16_t back) {
  menu_key_buttons.store(uint32_t(confirm) | (uint32_t(back) << 16), std::memory_order_relaxed);
}

void SetMouseLookActive(bool active) {
  mouse_look_active.store(active, std::memory_order_relaxed);
}

bool IsMouseLookActive() {
  return mouse_look_active.load(std::memory_order_relaxed);
}

MnkInputDriver::MnkInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

MnkInputDriver::~MnkInputDriver() {
  // Detach handled by OnClosing; if window outlives the driver, clean up here.
  DetachFromWindow();
}

X_STATUS MnkInputDriver::Setup() {
  return X_STATUS_SUCCESS;
}

void MnkInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window) {
    {
      std::lock_guard lock(state_mutex_);
      attached_window_ = window;
    }
    window->AddInputListener(this, window_z_order());
    window->AddListener(this);
  }
}

void MnkInputDriver::OnClosing(rex::ui::UIEvent&) {
  DetachFromWindow();
}

void MnkInputDriver::DetachFromWindow() {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }
  window->app_context().CallInUIThreadSynchronous([this, window] {
    // Detach first so nothing new is queued, then run out what already was.
    {
      std::lock_guard lock(state_mutex_);
      attached_window_ = nullptr;
    }
    if (mouse_capture_update_queued_.load(std::memory_order_relaxed)) {
      window->app_context().ExecutePendingFunctionsFromUIThread();
    }
    ReleaseMouseCaptureFromUIThread(window);
    window->RemoveInputListener(this);
    window->RemoveListener(this);
  });
}

bool MnkInputDriver::IsEnabled() const {
  // Passthrough enables the device on its own.
  return REXCVAR_GET(mnk_mode) || REXCVAR_GET(mnk_passthrough);
}

static bool IsPassthroughEnabled() {
  return REXCVAR_GET(mnk_passthrough);
}

static bool IsBindPressed(const bool (&key_down)[256], const std::string& cvar_val) {
  const uint8_t live_mods = LiveModifiers(key_down);
  std::string_view rest(cvar_val);
  while (!rest.empty()) {
    size_t comma = rest.find(',');
    std::string_view token = rest.substr(0, comma);
    if (comma == std::string_view::npos) {
      rest = std::string_view();
    } else {
      rest.remove_prefix(comma + 1);
    }
    token = TrimSpaces(token);
    if (!token.empty() && TokenPressed(key_down, token, live_mods)) {
      return true;
    }
  }
  return false;
}

void MnkInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  // Disabled means no device at all, so it never occupies a guest user slot.
  if (!IsEnabled()) {
    return;
  }
  DeviceInfo info;
  info.id = kMnkDevice;
  info.name = IsPassthroughEnabled() ? "Keyboard" : "Keyboard and Mouse";
  info.subtype =
      IsPassthroughEnabled() ? XINPUT_DEVSUBTYPE_USB_KEYBOARD : XINPUT_DEVSUBTYPE_GAMEPAD;
  info.synthetic = true;
  info.keyboard = true;
  const uint32_t player = REXCVAR_GET(keyboard_player);
  info.user_index = player >= 2 && player <= kMaxGuestUsers ? int32_t(player - 1) : -1;
  out.push_back(info);
}

X_RESULT MnkInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                               X_INPUT_CAPABILITIES* out_caps) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    if (IsPassthroughEnabled()) {
      out_caps->type = XINPUT_DEVTYPE_KEYBOARD;
      out_caps->sub_type = XINPUT_DEVSUBTYPE_USB_KEYBOARD;
      return X_ERROR_SUCCESS;
    }
    out_caps->type = XINPUT_DEVTYPE_GAMEPAD;
    out_caps->sub_type = XINPUT_DEVSUBTYPE_GAMEPAD;
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Keys reach the guest through GetDeviceKeystroke only, leaving the guest
  // user free for a real controller.
  if (IsPassthroughEnabled()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Mouse look is opt in. Without this gate, keyboard input alone would hide
  // and lock the cursor, breaking the ImGui overlays.
  QueueMouseCaptureUpdate(IsEnabled() && REXCVAR_GET(mnk_mouse) && IsMouseLookActive() &&
                          has_focus_ && is_active());

  std::lock_guard lock(state_mutex_);

  // The keys that count: the real ones while the game has the input (focus,
  // no dialog over it), and any injected ones.
  const bool live = is_active() && has_focus_;
  bool keys[256];
  bool any_injected = false;
  for (int i = 0; i < 256; ++i) {
    keys[i] = (live && key_down_[i]) || injected_down_[i];
    any_injected |= injected_down_[i];
  }
  if (!live && !any_injected) {
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
    }
    return X_ERROR_SUCCESS;
  }

  // The menu keys press what the screen wants them to (SetMenuKeyButtons);
  // while they do, they're kept from the other binds (Enter is also Start).
  uint16_t buttons = 0;
  {
    const uint32_t menu = menu_key_buttons.load(std::memory_order_relaxed);
    const uint16_t confirm_buttons = uint16_t(menu & 0xFFFF);
    const uint16_t back_buttons = uint16_t(menu >> 16);
    bool menu_keys[256] = {};
    auto take = [&](const std::string& bind, uint16_t pad_buttons) {
      if (!pad_buttons || !IsBindPressed(keys, bind)) {
        return;
      }
      buttons |= pad_buttons;
      std::string_view rest(bind);
      while (!rest.empty()) {
        const size_t comma = rest.find(',');
        std::string_view token = TrimSpaces(rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
        TakeModifiers(token);
        const uint16_t vk = static_cast<uint16_t>(rex::ui::ParseVirtualKey(token));
        if (vk && vk < 256) {
          menu_keys[vk] = true;
        }
      }
    };
    take(REXCVAR_GET(keybind_menu_confirm), confirm_buttons);
    take(REXCVAR_GET(keybind_menu_back), back_buttons);
    for (int i = 0; i < 256; ++i) {
      keys[i] = keys[i] && !menu_keys[i];
    }
  }

  if (IsBindPressed(keys, REXCVAR_GET(keybind_a)))
    buttons |= X_INPUT_GAMEPAD_A;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_b)))
    buttons |= X_INPUT_GAMEPAD_B;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_x)))
    buttons |= X_INPUT_GAMEPAD_X;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_y)))
    buttons |= X_INPUT_GAMEPAD_Y;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_left_shoulder)))
    buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_right_shoulder)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_lstick_press)))
    buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_rstick_press)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_back)))
    buttons |= X_INPUT_GAMEPAD_BACK;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_start)))
    buttons |= X_INPUT_GAMEPAD_START;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_guide)))
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_dpad_up)))
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_dpad_down)))
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_dpad_left)))
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_dpad_right)))
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

  uint8_t lt = IsBindPressed(keys, REXCVAR_GET(keybind_left_trigger)) ? 0xFF : 0;
  uint8_t rt = IsBindPressed(keys, REXCVAR_GET(keybind_right_trigger)) ? 0xFF : 0;

  int32_t lx = 0;
  int32_t ly = 0;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_lstick_left)))
    lx -= INT16_MAX;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_lstick_right)))
    lx += INT16_MAX;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_lstick_up)))
    ly += INT16_MAX;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_lstick_down)))
    ly -= INT16_MAX;

  int32_t rx = 0;
  int32_t ry = 0;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_rstick_left)))
    rx -= INT16_MAX;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_rstick_right)))
    rx += INT16_MAX;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_rstick_up)))
    ry += INT16_MAX;
  if (IsBindPressed(keys, REXCVAR_GET(keybind_rstick_down)))
    ry -= INT16_MAX;

  if (REXCVAR_GET(mnk_mouse) && IsMouseLookActive()) {
    double sensitivity = REXCVAR_GET(mnk_sensitivity);
    constexpr double kBaseScale = 200.0;
    rx += static_cast<int32_t>(double(mouse_dx_) * sensitivity * kBaseScale);
    ry += static_cast<int32_t>(double(-mouse_dy_) * sensitivity * kBaseScale);
  }
  // Drained unconditionally: deltas keep accumulating in OnMouseMove while the
  // mouse is off, and toggling it on would otherwise dump the whole backlog
  // into one frame as a camera snap.
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;

  auto clamp16 = [](int32_t v) -> int16_t {
    return static_cast<int16_t>(std::clamp(v, (int32_t)INT16_MIN, (int32_t)INT16_MAX));
  };

  packet_number_++;

  if (out_state) {
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = clamp16(lx);
    out_state->gamepad.thumb_ly = clamp16(ly);
    out_state->gamepad.thumb_rx = clamp16(rx);
    out_state->gamepad.thumb_ry = clamp16(ry);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                            X_INPUT_KEYSTROKE* out_keystroke) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = keystroke_queue_.front();
  }
  keystroke_queue_.pop();
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::EnqueueKeystroke(uint16_t vk_pad, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = vk_pad;
  ks.unicode = 0;
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  // InputSystem stamps the guest user this device is assigned to.
  ks.user_index = 0;
  ks.hid_code = 0;
  PushKeystroke(ks);
}

void MnkInputDriver::PushKeystroke(const X_INPUT_KEYSTROKE& ks) {
  if (keystroke_queue_.size() >= kMaxQueuedKeystrokes) {
    keystroke_queue_.pop();
  }
  keystroke_queue_.push(ks);
}

void MnkInputDriver::EnqueueRawKeystroke(const rex::ui::KeyEvent& e, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = static_cast<uint16_t>(e.virtual_key());
  ks.unicode = 0;
  ks.user_index = 0;
  ks.hid_code = VirtualKeyToHIDUsage(e.virtual_key());

  uint16_t flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  // Already down, so this is the OS auto-repeat.
  if (down && e.prev_state()) {
    flags |= X_INPUT_KEYSTROKE_REPEAT;
  }
  if (e.is_shift_pressed()) {
    flags |= X_INPUT_KEYSTROKE_SHIFT;
  }
  if (e.is_ctrl_pressed()) {
    flags |= X_INPUT_KEYSTROKE_CTRL;
  }
  if (e.is_alt_pressed()) {
    flags |= X_INPUT_KEYSTROKE_ALT;
  }
  ks.flags = flags;
  PushKeystroke(ks);
}

void MnkInputDriver::RefreshBoundKeystrokesLocked() {
  const auto binds = PadBinds();
  uint32_t pressed = 0;
  for (size_t i = 0; i < binds.size(); i++) {
    if (IsBindPressed(key_down_, binds[i].keys)) {
      pressed |= uint32_t(1) << i;
    }
  }

  // The whole table is diffed rather than the key that moved: binds carry
  // modifiers, so Shift alone can flip several of them at once.
  const uint32_t changed = pressed ^ bound_pressed_;
  bound_pressed_ = pressed;
  if (!changed) {
    return;
  }
  for (size_t i = 0; i < binds.size(); i++) {
    const uint32_t bit = uint32_t(1) << i;
    if (changed & bit) {
      EnqueueKeystroke(static_cast<uint16_t>(binds[i].pad_key), (pressed & bit) != 0);
    }
  }
}

void MnkInputDriver::QueueMouseCaptureUpdate(bool should_capture) {
  mouse_capture_requested_.store(should_capture, std::memory_order_relaxed);
  bool already_queued = false;
  if (!mouse_capture_update_queued_.compare_exchange_strong(already_queued, true)) {
    return;
  }
  std::lock_guard lock(state_mutex_);
  if (!attached_window_) {
    mouse_capture_update_queued_.store(false, std::memory_order_relaxed);
    return;
  }
  // Deferred, not CallInUIThread: running inline would re-enter state_mutex_.
  attached_window_->app_context().CallInUIThreadDeferred([this] {
    mouse_capture_update_queued_.store(false, std::memory_order_relaxed);
    ApplyMouseCaptureFromUIThread();
  });
}

void MnkInputDriver::ApplyMouseCaptureFromUIThread() {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }
  bool should_capture = mouse_capture_requested_.load(std::memory_order_relaxed);
  if (should_capture == mouse_captured_) {
    return;
  }
  if (!should_capture) {
    ReleaseMouseCaptureFromUIThread(window);
    return;
  }
  mouse_captured_ = true;
  precapture_cursor_visibility_ = window->GetCursorVisibility();
  window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
  window->CaptureMouse();
  relative_mouse_mode_ = window->SetRelativeMouseMode(true);
  if (!relative_mouse_mode_) {
    REXLOG_WARN("Pointer lock unavailable, mouse look falls back to recentering the cursor");
  }
  // Reset deltas to avoid a spike on capture start
  std::lock_guard lock(state_mutex_);
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;
}

void MnkInputDriver::ReleaseMouseCaptureFromUIThread(rex::ui::Window* window) {
  if (!mouse_captured_) {
    return;
  }
  mouse_captured_ = false;
  window->SetCursorVisibility(precapture_cursor_visibility_);
  window->SetRelativeMouseMode(false);
  relative_mouse_mode_ = false;
  window->ReleaseMouse();
}

void MnkInputDriver::RecenterCursorFromUIThread(int32_t x, int32_t y) {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }
  int32_t width = int32_t(window->GetActualPhysicalWidth());
  int32_t height = int32_t(window->GetActualPhysicalHeight());
  if (width <= 0 || height <= 0) {
    return;
  }
  // Only once the pointer has drifted well off the middle, to keep warps rare.
  if (std::abs(x - width / 2) < width / 4 && std::abs(y - height / 2) < height / 4) {
    return;
  }
  int32_t center_x = 0;
  int32_t center_y = 0;
  if (window->WarpMouseToCenter(center_x, center_y)) {
    prev_mouse_x_ = center_x;
    prev_mouse_y_ = center_y;
  }
}

void MnkInputDriver::SetKeyState(uint16_t vk, bool down) {
  if (vk < 256) {
    key_down_[vk] = down;
  }
}

void MnkInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  const bool injected = rex::ui::Window::IsInjectingKey();
  if (!IsEnabled() || (!has_focus_ && !injected))
    return;
  // Typing into a text box (the console) is not playing.
  if (!injected && attached_window_ && attached_window_->IsTextInputActive())
    return;
  std::lock_guard lock(state_mutex_);
  if (IsPassthroughEnabled()) {
    EnqueueRawKeystroke(e, true);
    return;
  }
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  if (injected) {
    if (vk < 256) {
      injected_down_[vk] = true;
    }
  } else {
    SetKeyState(vk, true);
  }
  RefreshBoundKeystrokesLocked();
}

void MnkInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  if (IsPassthroughEnabled()) {
    EnqueueRawKeystroke(e, false);
    return;
  }
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  if (rex::ui::Window::IsInjectingKey()) {
    if (vk < 256) {
      injected_down_[vk] = false;
    }
  } else {
    SetKeyState(vk, false);
  }
  RefreshBoundKeystrokesLocked();
}

void MnkInputDriver::OnKeyChar(rex::ui::KeyEvent& e) {
  if (!IsEnabled() || !IsPassthroughEnabled())
    return;
  // WM_CHAR convention: the codepoint arrives after its key-down, so it is
  // attached to the keystroke already queued for that key.
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return;
  }
  X_INPUT_KEYSTROKE& last = keystroke_queue_.back();
  const uint16_t flags = last.flags;
  if (!(flags & X_INPUT_KEYSTROKE_KEYDOWN) || last.unicode != 0) {
    return;
  }
  last.unicode = static_cast<uint16_t>(e.virtual_key());
  last.flags = static_cast<uint16_t>(flags | X_INPUT_KEYSTROKE_VALIDUNICODE);
}

void MnkInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), true);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), true);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), true);
      break;
    default:
      break;
  }
  RefreshBoundKeystrokesLocked();
}

void MnkInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), false);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), false);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), false);
      break;
    default:
      break;
  }
  RefreshBoundKeystrokesLocked();
}

void MnkInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  int32_t x = e.x();
  int32_t y = e.y();
  {
    std::lock_guard lock(state_mutex_);
    if (relative_mouse_mode_) {
      // The pointer is locked, so the absolute position no longer moves.
      mouse_dx_ += e.dx();
      mouse_dy_ += e.dy();
    } else {
      mouse_dx_ += float(x - prev_mouse_x_);
      mouse_dy_ += float(y - prev_mouse_y_);
    }
  }
  prev_mouse_x_ = x;
  prev_mouse_y_ = y;
  // Without a pointer lock the cursor still has to be kept off the edges.
  if (mouse_captured_ && !relative_mouse_mode_) {
    RecenterCursorFromUIThread(x, y);
  }
}

void MnkInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  has_focus_ = false;
  // Withdraw the request too, or an update queued before the focus loss grabs
  // the cursor straight back.
  mouse_capture_requested_.store(false, std::memory_order_relaxed);
  {
    std::lock_guard lock(state_mutex_);
    std::memset(key_down_, 0, sizeof(key_down_));
    // Releases landing on another window never arrive here as key-ups.
    RefreshBoundKeystrokesLocked();
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
  }
  if (attached_window_) {
    ReleaseMouseCaptureFromUIThread(attached_window_);
  }
}

void MnkInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  has_focus_ = true;
}

}  // namespace rex::input::mnk
