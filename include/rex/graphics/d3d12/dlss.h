/**
 * @file        graphics/d3d12/dlss.h
 * @brief       NVIDIA DLSS and AMD FSR (anti-aliasing and upscaling) for the 3D
 *              scene, run before the HUD is drawn
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rex/ui/d3d12/d3d12_api.h>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;
class D3D12FfxUpscaler;

// NVIDIA DLSS - or AMD FSR (FSR 4 where the GPU has it, FSR 3.1.5 elsewhere)
// when DLSS is off or doesn't work - for the 3D scene, run right before the HUD
// is drawn so the HUD isn't part of it (cvars dlss_mode, fsr_mode). Everything
// below is the same for both; only the call to the upscaler differs:
// - The scene draws - depth test on and not always passing, into the
//   frontbuffer-wide render target - get a sub-pixel jitter every frame.
// - At the first HUD-class draw after them (depth test on but always passing),
//   the render target cache hands over the scene's color and depth. Camera
//   motion vectors are made from the depth and this frame's and the previous
//   frame's camera matrix, and DLSS runs.
// The camera matrix is the vertex shader constants c0-c3 (row vectors) shared
// by the most scene draws of the frame - the static parts of the world.
// Objects moving by themselves get the camera motion of what's behind them.
//
// DLAA (the render resolution is the output's): DLSS's output replaces the
// scene in the color render target, and the game draws the HUD over it.
//
// Upscaling (the game renders at a lower draw resolution scale than the one
// configured, the "target"): DLSS's output is an output-size picture of its
// own. It's also taken back to the render resolution in place of the scene, so
// the game goes on with the anti-aliased scene. Every draw the game then makes
// into that render target - the HUD - is drawn again into the output-size
// picture (the command processor mirrors them), until the first copy (resolve)
// of the render target, which is where the frame is finished. Before
// presenting, the frame the game presents is compared, in tiles, with the
// render target at that copy: where they match, the output-size picture is
// presented, elsewhere (anything drawn later, a pause menu, a fade) the frame
// itself, stretched. Draws that can't be mirrored (that test depth or stencil,
// use the pixel position, write elsewhere too...) give up the output-size
// picture for the frame.
class D3D12Dlss {
 public:
  explicit D3D12Dlss(D3D12CommandProcessor& command_processor);
  ~D3D12Dlss();

  // Also finds out whether DLSS works on this GPU
  // (rex::graphics::SetDlssAvailability), and loads FSR if fsr_mode is on
  // (rex::graphics::SetFsrAvailability).
  bool Initialize();
  // Without host render targets (pixel shader interlock): neither upscaler
  // gets the scene.
  void OnHostRenderTargetsUnavailable();
  // With the GPU idle.
  void Shutdown();

  // Jitter of this frame's scene draws in render target pixels (X right, Y
  // down), 0 when there's none.
  float jitter_x() const { return jitter_[0]; }
  float jitter_y() const { return jitter_[1]; }
  // The scene has been handed over in this frame - what's drawn now is the
  // HUD (not jittered).
  bool scene_taken() const { return frame_scene_taken_; }
  // Added to the texture LOD bias (in 1/32) of the draws making the scene when
  // DLSS upscales it, for the detail of the output resolution.
  int32_t scene_lod_bias() const { return frame_scene_taken_ ? 0 : lod_bias_; }

  // For every scene draw before the HUD, with its vertex shader float
  // constants c0-c3.
  void OnSceneDraw(const float* constants_c0_c3);
  // Whether the next HUD-class draw should hand the scene over.
  bool WantsScene() const;
  // From the render target cache, before the HUD draw's render targets are
  // bound, with both of them in NON_PIXEL_SHADER_RESOURCE. The scene is in the
  // top-left width x height pixels, output_width x output_height is the size
  // DLSS makes (larger when upscaling). Returns the state the color render
  // target is left in.
  D3D12_RESOURCE_STATES ProcessScene(ID3D12Resource* color, ID3D12Resource* depth,
                                     D3D12_CPU_DESCRIPTOR_HANDLE depth_srv, uint32_t width,
                                     uint32_t height, uint32_t output_width,
                                     uint32_t output_height);

  // Upscaling: mirroring the draws into the scene's render target into the
  // output-size picture.
  bool IsHudMirrorActive() const { return hud_state_ == HudState::kMirroring; }
  // The render target (resource) whose draws are mirrored.
  ID3D12Resource* hud_render_target() const { return hud_render_target_; }
  // RTV / DSV formats of the output-size targets.
  DXGI_FORMAT hud_color_format() const { return hud_color_format_; }
  DXGI_FORMAT hud_depth_format() const { return hud_depth_format_; }
  uint32_t hud_width() const { return hud_width_; }
  uint32_t hud_height() const { return hud_height_; }
  // Binds the output-size targets (color, and depth / stencil if with_depth)
  // for a mirrored draw.
  void BindHudTargets(bool with_depth);
  // A draw into the render target that can't be mirrored: no output-size
  // picture in this frame. Logged once per reason and shaders.
  void InvalidateHud(const char* reason, uint64_t vertex_shader_hash, uint64_t pixel_shader_hash);
  // At the first resolve of the render target after the handover, with it in
  // NON_PIXEL_SHADER_RESOURCE: stops mirroring and keeps a copy of it for the
  // comparison before presenting. Returns the state the render target is
  // left in.
  D3D12_RESOURCE_STATES FreezeHud(ID3D12Resource* color);

  // Before presenting, in frames with upscaling configured: makes the
  // output-size picture to present (see the class comment) from the frame the
  // game presents (its texture, view and the frame's size in it). The
  // comparison leaves out edge_pixels at the top and left - where resolves at a
  // draw resolution scale fill the guest's half-pixel gap with the next
  // pixels. Returns false if it can't - the frame is presented as it is then.
  // upscaled_out: whether DLSS's picture is in it (otherwise it's the frame,
  // only stretched).
  bool ComposeUpscaledOutput(ID3D12Resource* frame, const D3D12_SHADER_RESOURCE_VIEW_DESC& frame_srv,
                             uint32_t frame_width, uint32_t frame_height,
                             uint32_t output_width, uint32_t output_height,
                             uint32_t edge_pixels, ID3D12Resource*& output_out,
                             D3D12_SHADER_RESOURCE_VIEW_DESC& output_srv_out,
                             bool& upscaled_out);

  // After the draw resolution scale (or the scale DLSS upscales to) changed,
  // with the GPU idle.
  void OnDrawResolutionScaleChanged();
  // After the frame's swap, outside a submission.
  void EndFrame();

 private:
  struct Texture {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  };

  enum class Backend { kNone, kDlss, kFsr };

  enum class HudState {
    // No output-size picture in this frame (yet).
    kNone,
    // DLSS made it, the draws into the render target are mirrored.
    kMirroring,
    // The render target has been copied - the picture is complete.
    kFrozen,
    // A draw couldn't be mirrored.
    kInvalid,
  };

  // The upscaler the scene goes to: DLSS if it's on and works, else FSR.
  Backend SelectBackend() const;
  bool IsEnabled() const { return SelectBackend() != Backend::kNone; }
  bool InitializeNgx();
  // Loads FSR (once); sets the availability.
  void TryInitializeFsr();
  void ShutdownNgx();
  bool EnsureTexture(Texture& texture, DXGI_FORMAT format, uint32_t width, uint32_t height,
                     const char* name,
                     D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  bool EnsureHudDepth(DXGI_FORMAT resource_format, uint32_t width, uint32_t height);
  void Transition(Texture& texture, D3D12_RESOURCE_STATES state);
  void ReleaseLater(Texture& texture);
  // The camera matrix shared by the most scene draws this frame, or nullptr.
  const std::array<float, 16>* FrameCamera() const;
  // Upscaling failed: anti-aliasing at the configured scale from now on (for
  // that upscaler).
  void DisableUpscaling(Backend backend, const char* reason);
  // FSR failed entirely: off from now on.
  void DisableFsr();
  // Records FSR for ProcessScene (its inputs ready, the output in
  // UNORDERED_ACCESS). False if it can't run.
  bool RecordFsr(ID3D12Resource* color, DXGI_FORMAT color_format, uint32_t width, uint32_t height,
                 uint32_t output_width, uint32_t output_height, bool upscale, bool reset,
                 ID3D12Resource* output);

  D3D12CommandProcessor& command_processor_;

  Microsoft::WRL::ComPtr<ID3D12RootSignature> inputs_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> inputs_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> debug_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> debug_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> downsample_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> downsample_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> compare_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> compare_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> present_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> present_pipeline_;
  // One RTV and one DSV for the output-size targets, written when they're
  // (re)created - only at a handover, when nothing recorded uses them.
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> hud_rtv_heap_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> hud_dsv_heap_;

  Texture depth_;
  Texture motion_;
  // DLAA's output, or the upscaled scene back at the render resolution.
  Texture output_;
  // Upscaling: the output-size picture (DLSS's output, then the HUD).
  Texture upscaled_;
  // Its depth / stencil for the mirrored draws (their pipelines have one).
  Texture hud_depth_;
  // The render target at the freeze, and the comparison's tile results.
  Texture frozen_;
  Texture tiles_;
  // The picture presented.
  Texture present_;
  // FSR: the scene's color at its own size (the render target is larger).
  Texture fsr_color_;
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
  uint32_t feature_output_width_ = 0;
  uint32_t feature_output_height_ = 0;
  DXGI_FORMAT feature_output_format_ = DXGI_FORMAT_UNKNOWN;
  uint32_t feature_preset_ = 0;
  bool feature_failed_ = false;
  bool upscaling_failed_ = false;

  // AMD FSR.
  std::unique_ptr<D3D12FfxUpscaler> ffx_;
  bool fsr_tried_ = false;
  // No host render targets.
  bool fsr_blocked_ = false;
  bool fsr_failed_ = false;
  bool fsr_upscaling_failed_ = false;
  std::chrono::steady_clock::time_point fsr_previous_time_{};
  bool fsr_camera_logged_ = false;

  // This frame.
  float jitter_[2] = {};
  bool frame_scene_drawn_ = false;
  // The scene has been handed over (DLSS doesn't run twice in a frame).
  bool frame_scene_taken_ = false;
  // DLSS (or the debug view) replaced the scene.
  bool frame_processed_ = false;
  // DLSS upscaled the scene.
  bool frame_upscaled_ = false;
  // When the command list runs: DLSS made its output - the work using it
  // (copying it into the render target, presenting it) checks this then, as
  // NGX only fails when the commands are recorded on the GPU's command list.
  bool dlss_output_valid_ = false;
  // Distinct c0-c3 of the scene draws and how many draws used each.
  std::vector<std::pair<std::array<float, 16>, uint32_t>> frame_cameras_;
  std::array<float, 16> frame_camera_{};
  bool frame_camera_valid_ = false;
  HudState hud_state_ = HudState::kNone;
  ID3D12Resource* hud_render_target_ = nullptr;
  DXGI_FORMAT hud_color_format_ = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT hud_depth_format_ = DXGI_FORMAT_UNKNOWN;
  uint32_t hud_width_ = 0;
  uint32_t hud_height_ = 0;
  uint32_t frame_render_width_ = 0;
  uint32_t frame_render_height_ = 0;
  std::set<std::string> hud_invalid_logged_;

  // Across frames.
  Backend previous_backend_ = Backend::kNone;
  bool previous_processed_ = false;
  std::array<float, 16> previous_camera_{};
  bool previous_camera_valid_ = false;
  uint32_t previous_width_ = 0;
  uint32_t previous_height_ = 0;
  uint32_t previous_output_width_ = 0;
  uint32_t previous_output_height_ = 0;
  uint32_t jitter_phases_ = 32;
  uint32_t jitter_index_ = 0;
  int32_t lod_bias_ = 0;
};

}  // namespace rex::graphics::d3d12
