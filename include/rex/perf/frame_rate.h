/**
 * @file        perf/frame_rate.h
 * @brief       Guest frame rate tracking, available in every build config.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rex::perf {

// Called once per guest swap (VdSwap) by the command processor.
void RecordGuestSwap();

// Total guest swaps since startup.
uint64_t GetGuestSwapCount();

// Times between the latest guest swaps in milliseconds, oldest first (up to
// the last 255). Returns how many were written.
size_t GetGuestFrameTimes(float* out_ms, size_t max_count);

// Guest frontbuffer size (before resolution scaling) of the latest swap.
void RecordGuestFrontbuffer(uint32_t width, uint32_t height);

// Draw resolution scale the GPU backend renders with, and the one the config
// asked for (higher when an upscaler quality mode renders below it).
void SetDrawResolutionScale(uint32_t x, uint32_t y, uint32_t requested_x, uint32_t requested_y);

struct RenderInfo {
  uint32_t frontbuffer_width = 0;
  uint32_t frontbuffer_height = 0;
  uint32_t scale_x = 0;
  uint32_t scale_y = 0;
  uint32_t requested_scale_x = 0;
  uint32_t requested_scale_y = 0;
};
RenderInfo GetRenderInfo();

// Texture cache activity, for the perf_report debug command (totals since
// startup; the reader takes differences). Written by the GPU thread.
struct TextureCacheStats {
  std::atomic<uint64_t> created{0};             // host textures created
  std::atomic<uint64_t> destroyed{0};           // destroyed by the memory limits
  std::atomic<uint64_t> loaded{0};              // texture data uploads (guest or replacement)
  std::atomic<uint64_t> replacement_uploads{0}; // uploads from replacement images
  std::atomic<uint64_t> replacement_lookups{0}; // texture lookups checked for a replacement
  std::atomic<uint64_t> replacement_hashes{0};  // full replacement key hashes of guest data
  std::atomic<uint64_t> replacement_hash_ns{0};
  std::atomic<uint64_t> replacement_decodes{0}; // image files decoded on the GPU thread
  std::atomic<uint64_t> replacement_decode_ns{0};
  // Current values.
  std::atomic<uint64_t> memory_bytes{0};        // host memory of the cached textures
  std::atomic<uint32_t> limit_soft_mb{0};
  std::atomic<uint32_t> limit_hard_mb{0};
  std::atomic<uint64_t> vram_usage_bytes{0};    // the process's local video memory use
  std::atomic<uint64_t> vram_budget_bytes{0};   // the OS budget for it
};
TextureCacheStats& GetTextureCacheStats();

}  // namespace rex::perf
