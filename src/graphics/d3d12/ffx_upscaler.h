/**
 * @file        graphics/d3d12/ffx_upscaler.h
 * @brief       AMD FSR 4 / FSR 3.1.5 through AMD's FidelityFX API DLLs, for the
 *              3D scene upscaler (D3D12Dlss)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::graphics::d3d12 {

// AMD's signed amd_fidelityfx_loader_dx12.dll, loaded at runtime from the
// executable's folder (it loads amd_fidelityfx_upscaler_dx12.dll from there):
// the upscaler picks FSR 4 where the GPU and driver have it (RX 9000, RX 7000)
// and FSR 3.1.5 elsewhere. No FidelityFX type is in this header - only
// ffx_upscaler.cpp includes AMD's headers, which are only there with
// REX_HAS_FSR_SDK (otherwise this is never available).
class D3D12FfxUpscaler {
 public:
  // Context creation options.
  enum CreateFlags : uint32_t {
    kCreateHighDynamicRange = 1u << 0,
    kCreateDepthInverted = 1u << 1,
    kCreateDepthInfinite = 1u << 2,
    kCreateAutoExposure = 1u << 3,
    // Perceptual (gamma-encoded) color - used by FSR 4 only.
    kCreateNonLinearColor = 1u << 4,
    kCreateDebugVisualization = 1u << 5,
    kCreateDebugChecking = 1u << 6,
  };

  struct DispatchParams {
    // In NON_PIXEL_SHADER_RESOURCE: the color (render size in its top-left),
    // depth (R32 float) and motion vectors (R16G16 float, in render pixels,
    // previous minus current, without the jitter).
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion_vectors = nullptr;
    // In UNORDERED_ACCESS.
    ID3D12Resource* output = nullptr;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    // The jitter of the scene's draws, in render pixels.
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    // 0 = no sharpening, up to 1.
    float sharpness = 0.0f;
    float frame_time_ms = 16.67f;
    bool reset = false;
    float camera_near = 0.1f;
    float camera_far = 1000.0f;
    float camera_fov_y = 1.047f;
    // The color is sRGB-encoded (FSR 4 only).
    bool non_linear_srgb = false;
    // FSR's own debug view in the output.
    bool debug_view = false;
  };

  D3D12FfxUpscaler() = default;
  ~D3D12FfxUpscaler();
  D3D12FfxUpscaler(const D3D12FfxUpscaler&) = delete;
  D3D12FfxUpscaler& operator=(const D3D12FfxUpscaler&) = delete;

  // Loads the DLLs and lists the upscaler versions this GPU has. False if FSR
  // isn't available (no DLLs, or built without the SDK).
  bool Initialize(ID3D12Device* device);
  // With the GPU idle: destroys the contexts (the DLLs stay loaded).
  void Shutdown();
  bool available() const { return available_; }

  // The upscaler versions (newest first): id and name ("4.1.1", "3.1.5"...).
  const std::vector<std::pair<uint64_t, std::string>>& versions() const { return versions_; }
  // The id of the newest version with this major version, 0 if none.
  uint64_t FindVersion(uint32_t major) const;

  // Makes the context for output_width x output_height (it also takes any
  // render size up to it) unless the current one already is for these
  // settings - the old one is destroyed once `submission` has completed.
  // version: an id from versions(), 0 = the newest the GPU has.
  bool EnsureContext(uint32_t output_width, uint32_t output_height, uint32_t create_flags,
                     uint64_t version, uint64_t submission, bool& created_out);
  // The current context, to capture when recording (it's used when the
  // command list runs).
  void* context() const { return context_; }
  // The version the current context runs: name and major version.
  const std::string& provider_name() const { return provider_name_; }
  uint32_t provider_major() const { return provider_major_; }
  // Records FSR into the command list (when the deferred command list runs).
  bool Dispatch(void* context, ID3D12GraphicsCommandList* command_list,
                const DispatchParams& params) const;
  // No context while FSR isn't used - destroyed once `submission` has
  // completed.
  void RetireContext(uint64_t submission);
  // Destroys the retired contexts the GPU is done with.
  void ReleaseRetired(uint64_t completed_submission);

 private:
  void DestroyContext(void* context);

  bool available_ = false;
  ID3D12Device* device_ = nullptr;
  // ffxCreateContext, ffxDestroyContext, ffxQuery, ffxDispatch.
  void* create_context_ = nullptr;
  void* destroy_context_ = nullptr;
  void* query_ = nullptr;
  void* dispatch_ = nullptr;
  std::vector<std::pair<uint64_t, std::string>> versions_;

  void* context_ = nullptr;
  uint32_t context_width_ = 0;
  uint32_t context_height_ = 0;
  uint32_t context_flags_ = 0;
  uint64_t context_version_ = 0;
  std::string provider_name_;
  uint32_t provider_major_ = 0;
  std::vector<std::pair<uint64_t, void*>> retired_;
};

}  // namespace rex::graphics::d3d12
