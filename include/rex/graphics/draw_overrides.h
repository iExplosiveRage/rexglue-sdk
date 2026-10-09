/**
 * @file        graphics/draw_overrides.h
 * @brief       Per-pixel-shader draw overrides, e.g. to switch off a game's
 *              post-processing passes.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rex::graphics {

// Overrides for the draws made with a specific guest pixel shader, identified
// by its ucode hash (as in the dump_shaders file names). Setting them is
// thread-safe; the command processor looks them up for every draw.
struct PixelShaderDrawOverride {
  uint64_t ucode_hash = 0;
  // Drop the draws entirely.
  bool skip = false;
  // Pixel shader float constants (c0-c255) replaced while drawing.
  struct Constant {
    uint32_t index = 0;
    uint32_t component_mask = 0xF;  // Bit 0 = x ... bit 3 = w.
    std::array<float, 4> value{};
  };
  std::vector<Constant> constants;
  // Follow the scene projection scale (field of view) on every vertex, not
  // only perspective ones - for scene effects drawn as screen-space sprites.
  bool scene_projection_all_vertices = false;
};

// Replaces the overrides set under `owner`; the overrides of all owners apply.
void SetPixelShaderDrawOverrides(std::string_view owner,
                                 std::vector<PixelShaderDrawOverride> overrides);

// For the command processor thread: the override of a pixel shader, or
// nullptr. The pointer stays valid until the next call on the same thread.
const PixelShaderDrawOverride* FindPixelShaderDrawOverride(uint64_t ucode_hash);

// Field of view of the 3D scene: the projected X and Y of depth-tested draws
// into the main (frontbuffer-wide) render target are multiplied by this,
// around the center of the screen. 1 = unchanged, below 1 = wider view (like
// a larger FOV), above 1 = zoomed in. Thread-safe.
void SetSceneProjectionScale(float scale);
float GetSceneProjectionScale();

// Also apply the field of view (and roll) to the perspective vertices of
// triangles drawn into the main render target with the depth test off - the
// scene effects some games draw after the scene (glows, flares, speed lines).
// Full-screen passes (quads, rectangles) and the HUD (depth test on, always
// passing) stay as they are. Thread-safe.
void SetSceneProjectionUndepthedTriangles(bool enable);
bool GetSceneProjectionUndepthedTriangles();

// Roll of the 3D scene in radians (the same draws as the projection scale),
// turning the picture around the center of the screen like a tilted camera.
// Thread-safe.
void SetSceneProjectionRoll(float radians);
float GetSceneProjectionRoll();

// Drops the draws made into the main render target with the depth test on but
// set to always pass - the HUD drawn over the 3D scene - e.g. for a photo
// mode. Thread-safe.
void SetHideHudDraws(bool hide);
bool GetHideHudDraws();

// NVIDIA DLSS on this GPU, set by the GPU backend once it has checked NGX.
// DLSS's upscaling modes only lower the draw resolution scale with
// kAvailable - with kDlaaOnly (upscaling failed) DLSS runs at the configured
// scale. Thread-safe.
enum class DlssAvailability : uint32_t { kUnavailable, kDlaaOnly, kAvailable };
void SetDlssAvailability(DlssAvailability availability);
DlssAvailability GetDlssAvailability();

// AMD FSR for the 3D scene (FSR 4 or 3.1.5) on this GPU, set by the GPU
// backend once AMD's DLLs are loaded - only when fsr_mode is on. Like
// DlssAvailability: its upscaling modes only lower the draw resolution scale
// with kAvailable (kNativeOnly: upscaling failed). DLSS goes first when both
// are on. Thread-safe.
enum class FsrAvailability : uint32_t { kUnavailable, kNativeOnly, kAvailable };
void SetFsrAvailability(FsrAvailability availability);
FsrAvailability GetFsrAvailability();
// The FSR version running ("4.1.1", "3.1.5"), empty before the first frame
// using it.
void SetFsrProviderName(std::string_view name);
std::string GetFsrProviderName();

// Replacement images the program supplies from memory, used like the files of
// a texture pack (any size, mipmapped when loaded) whether or not
// texture_replace_enabled is on, and before the pack's files. `key` is the
// texture pack key of the guest texture they replace (see
// TiledTexture2DReplacementKey). A key keeps its image once set. Thread-safe.
struct TextureMemoryReplacement {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;  // R8G8B8A8, straight alpha, width * height * 4
};
void SetTextureMemoryReplacement(uint64_t key, uint32_t width, uint32_t height,
                                 std::vector<uint8_t> rgba);
std::shared_ptr<const TextureMemoryReplacement> FindTextureMemoryReplacement(uint64_t key);
bool HasTextureMemoryReplacements();

// The texture pack key of a 2D texture with one level, tiled as the GPU reads
// it, whose blocks (4x4-texel blocks of `bytes_per_block` - 8 or 16 - or
// single texels of 1, 2, 4 or 8 bytes with `block_texels` 1) are `blocks`,
// row by row, each as the guest stores it: XXH3 of the tiled base level with
// the padding taken as zero, like the texture cache (and the files' names).
uint64_t TiledTexture2DReplacementKey(const uint8_t* blocks, uint32_t width, uint32_t height,
                                      uint32_t bytes_per_block, uint32_t block_texels = 4);

}  // namespace rex::graphics
