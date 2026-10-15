/**
 * @file        graphics/wide_aspect.h
 * @brief       Wider-than-the-console output (ultrawide): the aspect ratio the
 *              guest picture is shown at, and where the 2D overlay (HUD, menus)
 *              goes in it.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>

namespace rex::graphics {

// The aspect ratio (width / height) the presenter shows the guest's picture at
// instead of the one of the guest's video mode - for a game that renders a
// wider view into its usual frontbuffer (anamorphic: the picture is stretched
// to this aspect ratio when presented). 0 = the guest's own. Thread-safe.
void SetGuestDisplayAspect(float aspect);
float GetGuestDisplayAspect();

// The size of the surface the presenter last painted to (the window's client
// area or the screen), 0 before the first paint. Thread-safe.
void SetOutputSurfaceSize(uint32_t width, uint32_t height);
void GetOutputSurfaceSize(uint32_t& width, uint32_t& height);

// Horizontal scale of the 3D scene drawn by the GPU, on top of the scene
// projection scale (draw_overrides.h): the projected X of the scene draws is
// multiplied by this - below 1 shows more of the scene to the sides (Hor+ for
// a wider output when the game's own projection can't be changed). Thread-safe.
void SetSceneProjectionAspectScale(float scale_x);
float GetSceneProjectionAspectScale();

// 2D overlay placement. For draws into the main (frontbuffer-wide) render
// target with a viewport centered on it and the depth test off or always
// passing (the HUD, menus, full-screen passes), the GPU backend asks the
// callback where to put them: clip-space X becomes x * scale_x + offset_x * w
// (around the center of the screen; offset_x in normalized device
// coordinates), and the scissor follows. A game rendering a wider picture
// into its usual frontbuffer uses this to keep its 2D undistorted (scale_x =
// guest aspect / output aspect) and to move parts of it to the sides.
struct OverlayDrawInfo {
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  // Guest vertex shader float constants c0-c7 (x, y, z, w each).
  float vertex_constants[32] = {};
  // Size of the texture at fetch constant 0 (0 if it isn't a 2D texture).
  uint32_t texture0_width = 0;
  uint32_t texture0_height = 0;
  bool depth_test = false;  // on and always passing (the HUD class), else off
  uint32_t primitive_type = 0;  // xenos::PrimitiveType
  uint32_t vertex_count = 0;
  // The vertex buffer of the vertex shader's first vertex fetch, as in guest
  // memory (big-endian), and its size in bytes (nullptr / 0 if none).
  const uint8_t* vertex_data = nullptr;
  uint32_t vertex_data_size = 0;
  // Frontbuffer size in guest pixels.
  uint32_t frontbuffer_width = 0;
  uint32_t frontbuffer_height = 0;
};
struct OverlayPlacement {
  float scale_x = 1.0f;
  float offset_x = 0.0f;
};
// Pixel shaders whose draws are asked about too while depth-tested (2D the
// game draws with a depth test, such as a movie). Up to 8. Thread-safe.
void SetOverlayDepthTestedPixelShaders(const uint64_t* hashes, uint32_t count);
bool IsOverlayDepthTestedPixelShader(uint64_t hash);

// Runs on the GPU command processor thread, for every such draw - keep it
// cheap. nullptr = off.
using OverlayPlacementCallback = OverlayPlacement (*)(const OverlayDrawInfo& info);
void SetOverlayPlacementCallback(OverlayPlacementCallback callback);
OverlayPlacementCallback GetOverlayPlacementCallback();

}  // namespace rex::graphics
