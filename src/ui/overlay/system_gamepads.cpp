/**
 * @file        ui/overlay/system_gamepads.cpp
 *
 * @brief       The controllers read straight from SDL, for UI shown before the
 *              game's input system runs (a start screen, its settings menu):
 *              Xbox, PlayStation, Switch and other SDL-known pads alike.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <cstdlib>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/logging.h>
#include <rex/ui/overlay/quick_menu.h>

namespace rex::ui {

namespace {

// X_INPUT_GAMEPAD_* bits, by SDL's positional buttons (south = the bottom face
// button: A on Xbox, Cross on PlayStation, B on Switch).
struct Mapping {
  SDL_GamepadButton button;
  uint16_t bit;
};
constexpr Mapping kMappings[] = {
    {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004},     {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
    {SDL_GAMEPAD_BUTTON_START, 0x0010},         {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040},    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
    {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000},         {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
    {SDL_GAMEPAD_BUTTON_WEST, 0x4000},          {SDL_GAMEPAD_BUTTON_NORTH, 0x8000},
};

std::atomic<int> g_style{-1};

int StyleOf(SDL_Gamepad* pad) {
  switch (SDL_GetGamepadType(pad)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
      return 1;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
      return 2;
    default:
      return 0;
  }
}

int16_t Axis(SDL_Gamepad* pad, SDL_GamepadAxis axis, bool invert) {
  int value = SDL_GetGamepadAxis(pad, axis);
  if (invert) {
    value = -value - 1;  // SDL's Y points down, XInput's up
  }
  return int16_t(value < -32768 ? -32768 : value > 32767 ? 32767 : value);
}

}  // namespace

QuickMenuDialog::PadState ReadGamepadsBeforeGame() {
  QuickMenuDialog::PadState state;
  static const bool initialized = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  if (!initialized) {
    return state;
  }
  // Opened once seen (and kept: the game's own input opens them too later).
  static std::vector<SDL_Gamepad*> pads;
  static uint64_t last_scan = 0;
  const uint64_t now = SDL_GetTicks();
  if (pads.empty() || now - last_scan > 1000) {
    last_scan = now;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
      for (int i = 0; i < count; ++i) {
        if (!SDL_GetGamepadFromID(ids[i])) {
          if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i])) {
            const char* name = SDL_GetGamepadName(pad);
            REXLOG_INFO("UI controller: {}", name ? name : "(unnamed)");
            if (g_style.load() < 0) {
              g_style.store(StyleOf(pad));
            }
            pads.push_back(pad);
          }
        }
      }
      SDL_free(ids);
    }
  }
  SDL_UpdateGamepads();
  int stick_distance = 0;
  for (SDL_Gamepad* pad : pads) {
    if (!SDL_GamepadConnected(pad)) {
      continue;
    }
    bool pressed = false;
    for (const Mapping& mapping : kMappings) {
      if (SDL_GetGamepadButton(pad, mapping.button)) {
        state.buttons |= mapping.bit;
        pressed = true;
      }
    }
    if (pressed) {
      g_style.store(StyleOf(pad));
    }
    const int16_t lx = Axis(pad, SDL_GAMEPAD_AXIS_LEFTX, false);
    const int16_t ly = Axis(pad, SDL_GAMEPAD_AXIS_LEFTY, true);
    const int distance = std::abs(int(lx)) + std::abs(int(ly));
    if (distance > stick_distance) {
      stick_distance = distance;
      state.thumb_lx = lx;
      state.thumb_ly = ly;
    }
  }
  return state;
}

int GamepadStyleBeforeGame() {
  return g_style.load();
}

}  // namespace rex::ui
