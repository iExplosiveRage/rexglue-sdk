/**
 * @file        graphics/frame_pacing.h
 * @brief       Guest frame pacing controls for games (e.g. a frame rate option).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <chrono>
#include <cstdint>

namespace rex::graphics {

// SetGuestVblankRate value: the guest vblank follows the refresh rate of the
// display the game is shown on (60 Hz until it's known).
constexpr double kGuestVblankRateDisplay = -1.0;

// Rate of the guest's vertical blank interrupts in Hz, for games that pace
// their frames by it: 0 = as configured (the video mode's refresh with vsync
// on, 1000 with it off), kGuestVblankRateDisplay = the display's refresh rate.
// Thread-safe, applies live.
// With host vsync on (present_vsync), a rate that is the display's refresh
// rate or a whole fraction of it (60 on a 60, 120 or 240 Hz display, 144 on a
// 144 Hz one, within 1.5%) is locked to the display's own vblanks, so every
// frame stays on screen for the same number of refreshes and the guest never
// drifts against the display. Not while an online game session is open (its
// timing stays as it was).
void SetGuestVblankRate(double hz);
double GetGuestVblankRate();

// Host display vblanks (the monitor the presenter shows the game on), for
// locking the guest vblank to the display.
struct HostVblankInfo {
  uint64_t count = 0;      // vblanks seen so far
  int64_t time_ns = 0;     // steady clock time of the latest one
  double period_ns = 0.0;  // measured refresh period, 0 = unknown yet
  bool stable = false;     // the period is steady (a fixed refresh rate)
};
// Called by the presenter after each vblank of its monitor, with the refresh
// rate of the monitor's display mode (0 if unknown).
void NotifyHostVblank(double nominal_refresh_hz);
// Forgets the measured period (the presenter moved to another monitor or lost
// it).
void ResetHostVblank();
HostVblankInfo GetHostVblank();
// Waits for a host vblank after the one numbered `count`. Returns false on the
// timeout. `info` gets the latest state either way.
bool WaitForHostVblank(uint64_t count, std::chrono::nanoseconds timeout, HostVblankInfo& info);
// The display's measured refresh rate in Hz, 0 if not known.
double GetHostRefreshRate();

// Whether the presenter waits for vertical sync (present_vsync), set by the
// presenter.
void SetHostVsync(bool vsync);
bool IsHostVsync();

}  // namespace rex::graphics
