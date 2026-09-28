/**
 * @file        core/perf/frame_rate.cpp
 * @brief       Guest frame rate tracking. See perf/frame_rate.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/perf/frame_rate.h>

#include <atomic>

namespace rex::perf {

namespace {
std::atomic<uint64_t> g_guest_swap_count{0};
}  // namespace

void RecordGuestSwap() {
  g_guest_swap_count.fetch_add(1, std::memory_order_relaxed);
}

uint64_t GetGuestSwapCount() {
  return g_guest_swap_count.load(std::memory_order_relaxed);
}

}  // namespace rex::perf
