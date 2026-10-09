/**
 * @file        ui/overlay/quick_menu.cpp
 *
 * @brief       Quick settings menu. See quick_menu.h for details.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/quick_menu.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string_view>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/frame_rate.h>
#include <rex/string/numeric.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/overlay_text.h>

namespace rex::ui {

namespace {

// X_INPUT_GAMEPAD_* (rex/input/input.h).
constexpr uint16_t kPadUp = 0x0001;
constexpr uint16_t kPadDown = 0x0002;
constexpr uint16_t kPadLeft = 0x0004;
constexpr uint16_t kPadRight = 0x0008;
constexpr uint16_t kPadStart = 0x0010;
constexpr uint16_t kPadBack = 0x0020;
constexpr uint16_t kPadLeftShoulder = 0x0100;
constexpr uint16_t kPadRightShoulder = 0x0200;
constexpr uint16_t kPadA = 0x1000;
constexpr uint16_t kPadB = 0x2000;
constexpr uint16_t kPadY = 0x8000;
constexpr int16_t kStickThreshold = 16000;

// Seconds before a held direction repeats, and between the repeats.
constexpr double kRepeatDelay = 0.4;
constexpr double kRepeatIntervalVertical = 0.09;
constexpr double kRepeatIntervalHorizontal = 0.12;

ImVec2 TextSize(float size, std::string_view text) {
  return overlay_text::Measure(size, text);
}
void DrawText(ImDrawList* draw_list, float size, ImVec2 position, ImU32 color,
              std::string_view text, float wrap_width = 0.0f) {
  overlay_text::Draw(draw_list, size, position, color, text, wrap_width);
}

std::atomic<int> g_button_glyphs{int(ButtonGlyphs::kXbox)};

// Face buttons by position (Xbox A, B, X, Y).
enum class Face { kBottom, kRight, kLeft, kTop };

// What the glyph of `face` is called in the hints' text.
const char* FaceName(Face face) {
  static constexpr const char* kNames[3][4] = {{"A", "B", "X", "Y"},
                                               {"Cross", "Circle", "Square", "Triangle"},
                                               {"B", "A", "Y", "X"}};
  return kNames[g_button_glyphs.load(std::memory_order_relaxed)][int(face)];
}

// LB or RB as the controller calls it.
const char* ShoulderName(bool right) {
  static constexpr const char* kNames[3][2] = {{"LB", "RB"}, {"L1", "R1"}, {"L", "R"}};
  return kNames[g_button_glyphs.load(std::memory_order_relaxed)][right ? 1 : 0];
}

// A face button's glyph: Xbox's colored letters, PlayStation's symbols,
// Nintendo's letters on a dark button.
void DrawFaceGlyph(ImDrawList* draw_list, ImVec2 center, float radius, Face face, float alpha) {
  auto color = [alpha](int r, int g, int b) { return IM_COL32(r, g, b, int(255.0f * alpha)); };
  const auto glyphs = ButtonGlyphs(g_button_glyphs.load(std::memory_order_relaxed));
  if (glyphs == ButtonGlyphs::kPlayStation) {
    draw_list->AddCircleFilled(center, radius, color(34, 38, 46), 32);
    draw_list->AddCircle(center, radius - 0.75f, color(92, 100, 114), 32, 1.5f);
    const float r = radius * 0.46f, thickness = std::max(1.5f, radius * 0.15f);
    switch (face) {
      case Face::kBottom:
        draw_list->AddLine(ImVec2(center.x - r, center.y - r), ImVec2(center.x + r, center.y + r),
                           color(124, 178, 236), thickness);
        draw_list->AddLine(ImVec2(center.x - r, center.y + r), ImVec2(center.x + r, center.y - r),
                           color(124, 178, 236), thickness);
        break;
      case Face::kRight:
        draw_list->AddCircle(center, r * 1.05f, color(238, 98, 108), 24, thickness);
        break;
      case Face::kLeft:
        draw_list->AddRect(ImVec2(center.x - r * 0.9f, center.y - r * 0.9f),
                           ImVec2(center.x + r * 0.9f, center.y + r * 0.9f), color(228, 138, 206),
                           0.0f, 0, thickness);
        break;
      case Face::kTop:
        draw_list->AddTriangle(ImVec2(center.x, center.y - r * 1.05f),
                               ImVec2(center.x + r * 1.05f, center.y + r * 0.75f),
                               ImVec2(center.x - r * 1.05f, center.y + r * 0.75f),
                               color(76, 208, 168), thickness);
        break;
    }
    return;
  }
  const char* letter = FaceName(face);
  if (glyphs == ButtonGlyphs::kNintendo) {
    draw_list->AddCircleFilled(center, radius, color(58, 60, 66), 32);
    draw_list->AddCircle(center, radius - 0.75f, color(116, 120, 128), 32, 1.5f);
  } else {
    static constexpr int kColors[4][3] = {
        {22, 150, 62}, {206, 44, 44}, {32, 108, 214}, {222, 168, 18}};
    const int* c = kColors[int(face)];
    draw_list->AddCircleFilled(center, radius, color(c[0], c[1], c[2]), 32);
  }
  const float letter_size = radius * 1.27f;
  const ImVec2 extent = TextSize(letter_size, letter);
  DrawText(draw_list, letter_size, ImVec2(center.x - extent.x * 0.5f, center.y - extent.y * 0.5f),
           color(255, 255, 255), letter);
}

// Colors faded in with the menu.
struct Palette {
  float alpha = 1.0f;
  ImU32 operator()(int r, int g, int b, int a = 255) const {
    return IM_COL32(r, g, b, int(float(a) * alpha));
  }
};

bool IsOn(const std::string& value) {
  return rex::string::from_string<bool>(value, false);
}

bool SameValue(const std::string& a, const std::string& b) {
  if (a == b) {
    return true;
  }
  double number_a, number_b;
  return rex::cvar::ParseDouble(a, number_a) && rex::cvar::ParseDouble(b, number_b) &&
         number_a == number_b;
}

using Choice = std::pair<std::string, std::string>;

// The item's choices that its cvar accepts.
std::vector<const Choice*> AllowedChoices(const QuickMenuItem& item) {
  std::vector<const Choice*> allowed;
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(item.cvar);
  if (!info) {
    return allowed;
  }
  const rex::cvar::Constraints& constraints = info->constraints;
  for (const Choice& choice : item.choices) {
    if (constraints.HasAllowedValues() &&
        std::find(constraints.allowed_values.begin(), constraints.allowed_values.end(),
                  choice.first) == constraints.allowed_values.end()) {
      continue;
    }
    double number;
    if (constraints.HasRangeConstraint() && rex::cvar::ParseDouble(choice.first, number) &&
        ((constraints.min && number < *constraints.min) ||
         (constraints.max && number > *constraints.max))) {
      continue;
    }
    allowed.push_back(&choice);
  }
  return allowed;
}

int ChoiceIndex(const std::vector<const Choice*>& choices, const std::string& value) {
  for (size_t i = 0; i < choices.size(); ++i) {
    if (SameValue(choices[i]->first, value)) {
      return int(i);
    }
  }
  return -1;
}

double NumberValue(const QuickMenuItem& item, const std::string& value) {
  double number = item.min;
  rex::cvar::ParseDouble(value, number);
  return number;
}

std::string FormatNumber(const QuickMenuItem& item, double value) {
  char text[64];
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
  std::snprintf(text, sizeof(text), item.format.c_str(),
                value * item.display_scale + item.display_offset);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  return text;
}

// The text of a number for the cvar, by its type.
std::string CvarNumber(const std::string& cvar, double value) {
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(cvar);
  if (info && info->type != rex::cvar::FlagType::Double) {
    return std::to_string(std::llround(value));
  }
  char text[32];
  std::snprintf(text, sizeof(text), "%.6g", value);
  return text;
}

// A key name for people: "Numpad0" -> "Numpad 0", "PageUp" -> "Page Up".
std::string KeyLabel(const std::string& name) {
  std::string label;
  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (i && name[i - 1] >= 'a' && name[i - 1] <= 'z' &&
        ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
      label += ' ';
    }
    label += c;
  }
  return label;
}

std::atomic<int> open_menus{0};
// UI thread only.
QuickMenuDialog* capturing_menu = nullptr;

}  // namespace

void SetButtonGlyphs(ButtonGlyphs glyphs) {
  g_button_glyphs.store(int(glyphs), std::memory_order_relaxed);
}

ButtonGlyphs GetButtonGlyphs() {
  return ButtonGlyphs(g_button_glyphs.load(std::memory_order_relaxed));
}

struct QuickMenuDialog::SharedPad {
  PadState state;
  bool valid = false;
  bool pending = false;
};

QuickMenuDialog::QuickMenuDialog(ImGuiDrawer* imgui_drawer, QuickMenuConfig config,
                                 Callbacks callbacks)
    : ImGuiDialog(imgui_drawer),
      config_(std::move(config)),
      callbacks_(std::move(callbacks)),
      pad_(std::make_shared<SharedPad>()) {
  if (!config_.sections.empty()) {
    std::vector<size_t> shown = ShownItems(config_.sections.front());
    if (!shown.empty()) {
      selected_item_ = shown.front();
    }
  }
  open_menus.fetch_add(1, std::memory_order_relaxed);
}

QuickMenuDialog::~QuickMenuDialog() {
  if (capturing_menu == this) {
    capturing_menu = nullptr;
  }
  open_menus.fetch_sub(1, std::memory_order_relaxed);
}

bool QuickMenuDialog::IsOpen() {
  return open_menus.load(std::memory_order_relaxed) > 0;
}

bool QuickMenuDialog::CaptureKey(const KeyEvent& e) {
  QuickMenuDialog* menu = capturing_menu;
  if (!menu || !menu->capturing_) {
    return false;
  }
  // A held key repeats; only the press counts.
  if (!e.prev_state()) {
    menu->captured_key_ = e.virtual_key();
  }
  return true;
}

void QuickMenuDialog::StartCapture(const QuickMenuItem& item) {
  capturing_ = &item;
  captured_key_ = VirtualKey::kNone;
  capture_message_.clear();
  capturing_menu = this;
}

void QuickMenuDialog::FinishCapture(VirtualKey key) {
  switch (key) {
    case VirtualKey::kShift:
    case VirtualKey::kLShift:
    case VirtualKey::kRShift:
    case VirtualKey::kControl:
    case VirtualKey::kLControl:
    case VirtualKey::kRControl:
    case VirtualKey::kMenu:
    case VirtualKey::kLMenu:
    case VirtualKey::kRMenu:
    case VirtualKey::kLWin:
    case VirtualKey::kRWin:
      // The start of a shortcut like Alt+Tab, not the key wanted.
      return;
    default:
      break;
  }
  const std::string name = VirtualKeyToString(key);
  if (name.empty()) {
    capture_message_ = "That key can't be used.";
    return;
  }
  switch (key) {
    case VirtualKey::kUp:
    case VirtualKey::kDown:
    case VirtualKey::kLeft:
    case VirtualKey::kRight:
    case VirtualKey::kPrior:
    case VirtualKey::kNext:
    case VirtualKey::kReturn:
      capture_message_ = KeyLabel(name) + " is used by this menu.";
      return;
    default:
      break;
  }
  // With keyboard play on, the keys of the emulated controller are taken too.
  if (rex::cvar::Query<bool>("mnk_mode")) {
    for (const std::string& flag : rex::cvar::ListFlagsByCategory("Input/Keybinds/Controller")) {
      const std::string keys = rex::cvar::GetFlagByName(flag);
      size_t start = 0;
      while (start <= keys.size()) {
        const size_t end = std::min(keys.find_first_of(",+", start), keys.size());
        if (end > start && ParseVirtualKey(std::string_view(keys).substr(start, end - start)) == key) {
          const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(flag);
          capture_message_ = KeyLabel(name) + " is used by keyboard play" +
                             (info && !info->description.empty()
                                  ? " (" + info->description + ")."
                                  : std::string("."));
          return;
        }
        start = end + 1;
      }
    }
  }
  const std::string owner = FindBindForKey(key);
  if (!owner.empty() && owner != capturing_->cvar) {
    capture_message_ = KeyLabel(name) + " is already used";
    const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(owner);
    if (info && !info->description.empty()) {
      capture_message_ += " (" + info->description + ")";
    }
    capture_message_ += ".";
    return;
  }
  if (name != rex::cvar::GetFlagByName(capturing_->cvar)) {
    Set(*capturing_, name);
  }
  StopCapture();
}

void QuickMenuDialog::StopCapture() {
  capturing_ = nullptr;
  captured_key_ = VirtualKey::kNone;
  capture_message_.clear();
  if (capturing_menu == this) {
    capturing_menu = nullptr;
  }
  keyboard_quiet_until_ = ImGui::GetTime() + 0.25;
}

bool QuickMenuDialog::IsShown(const QuickMenuItem& item) const {
  if (item.kind != QuickMenuItem::Kind::kAction && !rex::cvar::GetFlagInfo(item.cvar)) {
    return false;
  }
  if (item.kind == QuickMenuItem::Kind::kChoice && AllowedChoices(item).empty()) {
    return false;
  }
  if (!item.shown_if_cvar.empty()) {
    if (!rex::cvar::GetFlagInfo(item.shown_if_cvar)) {
      return false;
    }
    const std::string value = rex::cvar::GetFlagByName(item.shown_if_cvar);
    return std::any_of(item.shown_if_values.begin(), item.shown_if_values.end(),
                       [&](const std::string& shown_value) { return SameValue(shown_value, value); });
  }
  return true;
}

std::vector<size_t> QuickMenuDialog::ShownItems(const QuickMenuSection& section) const {
  std::vector<size_t> shown;
  for (size_t i = 0; i < section.items.size(); ++i) {
    if (IsShown(section.items[i])) {
      shown.push_back(i);
    }
  }
  return shown;
}

void QuickMenuDialog::Change(const QuickMenuItem& item, int direction) {
  const std::string current = rex::cvar::GetFlagByName(item.cvar);
  switch (item.kind) {
    case QuickMenuItem::Kind::kToggle: {
      const bool on = IsOn(current);
      const bool target = direction ? direction > 0 : !on;
      if (target != on) {
        Set(item, target ? "true" : "false");
      }
    } break;
    case QuickMenuItem::Kind::kChoice: {
      const std::vector<const Choice*> choices = AllowedChoices(item);
      if (choices.empty()) {
        break;
      }
      const int count = int(choices.size());
      const int index = ChoiceIndex(choices, current);
      int target;
      if (!direction) {
        target = (index + 1) % count;
      } else if (index < 0) {
        target = direction > 0 ? 0 : count - 1;
      } else {
        target = std::clamp(index + direction, 0, count - 1);
      }
      if (target != index) {
        Set(item, choices[target]->first);
      }
    } break;
    case QuickMenuItem::Kind::kNumber: {
      const double value = NumberValue(item, current);
      // Right raises what's shown.
      const double step = item.display_scale < 0.0 ? -item.step : item.step;
      double target = value + (direction ? step * direction : step);
      if (!direction && (target > item.max + 1e-9 || target < item.min - 1e-9)) {
        // Activating past the end starts over.
        target = step > 0.0 ? item.min : item.max;
      }
      if (item.step > 0.0) {
        target = item.min + std::round((target - item.min) / item.step) * item.step;
      }
      target = std::clamp(target, item.min, item.max);
      if (std::abs(target - value) > 1e-9) {
        Set(item, CvarNumber(item.cvar, target));
      }
    } break;
    case QuickMenuItem::Kind::kKey:
      if (!direction) {
        StartCapture(item);
      }
      break;
    case QuickMenuItem::Kind::kAction: {
      const QuickMenuSection& section = config_.sections[section_];
      if (section.items.empty() || &item < &section.items.front() ||
          &item > &section.items.back()) {
        break;
      }
      const size_t index = size_t(&item - &section.items.front());
      std::string value;
      if (item.list) {
        if (index >= lists_.size() || lists_[index].empty()) {
          break;
        }
        const size_t count = lists_[index].size();
        size_t& pick = list_picks_[index];
        pick = std::min(pick, count - 1);
        if (direction) {
          pick = size_t(std::clamp(int(pick) + direction, 0, int(count) - 1));
          break;
        }
        value = lists_[index][pick].first;
      } else if (direction) {
        break;
      }
      if (item.action && callbacks_.defer) {
        callbacks_.defer([action = item.action, value]() { action(value); });
        // The lists may change (a new file): read again on the next paint,
        // which comes after the deferred action.
        lists_stale_ = true;
      }
    } break;
  }
}

void QuickMenuDialog::RefreshLists() {
  lists_stale_ = false;
  const QuickMenuSection& section = config_.sections[section_];
  std::vector<std::vector<std::pair<std::string, std::string>>> lists(section.items.size());
  std::vector<size_t> picks(section.items.size(), 0);
  for (size_t i = 0; i < section.items.size(); ++i) {
    const QuickMenuItem& item = section.items[i];
    if (item.kind != QuickMenuItem::Kind::kAction || !item.list) {
      continue;
    }
    lists[i] = item.list();
    // Keep the picked entry when it's still there.
    if (i < lists_.size() && i < list_picks_.size() && list_picks_[i] < lists_[i].size()) {
      const std::string& picked = lists_[i][list_picks_[i]].first;
      for (size_t j = 0; j < lists[i].size(); ++j) {
        if (lists[i][j].first == picked) {
          picks[i] = j;
          break;
        }
      }
    }
  }
  lists_ = std::move(lists);
  list_picks_ = std::move(picks);
}

void QuickMenuDialog::Set(const QuickMenuItem& item, const std::string& value) {
  std::vector<std::string> names{item.cvar};
  for (const std::string& name : item.mirrored_cvars) {
    if (rex::cvar::GetFlagInfo(name)) {
      names.push_back(name);
    }
  }
  // Not while painting: change callbacks may touch the presenter or window.
  auto apply = [names = std::move(names), value, changed = callbacks_.changed]() {
    for (const std::string& name : names) {
      rex::cvar::SetFlagByName(name, value);
    }
    REXLOG_INFO("Settings menu: {} = {}", names.front(), value);
    if (changed) {
      changed();
    }
  };
  if (callbacks_.defer) {
    callbacks_.defer(std::move(apply));
  }
}

void QuickMenuDialog::RequestClose() {
  if (close_requested_ || !callbacks_.close || !callbacks_.defer) {
    return;
  }
  close_requested_ = true;
  callbacks_.defer(callbacks_.close);
}

void QuickMenuDialog::OnDraw(ImGuiIO& io) {
  const double now = ImGui::GetTime();
  if (open_time_ < 0.0) {
    open_time_ = now;
  }
  if (config_.sections.empty()) {
    RequestClose();
    return;
  }

  // The controllers are read after the paint (reading them may pump window
  // events, which mustn't happen in the middle of it), for the next frame.
  if (callbacks_.read_pad && callbacks_.defer && !pad_->pending) {
    pad_->pending = true;
    callbacks_.defer([pad = pad_, read_pad = callbacks_.read_pad]() {
      pad->state = read_pad();
      pad->valid = true;
      pad->pending = false;
    });
  }

  const uint64_t swaps = rex::perf::GetGuestSwapCount();
  if (fps_time_ < 0.0) {
    fps_time_ = now;
    fps_swaps_ = swaps;
  } else if (now - fps_time_ >= 0.5) {
    fps_ = float(double(swaps - fps_swaps_) / (now - fps_time_));
    fps_time_ = now;
    fps_swaps_ = swaps;
  }

  // Input.
  int move = 0;
  int change = 0;
  int section_step = 0;
  bool activate = false;
  bool close = false;

  // A key setting waiting for its key (from CaptureKey) has the keyboard, the
  // controller and the mouse until then: Esc, B or a click cancels.
  const bool capturing = capturing_ != nullptr;
  bool cancel_capture = false;
  if (capturing && captured_key_ != VirtualKey::kNone) {
    const VirtualKey key = captured_key_;
    captured_key_ = VirtualKey::kNone;
    if (key == VirtualKey::kEscape) {
      cancel_capture = true;
    } else {
      FinishCapture(key);
    }
  }

  if (!capturing && now >= keyboard_quiet_until_) {
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
      move = -1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
      move = 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
      change = -1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
      change = 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) {
      section_step = -1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) {
      section_step = 1;
    }
    // With keyboard play on, Enter is the Start button of the emulated pad
    // (Space is A).
    if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
         ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) &&
        !rex::cvar::Query<bool>("mnk_mode")) {
      activate = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      close = true;
    }
  }

  if (pad_->valid) {
    const PadState& pad = pad_->state;
    uint16_t buttons = pad.buttons;
    if (pad.thumb_ly > kStickThreshold) {
      buttons |= kPadUp;
    } else if (pad.thumb_ly < -kStickThreshold) {
      buttons |= kPadDown;
    }
    if (pad.thumb_lx > kStickThreshold) {
      buttons |= kPadRight;
    } else if (pad.thumb_lx < -kStickThreshold) {
      buttons |= kPadLeft;
    }
    int direction = 0;
    if (buttons & kPadUp) {
      direction = 1;
    } else if (buttons & kPadDown) {
      direction = 2;
    } else if (buttons & kPadLeft) {
      direction = 3;
    } else if (buttons & kPadRight) {
      direction = 4;
    }
    if (!pad_seen_ || (!capturing && now < keyboard_quiet_until_)) {
      // Whatever is held as the menu opens (like the buttons that opened it)
      // is not a press in the menu - nor the key just bound, which keyboard
      // play turns into a controller button a frame later.
      pad_seen_ = true;
      last_buttons_ = buttons;
      repeat_direction_ = direction;
      repeat_time_ = DBL_MAX;
    }
    const uint16_t pressed = buttons & ~last_buttons_;
    last_buttons_ = buttons;
    if (pressed & kPadA) {
      activate = true;
    }
    if (pressed & kPadB) {
      if (capturing) {
        cancel_capture = true;
      } else {
        close_buttons_ |= kPadB;
      }
    }
    if (!capturing && (pressed & (kPadBack | kPadStart)) &&
        (buttons & (kPadBack | kPadStart)) == (kPadBack | kPadStart)) {
      close_buttons_ |= kPadBack | kPadStart;
    }
    if (!capturing && (pressed & kPadY) && !config_.quick_toggle_cvar.empty()) {
      close_buttons_ |= kPadY;
      quick_toggle_ = true;
    }
    // Closed once they're let go: the game takes the controllers back right
    // after, and a button still held then would reach it as a press.
    if (close_buttons_ && !(buttons & close_buttons_)) {
      close = true;
    }
    if (pressed & kPadLeftShoulder) {
      section_step = -1;
    }
    if (pressed & kPadRightShoulder) {
      section_step = 1;
    }
    bool fire = false;
    if (direction != repeat_direction_) {
      repeat_direction_ = direction;
      repeat_time_ = now + kRepeatDelay;
      fire = direction != 0;
    } else if (direction && now >= repeat_time_) {
      repeat_time_ = now + (direction <= 2 ? kRepeatIntervalVertical : kRepeatIntervalHorizontal);
      fire = true;
    }
    if (fire) {
      switch (direction) {
        case 1:
          move = -1;
          break;
        case 2:
          move = 1;
          break;
        case 3:
          change = -1;
          break;
        case 4:
          change = 1;
          break;
      }
    }
  }

  const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  if (capturing) {
    if (clicked) {
      cancel_capture = true;
    }
    move = change = section_step = 0;
    activate = close = false;
  }
  if (cancel_capture && capturing_) {
    StopCapture();
  }

  const int section_count = int(config_.sections.size());
  if (section_step) {
    section_ = size_t((int(section_) + section_step + section_count) % section_count);
    lists_stale_ = true;
    std::vector<size_t> shown = ShownItems(config_.sections[section_]);
    selected_item_ = shown.empty() ? 0 : shown.front();
  }
  section_ = std::min(section_, config_.sections.size() - 1);
  if (lists_stale_ || lists_.size() != config_.sections[section_].items.size()) {
    RefreshLists();
  }
  const QuickMenuSection& section = config_.sections[section_];
  const std::vector<size_t> rows = ShownItems(section);
  if (!rows.empty() && std::find(rows.begin(), rows.end(), selected_item_) == rows.end()) {
    // The selected item got hidden - go to the next one.
    auto next = std::lower_bound(rows.begin(), rows.end(), selected_item_);
    selected_item_ = next != rows.end() ? *next : rows.back();
  }
  if (move && !rows.empty()) {
    const int row_count = int(rows.size());
    const int row = int(std::find(rows.begin(), rows.end(), selected_item_) - rows.begin());
    selected_item_ = rows[size_t((row + move + row_count) % row_count)];
  }
  if (!rows.empty()) {
    const QuickMenuItem& item = section.items[selected_item_];
    if (change) {
      Change(item, change);
    } else if (activate) {
      Change(item, 0);
    }
  }
  if (close) {
    if (quick_toggle_ && !close_requested_) {
      QuickMenuItem quick_toggle;
      quick_toggle.cvar = config_.quick_toggle_cvar;
      if (rex::cvar::GetFlagInfo(quick_toggle.cvar)) {
        Change(quick_toggle, 0);
      }
    }
    quick_toggle_ = false;
    RequestClose();
  }

  // Layout, in 1080p units scaled to the display.
  const ImVec2 display = io.DisplaySize;
  // Room for the longest section, so the panel doesn't jump between sections.
  // Items sharing a label are alternatives shown one at a time.
  size_t max_rows = 1;
  for (const QuickMenuSection& menu_section : config_.sections) {
    std::vector<std::string_view> labels;
    for (const QuickMenuItem& menu_item : menu_section.items) {
      if (std::find(labels.begin(), labels.end(), menu_item.label) == labels.end()) {
        labels.push_back(menu_item.label);
      }
    }
    max_rows = std::max(max_rows, labels.size());
  }
  const float kTitleHeight = 60.0f, kTabsHeight = 54.0f, kRowHeight = 58.0f, kHelpHeight = 96.0f,
              kHintsHeight = 58.0f, kPadding = 30.0f;
  const float base_height = kPadding + kTitleHeight + kTabsHeight + 14.0f +
                            float(max_rows) * kRowHeight + 16.0f + kHelpHeight + kHintsHeight;
  float u = std::max(0.4f, std::min(display.x / 1920.0f, display.y / 1080.0f));
  u = std::min(u, display.y * 0.94f / base_height);

  const float padding = kPadding * u;
  const float panel_width = std::min(780.0f * u, display.x - 2.0f * padding);
  const float panel_height = base_height * u;
  // Left of the center, so the game stays visible while trying the settings.
  const ImVec2 panel_min(std::max(padding, display.x * 0.07f),
                         std::floor((display.y - panel_height) * 0.5f));
  const ImVec2 panel_max(panel_min.x + panel_width, panel_min.y + panel_height);
  const float content_left = panel_min.x + padding;
  const float content_right = panel_max.x - padding;

  const Palette color{float(std::clamp((now - open_time_) / 0.15, 0.0, 1.0))};

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(display);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const bool window_open = ImGui::Begin(
      "##rex_quick_menu", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav |
          ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar(2);
  if (!window_open) {
    ImGui::End();
    return;
  }
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  const bool mouse_moved = !capturing && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f);
  const bool row_clicked = clicked && !capturing;

  // Panel.
  draw_list->AddRectFilled(ImVec2(panel_min.x + 8.0f * u, panel_min.y + 10.0f * u),
                           ImVec2(panel_max.x + 8.0f * u, panel_max.y + 10.0f * u),
                           color(0, 0, 0, 90), 22.0f * u);
  draw_list->AddRectFilled(panel_min, panel_max, color(13, 17, 26, 236), 20.0f * u);
  draw_list->AddRect(panel_min, panel_max, color(255, 255, 255, 46), 20.0f * u, 0, 2.0f * u);

  // Title and what the game renders at.
  float y = panel_min.y + padding;
  DrawText(draw_list, 42.0f * u, ImVec2(content_left, y - 4.0f * u), color(255, 206, 38),
           config_.title);
  {
    std::string status;
    const rex::perf::RenderInfo info = rex::perf::GetRenderInfo();
    if (info.frontbuffer_width && info.scale_x && info.scale_y) {
      const uint32_t render_width = info.frontbuffer_width * info.scale_x;
      const uint32_t render_height = info.frontbuffer_height * info.scale_y;
      const uint32_t output_width =
          info.frontbuffer_width * std::max(info.requested_scale_x, info.scale_x);
      const uint32_t output_height =
          info.frontbuffer_height * std::max(info.requested_scale_y, info.scale_y);
      char text[96];
      if (render_width != output_width || render_height != output_height) {
        std::snprintf(text, sizeof(text), "%ux%u \xC2\xBB %ux%u", render_width, render_height,
                      output_width, output_height);
      } else {
        std::snprintf(text, sizeof(text), "%ux%u", render_width, render_height);
      }
      status = text;
    }
    if (fps_ > 0.0f) {
      char text[32];
      std::snprintf(text, sizeof(text), "%s%.0f FPS", status.empty() ? "" : "   ", fps_);
      status += text;
    }
    if (!status.empty()) {
      const float size = 22.0f * u;
      DrawText(draw_list, size,
               ImVec2(content_right - TextSize(size, status).x, y + 12.0f * u),
               color(160, 172, 188), status);
    }
  }
  y += kTitleHeight * u;

  // Section tabs, LB / RB.
  {
    const float size = 24.0f * u;
    const float chip_size = 18.0f * u;
    auto draw_chip = [&](float x, std::string_view text) {
      const ImVec2 text_size = TextSize(chip_size, text);
      const ImVec2 chip_min(x, y + 6.0f * u);
      const ImVec2 chip_max(x + text_size.x + 16.0f * u, y + 36.0f * u);
      draw_list->AddRectFilled(chip_min, chip_max, color(58, 66, 82), 6.0f * u);
      DrawText(draw_list, chip_size,
               ImVec2(chip_min.x + 8.0f * u, chip_min.y + (chip_max.y - chip_min.y - text_size.y) * 0.5f),
               color(225, 230, 238), text);
      return chip_max.x;
    };
    float x = draw_chip(content_left, ShoulderName(false)) + 18.0f * u;
    for (int i = 0; i < section_count; ++i) {
      const std::string& title = config_.sections[size_t(i)].title;
      const ImVec2 text_size = TextSize(size, title);
      const ImVec2 tab_min(x - 6.0f * u, y);
      const ImVec2 tab_max(x + text_size.x + 6.0f * u, y + 44.0f * u);
      const bool active = size_t(i) == section_;
      DrawText(draw_list, size, ImVec2(x, y + 6.0f * u),
               active ? color(255, 255, 255) : color(140, 150, 166), title);
      if (active) {
        draw_list->AddRectFilled(ImVec2(x, y + 40.0f * u), ImVec2(x + text_size.x, y + 44.0f * u),
                                 color(0, 196, 206), 2.0f * u);
      }
      if (row_clicked && ImGui::IsMouseHoveringRect(tab_min, tab_max) && !active) {
        section_ = size_t(i);
        lists_stale_ = true;
        std::vector<size_t> shown = ShownItems(config_.sections[section_]);
        selected_item_ = shown.empty() ? 0 : shown.front();
      }
      x += text_size.x + 28.0f * u;
    }
    draw_chip(x - 10.0f * u, ShoulderName(true));
  }
  y += kTabsHeight * u;
  draw_list->AddLine(ImVec2(content_left, y), ImVec2(content_right, y), color(255, 255, 255, 28),
                     1.5f * u);
  y += 14.0f * u;

  // Settings.
  const float row_height = kRowHeight * u;
  const float label_size = 29.0f * u;
  const float value_size = 27.0f * u;
  for (size_t row = 0; row < rows.size(); ++row) {
    const size_t item_index = rows[row];
    const QuickMenuItem& item = section.items[item_index];
    const ImVec2 row_min(panel_min.x + padding * 0.45f, y + float(row) * row_height);
    const ImVec2 row_max(panel_max.x - padding * 0.45f, row_min.y + row_height - 6.0f * u);
    const float center_y = (row_min.y + row_max.y) * 0.5f;
    const bool hovered = ImGui::IsMouseHoveringRect(row_min, row_max);
    if (hovered && mouse_moved) {
      selected_item_ = item_index;
    }
    const bool selected = item_index == selected_item_;
    if (selected) {
      draw_list->AddRectFilled(row_min, row_max, color(0, 128, 146, 226), 10.0f * u);
      draw_list->AddRectFilled(row_min, ImVec2(row_min.x + 6.0f * u, row_max.y),
                               color(255, 206, 38), 10.0f * u, ImDrawFlags_RoundCornersLeft);
    }

    // Label.
    const ImVec2 label_extent = TextSize(label_size, item.label);
    const float label_x = row_min.x + 30.0f * u;
    DrawText(draw_list, label_size, ImVec2(label_x, center_y - label_extent.y * 0.5f),
             selected ? color(255, 255, 255) : color(222, 228, 236), item.label);
    const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(item.cvar);
    if (info && info->lifecycle == rex::cvar::Lifecycle::kRequiresRestart) {
      const float badge_size = 17.0f * u;
      DrawText(draw_list, badge_size,
               ImVec2(label_x + label_extent.x + 12.0f * u,
                      center_y - TextSize(badge_size, "RESTART").y * 0.5f),
               color(255, 160, 60), "RESTART");
    }

    // Value.
    const std::string current = rex::cvar::GetFlagByName(item.cvar);
    const float right = row_max.x - 22.0f * u;
    int click_direction = 0;
    if (item.kind == QuickMenuItem::Kind::kToggle) {
      const bool on = IsOn(current);
      const float switch_width = 72.0f * u, switch_height = 34.0f * u;
      const ImVec2 track_min(right - switch_width, center_y - switch_height * 0.5f);
      const ImVec2 track_max(right, center_y + switch_height * 0.5f);
      draw_list->AddRectFilled(track_min, track_max, on ? color(0, 200, 160) : color(66, 74, 90),
                               switch_height * 0.5f);
      const float knob_radius = switch_height * 0.5f - 4.0f * u;
      draw_list->AddCircleFilled(
          ImVec2(on ? track_max.x - switch_height * 0.5f : track_min.x + switch_height * 0.5f,
                 center_y),
          knob_radius, color(245, 247, 250), 24);
      const char* state_text = on ? "ON" : "OFF";
      const float state_size = 21.0f * u;
      const ImVec2 state_extent = TextSize(state_size, state_text);
      DrawText(draw_list, state_size,
               ImVec2(track_min.x - 12.0f * u - state_extent.x, center_y - state_extent.y * 0.5f),
               selected ? color(255, 255, 255) : color(150, 160, 175), state_text);
    } else if (item.kind == QuickMenuItem::Kind::kKey) {
      // The key on a key cap, pulsing while it waits for a key.
      const bool waiting = capturing_ == &item;
      const std::string key_text =
          waiting ? "Press a key" : (current.empty() ? "None" : KeyLabel(current));
      const ImVec2 key_extent = TextSize(value_size, key_text);
      const ImVec2 cap_min(right - key_extent.x - 32.0f * u, center_y - 19.0f * u);
      const ImVec2 cap_max(right, center_y + 19.0f * u);
      if (waiting) {
        const float pulse = 0.5f + 0.5f * float(std::sin(now * 6.0));
        draw_list->AddRectFilled(cap_min, cap_max, color(255, 206, 38, 70 + int(110.0f * pulse)),
                                 8.0f * u);
      } else {
        draw_list->AddRectFilled(cap_min, cap_max, color(46, 54, 70), 8.0f * u);
      }
      draw_list->AddRect(cap_min, cap_max, color(255, 255, 255, 70), 8.0f * u, 0, 1.5f * u);
      DrawText(draw_list, value_size, ImVec2(cap_min.x + 16.0f * u, center_y - key_extent.y * 0.5f),
               waiting || selected ? color(255, 255, 255) : color(132, 222, 232), key_text);
    } else if (item.kind == QuickMenuItem::Kind::kAction && !item.list) {
      // A button.
      const std::string& text = item.action_label.empty() ? item.label : item.action_label;
      const ImVec2 text_extent = TextSize(value_size, text);
      const ImVec2 cap_min(right - text_extent.x - 40.0f * u, center_y - 19.0f * u);
      const ImVec2 cap_max(right, center_y + 19.0f * u);
      draw_list->AddRectFilled(cap_min, cap_max,
                               selected ? color(255, 206, 38, 200) : color(46, 54, 70), 8.0f * u);
      draw_list->AddRect(cap_min, cap_max, color(255, 255, 255, 70), 8.0f * u, 0, 1.5f * u);
      DrawText(draw_list, value_size, ImVec2(cap_min.x + 20.0f * u, center_y - text_extent.y * 0.5f),
               selected ? color(20, 24, 32) : color(132, 222, 232), text);
    } else {
      std::string value_text;
      bool can_lower = true, can_raise = true;
      if (item.kind == QuickMenuItem::Kind::kAction) {
        const std::vector<std::pair<std::string, std::string>>* list =
            item_index < lists_.size() ? &lists_[item_index] : nullptr;
        if (!list || list->empty()) {
          value_text = item.empty_text;
          can_lower = can_raise = false;
        } else {
          const size_t pick = std::min(list_picks_[item_index], list->size() - 1);
          value_text = (*list)[pick].second;
          can_lower = pick > 0;
          can_raise = pick + 1 < list->size();
        }
      } else if (item.kind == QuickMenuItem::Kind::kChoice) {
        const std::vector<const Choice*> choices = AllowedChoices(item);
        const int index = ChoiceIndex(choices, current);
        value_text = index >= 0 ? choices[size_t(index)]->second : current;
        if (index >= 0) {
          can_lower = index > 0;
          can_raise = index + 1 < int(choices.size());
        }
      } else {
        const double value = NumberValue(item, current);
        value_text = FormatNumber(item, value);
        const bool reversed = item.display_scale < 0.0;
        const bool at_min = value <= item.min + 1e-9, at_max = value >= item.max - 1e-9;
        can_lower = reversed ? !at_max : !at_min;
        can_raise = reversed ? !at_min : !at_max;
      }
      const float arrow_width = 11.0f * u, arrow_height = 18.0f * u, gap = 14.0f * u;
      const ImVec2 value_extent = TextSize(value_size, value_text);
      const float right_arrow_x = right - arrow_width;
      const float value_x = right_arrow_x - gap - value_extent.x;
      const float left_arrow_x = value_x - gap - arrow_width;
      const ImU32 value_color = selected ? color(255, 255, 255) : color(132, 222, 232);
      auto arrow_color = [&](bool enabled) {
        return enabled ? value_color : color(255, 255, 255, 50);
      };
      draw_list->AddTriangleFilled(ImVec2(left_arrow_x, center_y),
                                   ImVec2(left_arrow_x + arrow_width, center_y - arrow_height * 0.5f),
                                   ImVec2(left_arrow_x + arrow_width, center_y + arrow_height * 0.5f),
                                   arrow_color(can_lower));
      draw_list->AddTriangleFilled(ImVec2(right_arrow_x + arrow_width, center_y),
                                   ImVec2(right_arrow_x, center_y + arrow_height * 0.5f),
                                   ImVec2(right_arrow_x, center_y - arrow_height * 0.5f),
                                   arrow_color(can_raise));
      DrawText(draw_list, value_size, ImVec2(value_x, center_y - value_extent.y * 0.5f), value_color,
               value_text);
      if (row_clicked && hovered) {
        const float mouse_x = io.MousePos.x;
        if (mouse_x >= left_arrow_x - gap && mouse_x < value_x) {
          click_direction = -1;
        } else if (mouse_x >= value_x + value_extent.x) {
          click_direction = 1;
        }
      }
    }
    if (row_clicked && hovered) {
      selected_item_ = item_index;
      Change(item, click_direction);
    }
  }
  y += float(max_rows) * row_height + 16.0f * u;

  // Help for the selected setting.
  draw_list->AddLine(ImVec2(content_left, y - 8.0f * u), ImVec2(content_right, y - 8.0f * u),
                     color(255, 255, 255, 28), 1.5f * u);
  if (capturing_) {
    const std::string text =
        capture_message_.empty()
            ? "Press the key to use for " + capturing_->label + ". Esc or " +
                  FaceName(Face::kRight) + " cancels."
            : capture_message_ + " Press another key, or Esc or " + FaceName(Face::kRight) +
                  " to cancel.";
    DrawText(draw_list, 23.0f * u, ImVec2(content_left, y + 6.0f * u),
             capture_message_.empty() ? color(255, 206, 38) : color(255, 140, 90), text,
             content_right - content_left);
  } else {
    // A section's status line (like what an action did) goes first.
    float help_y = y + 6.0f * u;
    const QuickMenuStatus status = section.status ? section.status() : QuickMenuStatus{};
    if (!status.text.empty()) {
      const float status_size = 23.0f * u;
      DrawText(draw_list, status_size, ImVec2(content_left, help_y),
               status.error ? color(255, 140, 90) : color(255, 206, 38), status.text,
               content_right - content_left);
      // Wrapped: about one line per width of text.
      const ImVec2 extent = TextSize(status_size, status.text);
      const float lines =
          std::max(1.0f, std::ceil(extent.x * 1.08f / std::max(1.0f, content_right - content_left)));
      help_y += extent.y * lines + 6.0f * u;
    }
    if (!rows.empty() && help_y < y + (kHelpHeight - 20.0f) * u) {
      DrawText(draw_list, 23.0f * u, ImVec2(content_left, help_y), color(196, 204, 216),
               section.items[selected_item_].help, content_right - content_left);
    }
  }

  // Button hints.
  {
    const float hint_size = 21.0f * u;
    const float center_y = panel_max.y - kHintsHeight * u * 0.5f - 6.0f * u;
    float x = content_left;
    auto draw_button = [&](Face face, const char* text) {
      const float radius = 15.0f * u;
      DrawFaceGlyph(draw_list, ImVec2(x + radius, center_y), radius, face, color.alpha);
      x += radius * 2.0f + 9.0f * u;
      DrawText(draw_list, hint_size, ImVec2(x, center_y - TextSize(hint_size, text).y * 0.5f),
               color(196, 204, 216), text);
      x += TextSize(hint_size, text).x + 26.0f * u;
    };
    draw_button(Face::kBottom, "Change");
    draw_button(Face::kRight, "Close");
    if (!config_.quick_toggle_label.empty() &&
        rex::cvar::GetFlagInfo(config_.quick_toggle_cvar)) {
      draw_button(Face::kTop, config_.quick_toggle_label.c_str());
    }
    const char* saved = "Saved automatically";
    DrawText(draw_list, hint_size,
             ImVec2(content_right - TextSize(hint_size, saved).x,
                    center_y - TextSize(hint_size, saved).y * 0.5f),
             color(120, 132, 150), saved);
  }

  ImGui::End();
}

}  // namespace rex::ui
