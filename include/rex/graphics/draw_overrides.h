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

}  // namespace rex::graphics
