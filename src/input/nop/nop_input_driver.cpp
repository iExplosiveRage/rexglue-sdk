/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <string>

#include <rex/input/flags.h>
#include <rex/cvar.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/logging.h>

REXCVAR_DEFINE_UINT32(input_test_pads, 0, "Input",
                      "Debug: number of idle virtual controllers that count as real pads (pad 1 = "
                      "guest user 0, pad 2 = user 1...), for automated tests of multi-pad setups "
                      "with pad_press <buttons> <ms> <user>")
    .range(0, 4);

namespace rex::input::nop {

namespace {

// A single device, so its handle is a constant.
constexpr rex::input::DeviceId kNopDevice = static_cast<rex::input::DeviceId>(0x4E4F5000);
// input_test_pads devices: kNopDevice + 1 + n.
constexpr uint32_t kMaxTestPads = 4;

bool IsNopDevice(rex::input::DeviceId id) {
  const uint64_t value = static_cast<uint64_t>(id);
  const uint64_t base = static_cast<uint64_t>(kNopDevice);
  return value >= base && value <= base + std::min<uint32_t>(REXCVAR_GET(input_test_pads), kMaxTestPads);
}

}  // namespace

NopInputDriver::NopInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

NopInputDriver::~NopInputDriver() = default;

X_STATUS NopInputDriver::Setup() {
  return X_STATUS_SUCCESS;
}

// Spoof a connected controller for user 0 so games don't pause
// waiting for input. Returns idle state (no buttons pressed).

void NopInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  DeviceInfo info;
  info.id = kNopDevice;
  info.name = "None";
  info.synthetic = true;
  out.push_back(info);

  const uint32_t test_pads = std::min<uint32_t>(REXCVAR_GET(input_test_pads), kMaxTestPads);
  for (uint32_t n = 0; n < test_pads; ++n) {
    DeviceInfo pad;
    pad.id = static_cast<rex::input::DeviceId>(static_cast<uint32_t>(kNopDevice) + 1 + n);
    pad.name = "Test pad " + std::to_string(n + 1);
    pad.synthetic = false;
    out.push_back(pad);
  }
}

X_RESULT NopInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                               X_INPUT_CAPABILITIES* out_caps) {
  if (!IsNopDevice(id)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->flags = 0;
    // Report standard gamepad capabilities
    out_caps->gamepad.buttons = 0xFFFF;  // All buttons supported
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

X_RESULT NopInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (!IsNopDevice(id)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
    // Return idle controller state (no buttons pressed)
  }
  return X_ERROR_SUCCESS;
}

X_RESULT NopInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  if (!IsNopDevice(id)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // Accept vibration but do nothing
  return X_ERROR_SUCCESS;
}

X_RESULT NopInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                            X_INPUT_KEYSTROKE* out_keystroke) {
  if (!IsNopDevice(id)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // No keystrokes available
  return X_ERROR_EMPTY;
}

}  // namespace rex::input::nop
