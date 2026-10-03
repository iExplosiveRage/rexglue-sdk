/**
 * @file        graphics/d3d12/dlss.h
 * @brief       NVIDIA DLAA for the 3D scene, run before the HUD is drawn
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include <rex/ui/d3d12/d3d12_api.h>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;

// NVIDIA DLAA (DLSS at the render resolution) for the 3D scene, run right
// before the HUD is drawn so the HUD isn't part of it (cvar dlss_mode):
// - The scene draws - depth test on and not always passing, into the
//   frontbuffer-wide render target - get a sub-pixel jitter every frame.
// - At the first HUD-class draw after them (depth test on but always passing),
//   the render target cache hands over the scene's color and depth. Camera
//   motion vectors are made from the depth and this frame's and the previous
//   frame's camera matrix, DLSS runs, and its output replaces the scene color.
//   The game then draws the HUD over it as usual.
// The camera matrix is the vertex shader constants c0-c3 (row vectors) shared
// by the most scene draws of the frame - the static parts of the world.
// Objects moving by themselves get the camera motion of what's behind them.
class D3D12Dlss {
 public:
  explicit D3D12Dlss(D3D12CommandProcessor& command_processor);
  ~D3D12Dlss();

  bool Initialize();
  // With the GPU idle.
  void Shutdown();

  // Jitter of this frame's scene draws in render target pixels (X right, Y
  // down), 0 when there's none.
  float jitter_x() const { return jitter_[0]; }
  float jitter_y() const { return jitter_[1]; }

  // For every scene draw before the HUD, with its vertex shader float
  // constants c0-c3.
  void OnSceneDraw(const float* constants_c0_c3);
  // Whether the next HUD-class draw should hand the scene over.
  bool WantsScene() const;
  // From the render target cache, before the HUD draw's render targets are
  // bound, with both of them in NON_PIXEL_SHADER_RESOURCE. The scene is in the
  // top-left width x height pixels. Returns the state the color render target
  // is left in.
  D3D12_RESOURCE_STATES ProcessScene(ID3D12Resource* color, D3D12_CPU_DESCRIPTOR_HANDLE depth_srv,
                                     uint32_t width, uint32_t height);
  // After the frame's swap, outside a submission.
  void EndFrame();

 private:
  struct Texture {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  };

  bool IsEnabled() const;
  bool InitializeNgx();
  void ShutdownNgx();
  bool EnsureTexture(Texture& texture, DXGI_FORMAT format, uint32_t width, uint32_t height,
                     const char* name);
  void Transition(Texture& texture, D3D12_RESOURCE_STATES state);
  // The camera matrix shared by the most scene draws this frame, or nullptr.
  const std::array<float, 16>* FrameCamera() const;

  D3D12CommandProcessor& command_processor_;

  Microsoft::WRL::ComPtr<ID3D12RootSignature> inputs_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> inputs_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> debug_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> debug_pipeline_;

  Texture depth_;
  Texture motion_;
  Texture output_;
  // Resources (and DLSS features) replaced while the GPU may still use them,
  // with the submission after which they can go.
  std::vector<std::pair<uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>> resources_to_release_;
  std::vector<std::pair<uint64_t, NVSDK_NGX_Handle*>> features_to_release_;

  // NGX.
  enum class NgxState { kNotInitialized, kAvailable, kUnavailable };
  NgxState ngx_state_ = NgxState::kNotInitialized;
  NVSDK_NGX_Parameter* ngx_parameters_ = nullptr;
  NVSDK_NGX_Handle* feature_ = nullptr;
  uint32_t feature_width_ = 0;
  uint32_t feature_height_ = 0;
  DXGI_FORMAT feature_output_format_ = DXGI_FORMAT_UNKNOWN;
  uint32_t feature_preset_ = 0;
  bool feature_failed_ = false;

  // This frame.
  float jitter_[2] = {};
  bool frame_scene_drawn_ = false;
  // The scene has been handed over (DLSS doesn't run twice in a frame).
  bool frame_scene_taken_ = false;
  // DLSS (or the debug view) replaced the scene.
  bool frame_processed_ = false;
  // Distinct c0-c3 of the scene draws and how many draws used each.
  std::vector<std::pair<std::array<float, 16>, uint32_t>> frame_cameras_;
  std::array<float, 16> frame_camera_{};
  bool frame_camera_valid_ = false;

  // Across frames.
  bool previous_processed_ = false;
  std::array<float, 16> previous_camera_{};
  bool previous_camera_valid_ = false;
  uint32_t previous_width_ = 0;
  uint32_t previous_height_ = 0;
  uint32_t jitter_index_ = 0;
};

}  // namespace rex::graphics::d3d12
