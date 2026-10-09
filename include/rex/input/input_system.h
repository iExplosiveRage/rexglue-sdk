#pragma once
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

#include <array>
#include <atomic>
#include <bitset>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/state_merge.h>
#include <rex/system/interfaces/input.h>

namespace rex::ui {
class Window;
}

namespace rex::input {

class InputSystem : public system::IInputSystem {
 public:
  explicit InputSystem(rex::ui::Window* window);
  ~InputSystem() override;

  rex::ui::Window* window() const { return window_; }

  X_STATUS Setup() override;
  void Shutdown() override;

  void AddDriver(std::unique_ptr<InputDriver> driver);
  void AttachWindow(rex::ui::Window* window);
  void SetActiveCallback(std::function<bool()> callback);

  /// Replaces any previous assignment. Call before the guest starts polling.
  void SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment);

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags, X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);
  /// GetState for the emulator's own UI, which reads while the guest is blocked.
  X_RESULT GetStateForUI(uint32_t user_index, X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke);

  /// While any blocker is held the guest reads a neutral pad. Buttons still
  /// held when the last one drops stay masked until released, so the press
  /// that dismissed the dialog does not also reach the game.
  void AddUIInputBlocker();
  void RemoveUIInputBlocker();

  /// Calls `callback` on the polling thread when all of `buttons` (X_INPUT_GAMEPAD_*)
  /// get held on one controller, like Back + Start to open a menu. The guest
  /// doesn't see that press. Set it before the guest starts polling.
  void SetUIToggleCombo(uint16_t buttons, std::function<void()> callback);
  /// Changes the buttons of the combo, 0 to turn it off.
  void SetUIToggleComboButtons(uint16_t buttons);

  /// Debugging / automation: the guest sees `buttons` (X_INPUT_GAMEPAD_*)
  /// held on `user_index` for `duration_ms`, on top of the real controller -
  /// and the triggers pulled at least this far (0-255).
  void InjectButtons(uint32_t user_index, uint16_t buttons, uint32_t duration_ms,
                     uint8_t left_trigger = 0, uint8_t right_trigger = 0);

  bool GetVibrationEnabled() const;
  void ToggleVibration();

  std::bitset<kMaxGuestUsers> GetConnectedUsers() const { return connected_users_; }
  /// Guest user whose device most recently produced a button press.
  uint32_t GetLastUsedUser() const { return last_used_user_; }
  /// SDL_GamepadType (DeviceInfo::gamepad_type) of the controller `user_index` is
  /// using - the one most recently in the player's hands - or 0 when that is
  /// unknown, the keyboard or no controller. Any thread.
  uint8_t GetGamepadType(uint32_t user_index);

 private:
  using JoystickValue = std::pair<uint16_t, uint16_t>;
  /// Re-enumerates every driver and notifies the assignment when the set
  /// changed.
  void RefreshDevices();
  InputDriver* DriverForDevice(DeviceId id);
  const DeviceInfo* DeviceInfoFor(DeviceId id) const;
  /// The device that speaks for a user, preferring the one most recently in
  /// the player's hands.
  DeviceId ChooseDeviceForUser(uint32_t user_index) const;

  void UpdateConnectedUser(uint32_t user_index, bool connected);
  void AdjustDeadzoneLevels(uint32_t user_index, X_INPUT_GAMEPAD* gamepad) const;
  X_INPUT_VIBRATION ModifyVibrationLevel(const X_INPUT_VIBRATION* vibration) const;

  rex::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;

  std::unique_ptr<DeviceAssignment> assignment_;
  ActiveDeviceTracker active_devices_;

  // Ordered by ordinal. Ordinals are never recycled, so unplugging pad one
  // does not renumber pad two.
  std::vector<DeviceInfo> devices_;
  std::vector<InputDriver*> device_owners_;

  std::bitset<kMaxGuestUsers> connected_users_;
  uint32_t last_used_user_ = 0;
  // {left, right} stick maxima, scaling the deadzone percentages.
  std::array<std::pair<JoystickValue, JoystickValue>, kMaxGuestUsers> user_max_joystick_value_ = {};

  // The guest polls from its threads while the emulator's own UI reads the
  // controllers from the UI thread (dialogs).
  std::recursive_mutex mutex_;

  std::atomic<int> ui_input_blockers_{0};
  // Masked out per user until the guest sees them released.
  std::array<uint16_t, kMaxGuestUsers> consumed_buttons_ = {};

  std::atomic<uint16_t> ui_toggle_combo_{0};
  std::function<void()> ui_toggle_callback_;
  std::array<bool, kMaxGuestUsers> ui_toggle_combo_held_ = {};

  // InjectButtons: per user, the buttons and until when (steady clock ms).
  std::array<uint16_t, kMaxGuestUsers> injected_buttons_ = {};
  std::array<uint8_t, kMaxGuestUsers> injected_left_trigger_ = {};
  std::array<uint8_t, kMaxGuestUsers> injected_right_trigger_ = {};
  std::array<uint64_t, kMaxGuestUsers> injected_until_ms_ = {};
};

/// Create a default InputSystem with SDL + MnK + NOP drivers.
/// In tool mode, only the NOP driver is added.
std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode);

}  // namespace rex::input
