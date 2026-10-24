/**
 * @file        ui/ui_watchdog.cpp
 * @brief       UI thread watchdog (see rex/ui/ui_watchdog.h).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/ui_watchdog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(ui_watchdog, true, "UI",
                    "Log when the window stops answering for seconds (what it was doing)");

namespace rex::ui {

namespace {

std::atomic<const char*> g_phase{nullptr};
// Steady-clock ms when the UI thread started the event it is handling now, 0
// while it waits for events (idle).
std::atomic<int64_t> g_busy_since_ms{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void WatchdogLoop() {
  constexpr int64_t kFirstReportMs = 5000;
  int64_t reported_since = 0;  // the stall reported last (its start)
  int64_t next_report_ms = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int64_t since = g_busy_since_ms.load(std::memory_order_acquire);
    const int64_t now = NowMs();
    if (since == 0 || since != reported_since) {
      if (reported_since != 0) {
        REXLOG_WARN("UI watchdog: the window answers again");
        rex::FlushLogging();
        reported_since = 0;
      }
      if (since == 0 || now - since < kFirstReportMs) {
        continue;
      }
    }
    if (reported_since == 0) {
      reported_since = since;
      next_report_ms = since + kFirstReportMs;
    }
    if (now < next_report_ms) {
      continue;
    }
    const char* phase = g_phase.load(std::memory_order_acquire);
    REXLOG_WARN("UI watchdog: the window hasn't answered for {} s (busy: {})",
                (now - since) / 1000, phase ? phase : "unknown");
    rex::FlushLogging();
    // 5 s, 15 s, 30 s, 60 s, then every minute.
    const int64_t stalled = now - since;
    next_report_ms = since + (stalled < 15000   ? 15000
                              : stalled < 30000 ? 30000
                                                : (stalled / 60000 + 1) * 60000);
  }
}

}  // namespace

const char* SetUIThreadPhase(const char* phase) {
  return g_phase.exchange(phase, std::memory_order_acq_rel);
}

void UIThreadBusyBegin() {
  g_busy_since_ms.store(NowMs(), std::memory_order_release);
}

void UIThreadBusyEnd() {
  g_busy_since_ms.store(0, std::memory_order_release);
}

void StartUIWatchdog() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!REXCVAR_GET(ui_watchdog)) {
      return;
    }
    std::thread(WatchdogLoop).detach();
  });
}

}  // namespace rex::ui

