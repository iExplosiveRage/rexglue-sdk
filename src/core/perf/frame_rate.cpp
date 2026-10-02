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
std::atomic<uint32_t> g_frontbuffer_width{0};
std::atomic<uint32_t> g_frontbuffer_height{0};
std::atomic<uint32_t> g_scale_x{0};
std::atomic<uint32_t> g_scale_y{0};
std::atomic<uint32_t> g_requested_scale_x{0};
std::atomic<uint32_t> g_requested_scale_y{0};
}  // namespace

void RecordGuestSwap() {
  g_guest_swap_count.fetch_add(1, std::memory_order_relaxed);
}

uint64_t GetGuestSwapCount() {
  return g_guest_swap_count.load(std::memory_order_relaxed);
}

void RecordGuestFrontbuffer(uint32_t width, uint32_t height) {
  g_frontbuffer_width.store(width, std::memory_order_relaxed);
  g_frontbuffer_height.store(height, std::memory_order_relaxed);
}

void SetDrawResolutionScale(uint32_t x, uint32_t y, uint32_t requested_x, uint32_t requested_y) {
  g_scale_x.store(x, std::memory_order_relaxed);
  g_scale_y.store(y, std::memory_order_relaxed);
  g_requested_scale_x.store(requested_x, std::memory_order_relaxed);
  g_requested_scale_y.store(requested_y, std::memory_order_relaxed);
}

RenderInfo GetRenderInfo() {
  RenderInfo info;
  info.frontbuffer_width = g_frontbuffer_width.load(std::memory_order_relaxed);
  info.frontbuffer_height = g_frontbuffer_height.load(std::memory_order_relaxed);
  info.scale_x = g_scale_x.load(std::memory_order_relaxed);
  info.scale_y = g_scale_y.load(std::memory_order_relaxed);
  info.requested_scale_x = g_requested_scale_x.load(std::memory_order_relaxed);
  info.requested_scale_y = g_requested_scale_y.load(std::memory_order_relaxed);
  return info;
}

}  // namespace rex::perf
