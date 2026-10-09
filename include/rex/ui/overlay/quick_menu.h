/**
 * @file        rex/ui/overlay/quick_menu.h
 *
 * @brief       Controller-friendly settings menu drawn over the game: a few
 *              hand-picked cvars per section, applied live and saved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>

namespace rex::ui {

// The controller whose button glyphs the overlay's hints show (A / B / Y, LB /
// RB), by physical position: kPlayStation shows Cross / Circle / Triangle and
// L1 / R1, kNintendo B / A / X and L / R (the bottom face button is Nintendo's
// B). Any thread; a game sets it from its own setting.
enum class ButtonGlyphs { kXbox, kPlayStation, kNintendo };
void SetButtonGlyphs(ButtonGlyphs glyphs);
ButtonGlyphs GetButtonGlyphs();

// A setting in the quick menu, backed by a cvar.
struct QuickMenuItem {
  enum class Kind {
    // Boolean cvar, shown as a switch.
    kToggle,
    // One of `choices`.
    kChoice,
    // A number from `min` to `max` in `step` increments.
    kNumber,
    // A key bind (the cvar of a RegisterBind): activating it waits for the
    // next key press and binds that key.
    kKey,
    // A command, not a setting (no cvar): A / Enter / a click runs `action`.
    // With `list` set, left / right first pick one of the values it returns
    // and `action` gets that value.
    kAction,
  };

  Kind kind = Kind::kToggle;
  std::string label;
  // Shown under the list while the item is selected.
  std::string help;
  std::string cvar;
  // Set to the same value as `cvar` (like draw_resolution_scale_y with _x).
  std::vector<std::string> mirrored_cvars;
  // kChoice: the cvar value and the text shown for it. Values the cvar doesn't
  // allow are left out.
  std::vector<std::pair<std::string, std::string>> choices;
  // kNumber. Shown as printf(format, value * display_scale + display_offset);
  // with a negative display_scale, right lowers the cvar value.
  double min = 0.0;
  double max = 1.0;
  double step = 1.0;
  double display_scale = 1.0;
  double display_offset = 0.0;
  std::string format = "%g";
  // Only shown while the cvar `shown_if_cvar` has one of `shown_if_values`.
  std::string shown_if_cvar;
  std::vector<std::string> shown_if_values;

  // kAction: runs on the UI thread after the paint; `value` is the picked
  // entry of `list` (empty without a list).
  std::function<void(const std::string& value)> action;
  // kAction: the text shown on the right without a list (like "Export").
  std::string action_label;
  // kAction: values to pick from and their text, read when the menu opens,
  // when the section is shown and after every action. Nothing to pick ->
  // `empty_text` is shown and A does nothing.
  std::function<std::vector<std::pair<std::string, std::string>>()> list;
  std::string empty_text = "None";
};

// A result line under the help of a section, e.g. what an action did.
struct QuickMenuStatus {
  std::string text;
  bool error = false;
};

struct QuickMenuSection {
  std::string title;
  std::vector<QuickMenuItem> items;
  // Read every frame while the section is shown; empty text = no line.
  std::function<QuickMenuStatus()> status;
};

struct QuickMenuConfig {
  std::string title = "SETTINGS";
  std::vector<QuickMenuSection> sections;
  // Y on the controller turns this boolean cvar on or off and closes the menu,
  // a shortcut to a mode like a free camera. Empty = none.
  std::string quick_toggle_label;
  std::string quick_toggle_cvar;
};

class QuickMenuDialog : public ImGuiDialog {
 public:
  struct PadState {
    uint16_t buttons = 0;  // X_INPUT_GAMEPAD_* bits.
    int16_t thumb_lx = 0;
    int16_t thumb_ly = 0;
  };

  struct Callbacks {
    // Reads the controllers. Called on the UI thread, outside of painting.
    std::function<PadState()> read_pad;
    // Runs a function on the UI thread after the current paint.
    std::function<void(std::function<void()>)> defer;
    // Asks the owner to destroy the menu (run through `defer`).
    std::function<void()> close;
    // After a setting changed, e.g. to save the config (run through `defer`).
    std::function<void()> changed;
  };

  // Text is drawn with overlay_text (its fonts must be added to the atlas).
  QuickMenuDialog(ImGuiDrawer* imgui_drawer, QuickMenuConfig config, Callbacks callbacks);
  ~QuickMenuDialog() override;

  // Whether a quick menu is open (it has the controllers then). Any thread.
  static bool IsOpen();

  // While a key setting waits for a key, takes every key press (so binds
  // don't fire) and returns true. UI thread, before the binds.
  static bool CaptureKey(const KeyEvent& e);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  struct SharedPad;

  bool IsShown(const QuickMenuItem& item) const;
  std::vector<size_t> ShownItems(const QuickMenuSection& section) const;
  // direction: -1 left, 1 right, 0 activate (toggle / next).
  void Change(const QuickMenuItem& item, int direction);
  void Set(const QuickMenuItem& item, const std::string& value);
  void RequestClose();
  void StartCapture(const QuickMenuItem& item);
  void FinishCapture(VirtualKey key);
  void StopCapture();
  // Reads the lists of the current section's action items.
  void RefreshLists();

  QuickMenuConfig config_;
  // kAction lists of the current section by item index, and the picked entry.
  std::vector<std::vector<std::pair<std::string, std::string>>> lists_;
  std::vector<size_t> list_picks_;
  bool lists_stale_ = true;
  Callbacks callbacks_;
  std::shared_ptr<SharedPad> pad_;

  size_t section_ = 0;
  size_t selected_item_ = 0;
  bool close_requested_ = false;

  bool pad_seen_ = false;
  uint16_t last_buttons_ = 0;
  // Pressed to close the menu, which happens when they're released.
  uint16_t close_buttons_ = 0;
  // Y was pressed: toggle quick_toggle_cvar when closing.
  bool quick_toggle_ = false;
  int repeat_direction_ = 0;
  double repeat_time_ = 0.0;
  double open_time_ = -1.0;

  // The key setting waiting for a key, the key pressed meanwhile, and why the
  // last one wasn't taken.
  const QuickMenuItem* capturing_ = nullptr;
  VirtualKey captured_key_ = VirtualKey::kNone;
  std::string capture_message_;
  // Menu keys are ignored briefly after a key was bound (the same press).
  double keyboard_quiet_until_ = 0.0;

  uint64_t fps_swaps_ = 0;
  double fps_time_ = -1.0;
  float fps_ = 0.0f;
};

}  // namespace rex::ui
