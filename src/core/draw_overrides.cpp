/**
 * @file        core/draw_overrides.cpp
 * @brief       Per-pixel-shader draw overrides. See graphics/draw_overrides.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/draw_overrides.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <xxhash.h>

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
std::mutex g_memory_replacements_mutex;
std::unordered_map<uint64_t, std::shared_ptr<const TextureMemoryReplacement>>
    g_memory_replacements;
std::atomic<bool> g_has_memory_replacements{false};

// Xenos 2D tiling (XGAddress2DTiledOffset), as texture_util::GetTiledOffset2D.
uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bytes_per_block_log2) {
  pitch = (pitch + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bytes_per_block_log2 + 7);
  const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}
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

void SetTextureMemoryReplacement(uint64_t key, uint32_t width, uint32_t height,
                                 std::vector<uint8_t> rgba) {
  if (!width || !height || rgba.size() != size_t(width) * height * 4) {
    return;
  }
  auto image = std::make_shared<TextureMemoryReplacement>();
  image->width = width;
  image->height = height;
  image->rgba = std::move(rgba);
  std::lock_guard lock(g_memory_replacements_mutex);
  g_memory_replacements.emplace(key, std::move(image));
  g_has_memory_replacements.store(true, std::memory_order_release);
}

std::shared_ptr<const TextureMemoryReplacement> FindTextureMemoryReplacement(uint64_t key) {
  if (!g_has_memory_replacements.load(std::memory_order_acquire)) {
    return nullptr;
  }
  std::lock_guard lock(g_memory_replacements_mutex);
  auto it = g_memory_replacements.find(key);
  return it != g_memory_replacements.end() ? it->second : nullptr;
}

bool HasTextureMemoryReplacements() {
  return g_has_memory_replacements.load(std::memory_order_acquire);
}

uint64_t TiledTexture2DReplacementKey(const uint8_t* blocks, uint32_t width, uint32_t height,
                                      uint32_t bytes_per_block, uint32_t block_texels) {
  if (!blocks || !width || !height || !bytes_per_block || !block_texels ||
      (bytes_per_block & (bytes_per_block - 1))) {
    return 0;
  }
  uint32_t log2 = 0;
  while ((1u << log2) < bytes_per_block) {
    ++log2;
  }
  const uint32_t x_blocks = (width + block_texels - 1) / block_texels;
  const uint32_t y_blocks = (height + block_texels - 1) / block_texels;
  // The tiled base level takes whole 32x32-block tiles.
  const size_t size = size_t((x_blocks + 31) & ~31u) * ((y_blocks + 31) & ~31u) * bytes_per_block;
  std::vector<uint8_t> tiled(size, 0);
  for (uint32_t y = 0; y < y_blocks; ++y) {
    for (uint32_t x = 0; x < x_blocks; ++x) {
      const size_t offset = TiledOffset2D(x, y, x_blocks, log2);
      if (offset + bytes_per_block <= size) {
        std::memcpy(tiled.data() + offset,
                    blocks + (size_t(y) * x_blocks + x) * bytes_per_block, bytes_per_block);
      }
    }
  }
  return XXH3_64bits(tiled.data(), tiled.size());
}

}  // namespace rex::graphics
