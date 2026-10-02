/**
 * @file        core/frame_pacing.cpp
 * @brief       Guest frame pacing controls. See graphics/frame_pacing.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/frame_pacing.h>

#include <atomic>

namespace rex::graphics {

namespace {
std::atomic<double> g_guest_vblank_rate{0.0};
}  // namespace

void SetGuestVblankRate(double hz) {
  g_guest_vblank_rate.store(hz > 0.0 ? hz : 0.0, std::memory_order_relaxed);
}

double GetGuestVblankRate() {
  return g_guest_vblank_rate.load(std::memory_order_relaxed);
}

}  // namespace rex::graphics
