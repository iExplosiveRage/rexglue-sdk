/**
 * @file        rex/ui/ui_watchdog.h
 * @brief       UI thread watchdog: logs (and flushes the log) when the UI
 *              thread is stuck in one event for seconds, naming what it was
 *              doing - so a frozen, "not responding" window leaves a trace in
 *              the log even when the user has to kill the process.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

namespace rex::ui {

// What the UI thread is doing now (a string literal, or nullptr = nothing in
// particular). Only from the UI thread. Prefer UIThreadPhase.
const char* SetUIThreadPhase(const char* phase);

// The UI thread entered / left the handling of one event (the main loop).
void UIThreadBusyBegin();
void UIThreadBusyEnd();

// Starts the watchdog thread once (ui_watchdog cvar).
void StartUIWatchdog();

// Names a stretch of UI thread work for the watchdog, restoring the previous
// name at the end of the scope.
class UIThreadPhase {
 public:
  explicit UIThreadPhase(const char* phase) : previous_(SetUIThreadPhase(phase)) {}
  ~UIThreadPhase() { SetUIThreadPhase(previous_); }
  UIThreadPhase(const UIThreadPhase&) = delete;
  UIThreadPhase& operator=(const UIThreadPhase&) = delete;

 private:
  const char* previous_;
};

}  // namespace rex::ui
