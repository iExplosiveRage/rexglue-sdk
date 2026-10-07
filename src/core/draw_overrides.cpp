/**
 * @file        core/draw_overrides.cpp
 * @brief       Per-pixel-shader draw overrides. See graphics/draw_overrides.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/draw_overrides.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace rex::graphics {

namespace {
std::atomic<float> g_scene_projection_scale{1.0f};
std::atomic<float> g_scene_projection_roll{0.0f};
std::atomic<bool> g_scene_projection_undepthed_triangles{false};
std::atomic<bool> g_hide_hud_draws{false};
std::atomic<DlssAvailability> g_dlss_availability{DlssAvailability::kUnavailable};
std::atomic<FsrAvailability> g_fsr_availability{FsrAvailability::kUnavailable};
std::mutex g_fsr_provider_mutex;
std::string g_fsr_provider_name;
std::mutex g_overrides_mutex;
std::map<std::string, std::vector<PixelShaderDrawOverride>> g_overrides_by_owner;
std::atomic<uint32_t> g_overrides_generation{0};
}  // namespace

void SetPixelShaderDrawOverrides(std::string_view owner,
                                 std::vector<PixelShaderDrawOverride> overrides) {
  std::lock_guard lock(g_overrides_mutex);
  if (overrides.empty()) {
    g_overrides_by_owner.erase(std::string(owner));
  } else {
    g_overrides_by_owner[std::string(owner)] = std::move(overrides);
  }
  g_overrides_generation.fetch_add(1, std::memory_order_release);
}

const PixelShaderDrawOverride* FindPixelShaderDrawOverride(uint64_t ucode_hash) {
  // A copy per reading thread, refreshed only when the overrides change, so
  // the per-draw lookup takes no lock.
  thread_local std::vector<PixelShaderDrawOverride> snapshot;
  thread_local uint32_t snapshot_generation = UINT32_MAX;
  uint32_t generation = g_overrides_generation.load(std::memory_order_acquire);
  if (generation != snapshot_generation) {
    std::lock_guard lock(g_overrides_mutex);
    snapshot.clear();
    for (const auto& owner_overrides : g_overrides_by_owner) {
      snapshot.insert(snapshot.end(), owner_overrides.second.begin(),
                      owner_overrides.second.end());
    }
    snapshot_generation = g_overrides_generation.load(std::memory_order_relaxed);
  }
  for (const PixelShaderDrawOverride& draw_override : snapshot) {
    if (draw_override.ucode_hash == ucode_hash) {
      return &draw_override;
    }
  }
  return nullptr;
}

void SetSceneProjectionScale(float scale) {
  g_scene_projection_scale.store(scale, std::memory_order_relaxed);
}

float GetSceneProjectionScale() {
  return g_scene_projection_scale.load(std::memory_order_relaxed);
}

void SetSceneProjectionRoll(float radians) {
  g_scene_projection_roll.store(radians, std::memory_order_relaxed);
}

float GetSceneProjectionRoll() {
  return g_scene_projection_roll.load(std::memory_order_relaxed);
}

void SetSceneProjectionUndepthedTriangles(bool enable) {
  g_scene_projection_undepthed_triangles.store(enable, std::memory_order_relaxed);
}

bool GetSceneProjectionUndepthedTriangles() {
  return g_scene_projection_undepthed_triangles.load(std::memory_order_relaxed);
}

void SetHideHudDraws(bool hide) {
  g_hide_hud_draws.store(hide, std::memory_order_relaxed);
}

bool GetHideHudDraws() {
  return g_hide_hud_draws.load(std::memory_order_relaxed);
}

void SetDlssAvailability(DlssAvailability availability) {
  g_dlss_availability.store(availability, std::memory_order_release);
}

DlssAvailability GetDlssAvailability() {
  return g_dlss_availability.load(std::memory_order_acquire);
}

void SetFsrAvailability(FsrAvailability availability) {
  g_fsr_availability.store(availability, std::memory_order_release);
}

FsrAvailability GetFsrAvailability() {
  return g_fsr_availability.load(std::memory_order_acquire);
}

void SetFsrProviderName(std::string_view name) {
  std::lock_guard<std::mutex> lock(g_fsr_provider_mutex);
  g_fsr_provider_name = name;
}

std::string GetFsrProviderName() {
  std::lock_guard<std::mutex> lock(g_fsr_provider_mutex);
  return g_fsr_provider_name;
}

}  // namespace rex::graphics
