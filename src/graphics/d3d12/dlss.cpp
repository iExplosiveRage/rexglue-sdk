/**
 * @file        graphics/d3d12/dlss.cpp
 * @brief       NVIDIA DLAA for the 3D scene, run before the HUD is drawn
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d12/dlss.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/draw_overrides.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_util.h>

#if defined(REX_HAS_DLSS)
#include <nvsdk_ngx_helpers.h>
#endif

REXCVAR_DEFINE_STRING(dlss_mode, "off", "GPU",
                      "NVIDIA DLSS for the 3D scene (RTX GPUs): off, or dlaa - anti-aliasing at "
                      "the render resolution")
    .allowed({"off", "dlaa"});

REXCVAR_DEFINE_STRING(dlss_preset, "m", "GPU",
                      "DLSS model (NVIDIA's render presets): k (DLAA's default), l, or m - "
                      "sharper and more stable than k, at about the same cost on RTX 40 GPUs")
    .allowed({"k", "l", "m"});

REXCVAR_DEFINE_INT32(dlss_debug_view, 0, "GPU/Debug",
                     "Show DLSS's inputs instead of its output: 0 = off, 1 = motion vectors, "
                     "2 = depth")
    .range(0, 2);

REXCVAR_DEFINE_BOOL(dlss_jitter, true, "GPU/Debug",
                    "Jitter the scene for DLSS (off = no temporal anti-aliasing, to compare)");

REXCVAR_DEFINE_INT32(dlss_jitter_sign, 0, "GPU/Debug",
                     "Negate the jitter given to DLSS: bit 0 = X, bit 1 = Y (to check its "
                     "convention)")
    .range(0, 3);

namespace rex::graphics::d3d12 {

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/dlss_debug_cs.h"
#include "../shaders/bytecode/d3d12_5_1/dlss_inputs_cs.h"
}  // namespace shaders

namespace {

// A project ID of our own - NGX only wants a GUID-like string for custom
// engines.
constexpr char kNgxProjectId[] = "6c1f3b2e-8d4a-4f7b-9a35-2e81c0d47b19";

// Halton (2, 3) points, as recommended for DLSS - at least 16 phases for DLAA.
constexpr uint32_t kJitterPhases = 32;

float Halton(uint32_t index, uint32_t base) {
  float result = 0.0f;
  float fraction = 1.0f;
  while (index) {
    fraction /= float(base);
    result += fraction * float(index % base);
    index /= base;
  }
  return result;
}

// Inverse of a row-major 4x4 matrix (Gauss-Jordan with partial pivoting).
bool Invert4x4(const double* matrix, double* inverse) {
  double a[4][8];
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      a[row][column] = matrix[row * 4 + column];
      a[row][4 + column] = row == column ? 1.0 : 0.0;
    }
  }
  for (int column = 0; column < 4; ++column) {
    int pivot = column;
    for (int row = column + 1; row < 4; ++row) {
      if (std::abs(a[row][column]) > std::abs(a[pivot][column])) {
        pivot = row;
      }
    }
    if (std::abs(a[pivot][column]) < 1.0e-12) {
      return false;
    }
    if (pivot != column) {
      for (int i = 0; i < 8; ++i) {
        std::swap(a[pivot][i], a[column][i]);
      }
    }
    double scale = 1.0 / a[column][column];
    for (int i = 0; i < 8; ++i) {
      a[column][i] *= scale;
    }
    for (int row = 0; row < 4; ++row) {
      if (row != column) {
        double factor = a[row][column];
        for (int i = 0; i < 8; ++i) {
          a[row][i] -= factor * a[column][i];
        }
      }
    }
  }
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      inverse[row * 4 + column] = a[row][4 + column];
    }
  }
  return true;
}

struct InputsConstants {
  uint32_t size[2];
  uint32_t has_motion;
  uint32_t padding;
  float reprojection[16];
};

struct DebugConstants {
  uint32_t size[2];
  uint32_t view;
  uint32_t padding;
};

// Root signature: 32-bit constants at b0, then one descriptor table per SRV
// (t0...) and per UAV (u0...) - one-use descriptors aren't contiguous with
// bindless resources.
ID3D12RootSignature* CreateComputeRootSignature(const ui::d3d12::D3D12Provider& provider,
                                                uint32_t constant_count, uint32_t srv_count,
                                                uint32_t uav_count) {
  std::vector<D3D12_DESCRIPTOR_RANGE> ranges(srv_count + uav_count);
  std::vector<D3D12_ROOT_PARAMETER> parameters(1 + srv_count + uav_count);
  D3D12_ROOT_PARAMETER& constants = parameters[0];
  constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  constants.Constants.ShaderRegister = 0;
  constants.Constants.RegisterSpace = 0;
  constants.Constants.Num32BitValues = constant_count;
  constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  for (uint32_t i = 0; i < srv_count + uav_count; ++i) {
    bool uav = i >= srv_count;
    D3D12_DESCRIPTOR_RANGE& range = ranges[i];
    range.RangeType = uav ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = uav ? i - srv_count : i;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER& table = parameters[1 + i];
    table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table.DescriptorTable.NumDescriptorRanges = 1;
    table.DescriptorTable.pDescriptorRanges = &range;
    table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC desc;
  desc.NumParameters = UINT(parameters.size());
  desc.pParameters = parameters.data();
  desc.NumStaticSamplers = 0;
  desc.pStaticSamplers = nullptr;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  return ui::d3d12::util::CreateRootSignature(provider, desc);
}

void CreateTexture2DSrv(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format,
                        D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = format;
  desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(resource, &desc, handle);
}

void CreateTexture2DUav(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format,
                        D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
  desc.Format = format;
  desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(resource, nullptr, &desc, handle);
}

}  // namespace

D3D12Dlss::D3D12Dlss(D3D12CommandProcessor& command_processor)
    : command_processor_(command_processor) {}

D3D12Dlss::~D3D12Dlss() {
  Shutdown();
}

bool D3D12Dlss::Initialize() {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  *(inputs_root_signature_.ReleaseAndGetAddressOf()) =
      CreateComputeRootSignature(provider, sizeof(InputsConstants) / sizeof(uint32_t), 1, 2);
  *(debug_root_signature_.ReleaseAndGetAddressOf()) =
      CreateComputeRootSignature(provider, sizeof(DebugConstants) / sizeof(uint32_t), 2, 1);
  if (!inputs_root_signature_ || !debug_root_signature_) {
    REXGPU_ERROR("DLSS: failed to create the root signatures");
    return false;
  }
  *(inputs_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::dlss_inputs_cs, sizeof(shaders::dlss_inputs_cs),
      inputs_root_signature_.Get());
  *(debug_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::dlss_debug_cs, sizeof(shaders::dlss_debug_cs), debug_root_signature_.Get());
  if (!inputs_pipeline_ || !debug_pipeline_) {
    REXGPU_ERROR("DLSS: failed to create the compute pipelines");
    return false;
  }
  return true;
}

void D3D12Dlss::Shutdown() {
  ShutdownNgx();
  resources_to_release_.clear();
  depth_ = {};
  motion_ = {};
  output_ = {};
  debug_pipeline_.Reset();
  debug_root_signature_.Reset();
  inputs_pipeline_.Reset();
  inputs_root_signature_.Reset();
}

bool D3D12Dlss::IsEnabled() const {
#if defined(REX_HAS_DLSS)
  return inputs_pipeline_ && ngx_state_ != NgxState::kUnavailable && !feature_failed_ &&
         REXCVAR_GET(dlss_mode) == "dlaa";
#else
  return false;
#endif
}

bool D3D12Dlss::InitializeNgx() {
#if defined(REX_HAS_DLSS)
  if (ngx_state_ != NgxState::kNotInitialized) {
    return ngx_state_ == NgxState::kAvailable;
  }
  ngx_state_ = NgxState::kUnavailable;
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();

  // nvngx_dlss.dll sits next to the executable.
  std::wstring dll_folder = rex::filesystem::GetExecutableFolder().wstring();
  const wchar_t* dll_folders[] = {dll_folder.c_str()};
  NVSDK_NGX_FeatureCommonInfo feature_info = {};
  feature_info.PathListInfo.Path = dll_folders;
  feature_info.PathListInfo.Length = 1;
  // NGX wants a writable folder for its logs.
  std::error_code error;
  std::filesystem::path data_folder = std::filesystem::temp_directory_path(error) / "rexglue_ngx";
  std::filesystem::create_directories(data_folder, error);

  NVSDK_NGX_Result result =
      NVSDK_NGX_D3D12_Init_with_ProjectID(kNgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                                          data_folder.c_str(), device, &feature_info);
  if (NVSDK_NGX_FAILED(result)) {
    REXGPU_WARN("DLSS: not available - NGX initialization failed ({:08X}); it needs an NVIDIA "
                "RTX GPU",
                uint32_t(result));
    return false;
  }
  result = NVSDK_NGX_D3D12_GetCapabilityParameters(&ngx_parameters_);
  if (NVSDK_NGX_FAILED(result) || !ngx_parameters_) {
    REXGPU_WARN("DLSS: not available - no NGX capability parameters ({:08X})", uint32_t(result));
    ngx_parameters_ = nullptr;
    NVSDK_NGX_D3D12_Shutdown1(device);
    return false;
  }
  int needs_updated_driver = 0;
  int available = 0;
  NVSDK_NGX_Parameter_GetI(ngx_parameters_, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                           &needs_updated_driver);
  NVSDK_NGX_Parameter_GetI(ngx_parameters_, NVSDK_NGX_Parameter_SuperSampling_Available,
                           &available);
  if (!available || needs_updated_driver) {
    int init_result = 0;
    NVSDK_NGX_Parameter_GetI(ngx_parameters_, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult,
                             &init_result);
    REXGPU_WARN("DLSS: not available (available {}, needs a newer driver {}, init result {:08X})",
                available, needs_updated_driver, uint32_t(init_result));
    NVSDK_NGX_D3D12_DestroyParameters(ngx_parameters_);
    ngx_parameters_ = nullptr;
    NVSDK_NGX_D3D12_Shutdown1(device);
    return false;
  }
  ngx_state_ = NgxState::kAvailable;
  REXGPU_INFO("DLSS: NGX initialized, DLSS is available");
  return true;
#else
  ngx_state_ = NgxState::kUnavailable;
  return false;
#endif
}

void D3D12Dlss::ShutdownNgx() {
#if defined(REX_HAS_DLSS)
  for (auto& feature : features_to_release_) {
    NVSDK_NGX_D3D12_ReleaseFeature(feature.second);
  }
  features_to_release_.clear();
  if (feature_) {
    NVSDK_NGX_D3D12_ReleaseFeature(feature_);
    feature_ = nullptr;
  }
  feature_width_ = 0;
  feature_height_ = 0;
  if (ngx_state_ == NgxState::kAvailable) {
    if (ngx_parameters_) {
      NVSDK_NGX_D3D12_DestroyParameters(ngx_parameters_);
      ngx_parameters_ = nullptr;
    }
    NVSDK_NGX_D3D12_Shutdown1(command_processor_.GetD3D12Provider().GetDevice());
  }
#endif
  ngx_state_ = NgxState::kNotInitialized;
}

bool D3D12Dlss::EnsureTexture(Texture& texture, DXGI_FORMAT format, uint32_t width,
                              uint32_t height, const char* name) {
  if (texture.resource) {
    D3D12_RESOURCE_DESC desc = texture.resource->GetDesc();
    if (desc.Format == format && desc.Width == width && desc.Height == height) {
      return true;
    }
    resources_to_release_.emplace_back(command_processor_.GetCurrentSubmission(),
                                       std::move(texture.resource));
    texture = {};
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  texture.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if (FAILED(provider.GetDevice()->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
          texture.state, nullptr, IID_PPV_ARGS(&texture.resource)))) {
    REXGPU_ERROR("DLSS: failed to create the {} texture ({}x{})", name, width, height);
    texture = {};
    return false;
  }
  return true;
}

void D3D12Dlss::Transition(Texture& texture, D3D12_RESOURCE_STATES state) {
  if (texture.state != state) {
    command_processor_.PushTransitionBarrier(texture.resource.Get(), texture.state, state);
    texture.state = state;
  }
}

const std::array<float, 16>* D3D12Dlss::FrameCamera() const {
  const std::pair<std::array<float, 16>, uint32_t>* best = nullptr;
  for (const auto& camera : frame_cameras_) {
    if (!best || camera.second > best->second) {
      best = &camera;
    }
  }
  // A matrix used by a single draw may just be one object's.
  return best && best->second >= 2 ? &best->first : nullptr;
}

void D3D12Dlss::OnSceneDraw(const float* constants_c0_c3) {
  if (frame_scene_taken_ || !IsEnabled()) {
    return;
  }
  frame_scene_drawn_ = true;
  for (auto& camera : frame_cameras_) {
    if (!std::memcmp(camera.first.data(), constants_c0_c3, sizeof(float) * 16)) {
      ++camera.second;
      return;
    }
  }
  if (frame_cameras_.size() < 64) {
    auto& camera = frame_cameras_.emplace_back();
    std::memcpy(camera.first.data(), constants_c0_c3, sizeof(float) * 16);
    camera.second = 1;
  }
}

bool D3D12Dlss::WantsScene() const {
  return frame_scene_drawn_ && !frame_scene_taken_ && IsEnabled();
}

D3D12_RESOURCE_STATES D3D12Dlss::ProcessScene(ID3D12Resource* color,
                                              D3D12_CPU_DESCRIPTOR_HANDLE depth_srv,
                                              uint32_t width, uint32_t height) {
  D3D12_RESOURCE_STATES color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  frame_scene_taken_ = true;
#if defined(REX_HAS_DLSS)
  if (!width || !height || !InitializeNgx()) {
    return color_state;
  }
  D3D12_RESOURCE_DESC color_desc = color->GetDesc();
  if (color_desc.SampleDesc.Count != 1 || width > color_desc.Width ||
      height > color_desc.Height) {
    return color_state;
  }
  DXGI_FORMAT output_format = color_desc.Format;
  switch (output_format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      break;
    default: {
      static bool format_logged = false;
      if (!format_logged) {
        format_logged = true;
        REXGPU_WARN("DLSS: the scene's color format {} isn't supported", int(output_format));
      }
      return color_state;
    }
  }
  if (!EnsureTexture(depth_, DXGI_FORMAT_R32_FLOAT, width, height, "depth") ||
      !EnsureTexture(motion_, DXGI_FORMAT_R16G16_FLOAT, width, height, "motion vector") ||
      !EnsureTexture(output_, output_format, width, height, "output")) {
    return color_state;
  }

  // Camera motion: this frame's clip space -> the previous frame's.
  bool reset = !previous_processed_ || width != previous_width_ || height != previous_height_;
  InputsConstants inputs_constants = {};
  inputs_constants.size[0] = width;
  inputs_constants.size[1] = height;
  const std::array<float, 16>* camera = FrameCamera();
  frame_camera_valid_ = camera != nullptr;
  if (camera) {
    frame_camera_ = *camera;
  }
  if (camera && previous_camera_valid_) {
    double current[16], previous[16], current_inverse[16];
    for (int i = 0; i < 16; ++i) {
      current[i] = (*camera)[i];
      previous[i] = previous_camera_[i];
    }
    if (Invert4x4(current, current_inverse)) {
      double reprojection[16];
      for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
          double sum = 0.0;
          for (int i = 0; i < 4; ++i) {
            sum += current_inverse[row * 4 + i] * previous[i * 4 + column];
          }
          reprojection[row * 4 + column] = sum;
        }
      }
      // A camera cut: points of the screen moving by more than a quarter of
      // it at once.
      bool cut = false;
      for (double depth_value : {0.5, 0.99}) {
        for (double y : {-0.8, 0.0, 0.8}) {
          for (double x : {-0.8, 0.0, 0.8}) {
            double point[4] = {x, y, depth_value, 1.0};
            double moved[4];
            for (int column = 0; column < 4; ++column) {
              moved[column] = point[0] * reprojection[column] + point[1] * reprojection[4 + column] +
                              point[2] * reprojection[8 + column] + reprojection[12 + column];
            }
            if (moved[3] <= 1.0e-6 || std::abs(moved[0] / moved[3] - x) > 0.5 ||
                std::abs(moved[1] / moved[3] - y) > 0.5) {
              cut = true;
            }
          }
        }
      }
      if (cut) {
        reset = true;
      } else {
        inputs_constants.has_motion = 1;
        for (int i = 0; i < 16; ++i) {
          inputs_constants.reprojection[i] = float(reprojection[i]);
        }
      }
    }
  }
  if (!inputs_constants.has_motion) {
    reset = true;
  }
  // The GPU-side scene projection (online field of view, free camera roll)
  // isn't in the game's matrices - no history then.
  if (rex::graphics::GetSceneProjectionScale() != 1.0f ||
      rex::graphics::GetSceneProjectionRoll() != 0.0f) {
    reset = true;
  }

  D3D12CommandProcessor& command_processor = command_processor_;
  ID3D12Device* device = command_processor.GetD3D12Provider().GetDevice();
  DeferredCommandList& command_list = command_processor.GetDeferredCommandList();
  uint32_t group_count_x = (width + 7) / 8;
  uint32_t group_count_y = (height + 7) / 8;

  // Depth and motion vectors.
  ui::d3d12::util::DescriptorCpuGpuHandlePair inputs_descriptors[3];
  if (!command_processor.RequestOneUseSingleViewDescriptors(3, inputs_descriptors)) {
    return color_state;
  }
  device->CopyDescriptorsSimple(1, inputs_descriptors[0].first, depth_srv,
                                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  CreateTexture2DUav(device, depth_.resource.Get(), DXGI_FORMAT_R32_FLOAT,
                     inputs_descriptors[1].first);
  CreateTexture2DUav(device, motion_.resource.Get(), DXGI_FORMAT_R16G16_FLOAT,
                     inputs_descriptors[2].first);
  Transition(depth_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Transition(motion_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  command_processor.SubmitBarriers();
  command_processor.SetExternalPipeline(inputs_pipeline_.Get());
  command_list.D3DSetComputeRootSignature(inputs_root_signature_.Get());
  command_list.D3DSetComputeRoot32BitConstants(0, sizeof(inputs_constants) / sizeof(uint32_t),
                                               &inputs_constants, 0);
  command_list.D3DSetComputeRootDescriptorTable(1, inputs_descriptors[0].second);
  command_list.D3DSetComputeRootDescriptorTable(2, inputs_descriptors[1].second);
  command_list.D3DSetComputeRootDescriptorTable(3, inputs_descriptors[2].second);
  command_list.D3DDispatch(group_count_x, group_count_y, 1);
  Transition(depth_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(motion_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  command_processor.SubmitBarriers();

  int32_t debug_view = REXCVAR_GET(dlss_debug_view);
  if (debug_view) {
    ui::d3d12::util::DescriptorCpuGpuHandlePair debug_descriptors[3];
    if (!command_processor.RequestOneUseSingleViewDescriptors(3, debug_descriptors)) {
      return color_state;
    }
    CreateTexture2DSrv(device, motion_.resource.Get(), DXGI_FORMAT_R16G16_FLOAT,
                       debug_descriptors[0].first);
    CreateTexture2DSrv(device, depth_.resource.Get(), DXGI_FORMAT_R32_FLOAT,
                       debug_descriptors[1].first);
    CreateTexture2DUav(device, output_.resource.Get(), output_format, debug_descriptors[2].first);
    DebugConstants debug_constants = {};
    debug_constants.size[0] = width;
    debug_constants.size[1] = height;
    debug_constants.view = uint32_t(debug_view);
    command_processor.SetExternalPipeline(debug_pipeline_.Get());
    command_list.D3DSetComputeRootSignature(debug_root_signature_.Get());
    command_list.D3DSetComputeRoot32BitConstants(0, sizeof(debug_constants) / sizeof(uint32_t),
                                                 &debug_constants, 0);
    command_list.D3DSetComputeRootDescriptorTable(1, debug_descriptors[0].second);
    command_list.D3DSetComputeRootDescriptorTable(2, debug_descriptors[1].second);
    command_list.D3DSetComputeRootDescriptorTable(3, debug_descriptors[2].second);
    command_list.D3DDispatch(group_count_x, group_count_y, 1);
  } else {
    const std::string& preset_name = REXCVAR_GET(dlss_preset);
    uint32_t preset = preset_name == "k"   ? uint32_t(NVSDK_NGX_DLSS_Hint_Render_Preset_K)
                      : preset_name == "l" ? uint32_t(NVSDK_NGX_DLSS_Hint_Render_Preset_L)
                                           : uint32_t(NVSDK_NGX_DLSS_Hint_Render_Preset_M);
    bool create = !feature_ || feature_width_ != width || feature_height_ != height ||
                  feature_output_format_ != output_format || feature_preset_ != preset;
    if (create) {
      if (feature_) {
        features_to_release_.emplace_back(command_processor.GetCurrentSubmission(), feature_);
        feature_ = nullptr;
      }
      feature_width_ = width;
      feature_height_ = height;
      feature_output_format_ = output_format;
      feature_preset_ = preset;
      reset = true;
    }
    int32_t jitter_sign = REXCVAR_GET(dlss_jitter_sign);
    float jitter_x = (jitter_sign & 1) ? -jitter_[0] : jitter_[0];
    float jitter_y = (jitter_sign & 2) ? -jitter_[1] : jitter_[1];
    ID3D12Resource* depth_resource = depth_.resource.Get();
    ID3D12Resource* motion_resource = motion_.resource.Get();
    ID3D12Resource* output_resource = output_.resource.Get();
    command_list.ExternalCallback([this, create, preset, color, depth_resource, motion_resource,
                                   output_resource, width, height, jitter_x, jitter_y,
                                   reset](ID3D12GraphicsCommandList* d3d_command_list) {
      if (create) {
        NVSDK_NGX_Parameter_SetUI(ngx_parameters_, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                                  preset);
        NVSDK_NGX_DLSS_Create_Params create_params = {};
        create_params.Feature.InWidth = width;
        create_params.Feature.InHeight = height;
        create_params.Feature.InTargetWidth = width;
        create_params.Feature.InTargetHeight = height;
        create_params.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
        // Motion vectors at the render resolution, without the jitter; LDR
        // color; depth 0 = near.
        create_params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSS_EXT(d3d_command_list, 1, 1, &feature_,
                                                            ngx_parameters_, &create_params);
        if (NVSDK_NGX_FAILED(result) || !feature_) {
          REXGPU_ERROR("DLSS: failed to create the DLAA feature ({}x{}, {:08X})", width, height,
                       uint32_t(result));
          feature_ = nullptr;
          feature_failed_ = true;
          return;
        }
        REXGPU_INFO("DLSS: DLAA at {}x{}, preset {}", width, height, char('A' + preset - 1));
      }
      if (!feature_) {
        return;
      }
      NVSDK_NGX_D3D12_DLSS_Eval_Params eval_params = {};
      eval_params.Feature.pInColor = color;
      eval_params.Feature.pInOutput = output_resource;
      eval_params.pInDepth = depth_resource;
      eval_params.pInMotionVectors = motion_resource;
      eval_params.InJitterOffsetX = jitter_x;
      eval_params.InJitterOffsetY = jitter_y;
      eval_params.InRenderSubrectDimensions.Width = width;
      eval_params.InRenderSubrectDimensions.Height = height;
      eval_params.InReset = reset ? 1 : 0;
      eval_params.InMVScaleX = 1.0f;
      eval_params.InMVScaleY = 1.0f;
      NVSDK_NGX_Result result =
          NGX_D3D12_EVALUATE_DLSS_EXT(d3d_command_list, feature_, ngx_parameters_, &eval_params);
      if (NVSDK_NGX_FAILED(result)) {
        REXGPU_ERROR("DLSS: evaluation failed ({:08X})", uint32_t(result));
        feature_failed_ = true;
      }
    });
    command_processor.InvalidateStateAfterExternalCommands();
  }

  // The output replaces the scene in the color render target.
  Transition(output_, D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor.PushTransitionBarrier(color, color_state, D3D12_RESOURCE_STATE_COPY_DEST);
  color_state = D3D12_RESOURCE_STATE_COPY_DEST;
  command_processor.SubmitBarriers();
  D3D12_TEXTURE_COPY_LOCATION copy_dest;
  copy_dest.pResource = color;
  copy_dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  copy_dest.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION copy_source;
  copy_source.pResource = output_.resource.Get();
  copy_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  copy_source.SubresourceIndex = 0;
  D3D12_BOX copy_box = {0, 0, 0, width, height, 1};
  command_list.D3DCopyTextureRegion(&copy_dest, 0, 0, 0, &copy_source, &copy_box);

  frame_processed_ = true;
  previous_width_ = width;
  previous_height_ = height;
#endif
  return color_state;
}

void D3D12Dlss::EndFrame() {
  uint64_t completed_submission = command_processor_.GetCompletedSubmission();
  std::erase_if(resources_to_release_, [completed_submission](const auto& resource) {
    return resource.first <= completed_submission;
  });
#if defined(REX_HAS_DLSS)
  std::erase_if(features_to_release_, [completed_submission](const auto& feature) {
    if (feature.first > completed_submission) {
      return false;
    }
    NVSDK_NGX_D3D12_ReleaseFeature(feature.second);
    return true;
  });
#endif

  previous_processed_ = frame_processed_;
  previous_camera_ = frame_camera_;
  previous_camera_valid_ = frame_processed_ && frame_camera_valid_;

  // Only jitter the next frame if DLSS ran in this one - in frames it doesn't
  // see (menus without a 3D scene, the HUD hidden) the jitter would just make
  // the picture wobble.
  if (frame_processed_ && IsEnabled() && REXCVAR_GET(dlss_jitter)) {
    jitter_index_ = (jitter_index_ + 1) % kJitterPhases;
    jitter_[0] = Halton(jitter_index_ + 1, 2) - 0.5f;
    jitter_[1] = Halton(jitter_index_ + 1, 3) - 0.5f;
  } else {
    jitter_[0] = 0.0f;
    jitter_[1] = 0.0f;
  }

  frame_scene_drawn_ = false;
  frame_scene_taken_ = false;
  frame_processed_ = false;
  frame_cameras_.clear();
  frame_camera_valid_ = false;
}

}  // namespace rex::graphics::d3d12
