/**
 * @file        core/frame_pacing.cpp
 * @brief       Guest frame pacing controls. See graphics/frame_pacing.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/frame_pacing.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>

namespace rex::graphics {

namespace {
std::atomic<double> g_guest_vblank_rate{0.0};
std::atomic<bool> g_host_vsync{true};

std::mutex g_host_vblank_mutex;
std::condition_variable g_host_vblank_condition;
HostVblankInfo g_host_vblank;
// Running variance of the vblank intervals (ns^2) and how many fed it.
double g_host_vblank_variance = 0.0;
uint32_t g_host_vblank_samples = 0;

int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

void SetGuestVblankRate(double hz) {
  if (hz == kGuestVblankRateDisplay) {
    g_guest_vblank_rate.store(hz, std::memory_order_relaxed);
    return;
  }
  g_guest_vblank_rate.store(hz > 0.0 ? hz : 0.0, std::memory_order_relaxed);
}

double GetGuestVblankRate() {
  return g_guest_vblank_rate.load(std::memory_order_relaxed);
}

void NotifyHostVblank(double nominal_refresh_hz) {
  const int64_t now = SteadyNowNs();
  {
    std::lock_guard<std::mutex> lock(g_host_vblank_mutex);
    HostVblankInfo& v = g_host_vblank;
    if (v.time_ns) {
      const double interval = double(now - v.time_ns);
      // Up to 500 Hz, down to 20 Hz.
      if (interval >= 2.0e6 && interval <= 50.0e6) {
        if (v.period_ns <= 0.0) {
          v.period_ns = interval;
          g_host_vblank_variance = 0.0;
          g_host_vblank_samples = 1;
        } else if (interval < v.period_ns * 1.5 && interval > v.period_ns * 0.5) {
          // Exponential averages over ~32 vblanks: the waits wake a little
          // late at times, the average stays on the real period.
          const double delta = interval - v.period_ns;
          v.period_ns += delta / 32.0;
          g_host_vblank_variance += (delta * delta - g_host_vblank_variance) / 32.0;
          if (g_host_vblank_samples < 1000) {
            ++g_host_vblank_samples;
          }
        } else if (interval < v.period_ns * 0.5) {
          // Much shorter: a different (faster) refresh rate - start over.
          v.period_ns = interval;
          g_host_vblank_variance = 0.0;
          g_host_vblank_samples = 1;
        }
        // Longer ones are missed wakes (or a variable refresh rate) - they
        // only count toward the stability below through the variance.
      }
      // Steady: the intervals deviate by under 10% (the wakes are up to a few
      // hundred microseconds late on a busy system; a variable refresh rate
      // display waiting for frames deviates much more).
      // And near the refresh rate the display mode says, if known (with a
      // variable refresh rate the vblanks may follow the game's own frames -
      // locking the game to them would feed back on itself).
      bool nominal = true;
      if (nominal_refresh_hz > 0.0 && v.period_ns > 0.0) {
        const double measured_hz = 1.0e9 / v.period_ns;
        nominal = std::abs(measured_hz - nominal_refresh_hz) <=
                  std::max(1.5, nominal_refresh_hz * 0.015);
      }
      v.stable = nominal && g_host_vblank_samples >= 32 && v.period_ns > 0.0 &&
                 std::sqrt(g_host_vblank_variance) < v.period_ns * 0.1;
    }
    v.time_ns = now;
    ++v.count;
  }
  g_host_vblank_condition.notify_all();
}

void ResetHostVblank() {
  std::lock_guard<std::mutex> lock(g_host_vblank_mutex);
  g_host_vblank.time_ns = 0;
  g_host_vblank.period_ns = 0.0;
  g_host_vblank.stable = false;
  g_host_vblank_variance = 0.0;
  g_host_vblank_samples = 0;
}

HostVblankInfo GetHostVblank() {
  std::lock_guard<std::mutex> lock(g_host_vblank_mutex);
  return g_host_vblank;
}

bool WaitForHostVblank(uint64_t count, std::chrono::nanoseconds timeout, HostVblankInfo& info) {
  std::unique_lock<std::mutex> lock(g_host_vblank_mutex);
  const bool signaled = g_host_vblank_condition.wait_for(
      lock, timeout, [count] { return g_host_vblank.count > count; });
  info = g_host_vblank;
  return signaled;
}

double GetHostRefreshRate() {
  std::lock_guard<std::mutex> lock(g_host_vblank_mutex);
  return g_host_vblank.period_ns > 0.0 && g_host_vblank_samples >= 32 ? 1.0e9 / g_host_vblank.period_ns
                                                                       : 0.0;
}

void SetHostVsync(bool vsync) {
  g_host_vsync.store(vsync, std::memory_order_relaxed);
}

bool IsHostVsync() {
  return g_host_vsync.load(std::memory_order_relaxed);
}

}  // namespace rex::graphics
