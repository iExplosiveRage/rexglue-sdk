/**
 * @file        core/wide_aspect.cpp
 * @brief       Ultrawide output state. See graphics/wide_aspect.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/wide_aspect.h>

#include <atomic>

namespace rex::graphics {

namespace {
std::atomic<float> g_guest_display_aspect{0.0f};
std::atomic<uint64_t> g_output_surface_size{0};
std::atomic<float> g_scene_projection_aspect_scale{1.0f};
std::atomic<OverlayPlacementCallback> g_overlay_placement_callback{nullptr};
constexpr uint32_t kMaxOverlayDepthTestedPixelShaders = 8;
std::atomic<uint64_t> g_overlay_depth_tested_pixel_shaders[kMaxOverlayDepthTestedPixelShaders] = {};
}  // namespace

void SetGuestDisplayAspect(float aspect) {
  g_guest_display_aspect.store(aspect > 0.0f ? aspect : 0.0f, std::memory_order_relaxed);
}

float GetGuestDisplayAspect() {
  return g_guest_display_aspect.load(std::memory_order_relaxed);
}

void SetOutputSurfaceSize(uint32_t width, uint32_t height) {
  g_output_surface_size.store(uint64_t(width) << 32 | height, std::memory_order_relaxed);
}

void GetOutputSurfaceSize(uint32_t& width, uint32_t& height) {
  const uint64_t size = g_output_surface_size.load(std::memory_order_relaxed);
  width = uint32_t(size >> 32);
  height = uint32_t(size);
}

void SetSceneProjectionAspectScale(float scale_x) {
  g_scene_projection_aspect_scale.store(scale_x > 0.0f ? scale_x : 1.0f,
                                        std::memory_order_relaxed);
}

float GetSceneProjectionAspectScale() {
  return g_scene_projection_aspect_scale.load(std::memory_order_relaxed);
}

void SetOverlayPlacementCallback(OverlayPlacementCallback callback) {
  g_overlay_placement_callback.store(callback, std::memory_order_release);
}

void SetOverlayDepthTestedPixelShaders(const uint64_t* hashes, uint32_t count) {
  for (uint32_t i = 0; i < kMaxOverlayDepthTestedPixelShaders; ++i) {
    g_overlay_depth_tested_pixel_shaders[i].store(i < count ? hashes[i] : 0,
                                                  std::memory_order_relaxed);
  }
}

bool IsOverlayDepthTestedPixelShader(uint64_t hash) {
  if (!hash) {
    return false;
  }
  for (const auto& entry : g_overlay_depth_tested_pixel_shaders) {
    if (entry.load(std::memory_order_relaxed) == hash) {
      return true;
    }
  }
  return false;
}

OverlayPlacementCallback GetOverlayPlacementCallback() {
  return g_overlay_placement_callback.load(std::memory_order_acquire);
}

}  // namespace rex::graphics
