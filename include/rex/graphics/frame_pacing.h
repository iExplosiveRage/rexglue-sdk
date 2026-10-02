/**
 * @file        graphics/frame_pacing.h
 * @brief       Guest frame pacing controls for games (e.g. a frame rate option).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

namespace rex::graphics {

// Rate of the guest's vertical blank interrupts in Hz, for games that pace
// their frames by it: 0 = as configured (the video mode's refresh with vsync
// on, 1000 with it off). Thread-safe, applies live.
void SetGuestVblankRate(double hz);
double GetGuestVblankRate();

}  // namespace rex::graphics
