/**
 * @file        graphics/d3d12/dlss.cpp
 * @brief       NVIDIA DLSS and AMD FSR (anti-aliasing and upscaling) for the 3D
 *              scene, run before the HUD is drawn
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

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/draw_overrides.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "ffx_upscaler.h"

#if defined(REX_HAS_DLSS)
#include <nvsdk_ngx_helpers.h>
#endif

REXCVAR_DEFINE_STRING(dlss_mode, "off", "GPU",
                      "NVIDIA DLSS for the 3D scene (RTX GPUs): off; dlaa - anti-aliasing at "
                      "the draw resolution scale; quality, balanced, performance, "
                      "ultra_performance - the game renders below the draw resolution scale and "
                      "DLSS upscales the scene to it (the HUD is drawn at it). Only whole scales "
                      "exist: at 3x, quality / balanced / performance render at 2x and "
                      "ultra_performance at 1x; at 2x and 4x every mode renders at half")
    .allowed({"off", "dlaa", "quality", "balanced", "performance", "ultra_performance"});

REXCVAR_DEFINE_STRING(dlss_preset, "m", "GPU",
                      "DLSS model (NVIDIA's render presets): k (NVIDIA's default for DLAA and "
                      "Quality), l, or m - sharper and more stable than k, at about the same cost "
                      "on RTX 40 GPUs")
    .allowed({"k", "l", "m"});

REXCVAR_DEFINE_INT32(dlss_debug_view, 0, "GPU/Debug",
                     "Show DLSS's inputs instead of its output: 0 = off, 1 = motion vectors, "
                     "2 = depth; 3 = with upscaling, tint red what's presented from the game's "
                     "own (not upscaled) frame")
    .range(0, 3);

REXCVAR_DEFINE_BOOL(dlss_jitter, true, "GPU/Debug",
                    "Jitter the scene for DLSS (off = no temporal anti-aliasing, to compare)");

REXCVAR_DEFINE_INT32(dlss_jitter_sign, 0, "GPU/Debug",
                     "Negate the jitter given to DLSS: bit 0 = X, bit 1 = Y (to check its "
                     "convention)")
    .range(0, 3);

REXCVAR_DEFINE_STRING(fsr_mode, "off", "GPU",
                      "AMD FSR for the 3D scene - FSR 4 on GPUs that have it (RX 9000, RX 7000), "
                      "FSR 3.1.5 elsewhere - used while dlss_mode is off: off; native_aa - "
                      "anti-aliasing at the draw resolution scale; quality, balanced, "
                      "performance, ultra_performance - the game renders below the draw "
                      "resolution scale and FSR upscales the scene to it (the HUD is drawn at "
                      "it). Only whole scales exist, at most 3x below")
    .allowed({"off", "native_aa", "quality", "balanced", "performance", "ultra_performance"});

REXCVAR_DEFINE_DOUBLE(fsr_sharpness, 0.0, "GPU",
                      "AMD FSR's sharpening, 0 (off) to 1")
    .range(0.0, 1.0);

REXCVAR_DEFINE_STRING(fsr_version, "auto", "GPU",
                      "AMD FSR version: auto (the newest the GPU has), 4 or 3 (if the GPU has it)")
    .allowed({"auto", "4", "3"});

REXCVAR_DEFINE_BOOL(fsr_debug_view, false, "GPU/Debug", "Show AMD FSR's own debug view");

REXCVAR_DEFINE_INT32(fsr_jitter_sign, 0, "GPU/Debug",
                     "Negate the jitter given to AMD FSR: bit 0 = X, bit 1 = Y (to check its "
                     "convention)")
    .range(0, 3);

namespace rex::graphics::d3d12 {

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/dlss_debug_cs.h"
#include "../shaders/bytecode/d3d12_5_1/dlss_downsample_cs.h"
#include "../shaders/bytecode/d3d12_5_1/dlss_inputs_cs.h"
#include "../shaders/bytecode/d3d12_5_1/dlss_present_compare_cs.h"
#include "../shaders/bytecode/d3d12_5_1/dlss_present_cs.h"
}  // namespace shaders

namespace {

// A project ID of our own - NGX only wants a GUID-like string for custom
// engines.
constexpr char kNgxProjectId[] = "6c1f3b2e-8d4a-4f7b-9a35-2e81c0d47b19";

// Halton (2, 3) points, as recommended for DLSS: 8 * (output / render)^2
// phases, at least 32.
constexpr uint32_t kMinJitterPhases = 32;

// The picture presented with upscaling.
constexpr DXGI_FORMAT kPresentFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

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

struct DownsampleConstants {
  uint32_t size[2];
  float ratio[2];
  float source_size_inv[2];
  float padding[2];
};

struct CompareConstants {
  uint32_t size[2];
  float tolerance;
  uint32_t border;
};

struct PresentConstants {
  uint32_t size[2];
  uint32_t frame_size[2];
  float frame_ratio[2];
  float frame_texture_size_inv[2];
  uint32_t mode;
  uint32_t padding[3];
};

// Root signature: 32-bit constants at b0, then one descriptor table per SRV
// (t0...) and per UAV (u0...) - one-use descriptors aren't contiguous with
// bindless resources - and optionally a bilinear clamping sampler at s0.
ID3D12RootSignature* CreateComputeRootSignature(const ui::d3d12::D3D12Provider& provider,
                                                uint32_t constant_count, uint32_t srv_count,
                                                uint32_t uav_count, bool linear_sampler = false) {
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
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderRegister = 0;
  sampler.RegisterSpace = 0;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC desc;
  desc.NumParameters = UINT(parameters.size());
  desc.pParameters = parameters.data();
  desc.NumStaticSamplers = linear_sampler ? 1 : 0;
  desc.pStaticSamplers = linear_sampler ? &sampler : nullptr;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  return ui::d3d12::util::CreateRootSignature(provider, desc);
}

// What FSR wants to know of the camera, from the scene's view * projection
// matrix (row vectors): with a rigid view and a perspective projection, the
// depth is k + c / w, w being the distance along the view direction.
struct CameraParameters {
  float near_plane = 0.1f;
  float far_plane = 1000.0f;
  float fov_y = 1.047f;
  bool inverted = false;
  bool infinite = false;
  bool valid = false;
};

CameraParameters GetCameraParameters(const std::array<float, 16>& matrix) {
  CameraParameters parameters;
  auto m = [&matrix](int row, int column) { return double(matrix[row * 4 + column]); };
  // The row with the most of w, for k.
  int row = 0;
  for (int i = 1; i < 3; ++i) {
    if (std::abs(m(i, 3)) > std::abs(m(row, 3))) {
      row = i;
    }
  }
  if (std::abs(m(row, 3)) < 1.0e-6) {
    return parameters;
  }
  const double k = m(row, 2) / m(row, 3);
  const double c = m(3, 2) - k * m(3, 3);
  const double column1 = std::sqrt(m(0, 1) * m(0, 1) + m(1, 1) * m(1, 1) + m(2, 1) * m(2, 1));
  const double column3 = std::sqrt(m(0, 3) * m(0, 3) + m(1, 3) * m(1, 3) + m(2, 3) * m(2, 3));
  if (std::abs(c) < 1.0e-9 || column1 < 1.0e-9) {
    return parameters;
  }
  // The depth grows with the distance (c < 0) or falls (c > 0, inverted).
  // Where it's 0 and 1: the near and far planes, the far one possibly at
  // infinity (or past it, when the projection was tweaked for that).
  const bool inverted = c > 0.0;
  const double w0 = std::abs(k) > 1.0e-9 ? -c / k : HUGE_VAL;
  const double w1 = std::abs(1.0 - k) > 1.0e-9 ? c / (1.0 - k) : HUGE_VAL;
  const double near_plane = inverted ? w1 : w0;
  const double far_plane = inverted ? w0 : w1;
  const double fov_y = 2.0 * std::atan(column3 / column1);
  if (!(near_plane > 0.0) || !std::isfinite(near_plane) || !(fov_y > 0.05 && fov_y < 3.0)) {
    return parameters;
  }
  const bool infinite = !std::isfinite(far_plane) || !(far_plane > near_plane);
  parameters.inverted = inverted;
  parameters.infinite = infinite;
  parameters.near_plane = float(near_plane);
  parameters.far_plane = infinite ? 1.0e6f : float(far_plane);
  parameters.fov_y = float(fov_y);
  parameters.valid = true;
  return parameters;
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

// The depth / stencil view format of a depth render target's resource format.
DXGI_FORMAT GetDsvFormat(DXGI_FORMAT resource_format) {
  switch (resource_format) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

}  // namespace

D3D12Dlss::D3D12Dlss(D3D12CommandProcessor& command_processor)
    : command_processor_(command_processor) {}

D3D12Dlss::~D3D12Dlss() {
  Shutdown();
}

bool D3D12Dlss::Initialize() {
  rex::graphics::SetDlssAvailability(rex::graphics::DlssAvailability::kUnavailable);
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  *(inputs_root_signature_.ReleaseAndGetAddressOf()) =
      CreateComputeRootSignature(provider, sizeof(InputsConstants) / sizeof(uint32_t), 1, 2);
  *(debug_root_signature_.ReleaseAndGetAddressOf()) =
      CreateComputeRootSignature(provider, sizeof(DebugConstants) / sizeof(uint32_t), 2, 1);
  *(downsample_root_signature_.ReleaseAndGetAddressOf()) = CreateComputeRootSignature(
      provider, sizeof(DownsampleConstants) / sizeof(uint32_t), 1, 1, true);
  *(compare_root_signature_.ReleaseAndGetAddressOf()) =
      CreateComputeRootSignature(provider, sizeof(CompareConstants) / sizeof(uint32_t), 2, 1);
  *(present_root_signature_.ReleaseAndGetAddressOf()) = CreateComputeRootSignature(
      provider, sizeof(PresentConstants) / sizeof(uint32_t), 3, 1, true);
  if (!inputs_root_signature_ || !debug_root_signature_ || !downsample_root_signature_ ||
      !compare_root_signature_ || !present_root_signature_) {
    REXGPU_ERROR("DLSS: failed to create the root signatures");
    return false;
  }
  *(inputs_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::dlss_inputs_cs, sizeof(shaders::dlss_inputs_cs),
      inputs_root_signature_.Get());
  *(debug_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::dlss_debug_cs, sizeof(shaders::dlss_debug_cs), debug_root_signature_.Get());
  *(downsample_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::dlss_downsample_cs, sizeof(shaders::dlss_downsample_cs),
      downsample_root_signature_.Get());
  *(compare_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::dlss_present_compare_cs, sizeof(shaders::dlss_present_compare_cs),
      compare_root_signature_.Get());
  *(present_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::dlss_present_cs, sizeof(shaders::dlss_present_cs),
      present_root_signature_.Get());
  if (!inputs_pipeline_ || !debug_pipeline_ || !downsample_pipeline_ || !compare_pipeline_ ||
      !present_pipeline_) {
    REXGPU_ERROR("DLSS: failed to create the compute pipelines");
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap_desc.NumDescriptors = 1;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&hud_rtv_heap_)))) {
    REXGPU_ERROR("DLSS: failed to create the RTV heap");
    return false;
  }
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&hud_dsv_heap_)))) {
    REXGPU_ERROR("DLSS: failed to create the DSV heap");
    return false;
  }

  // Whether DLSS works decides the render scale of its upscaling modes, so NGX
  // is checked right away on NVIDIA GPUs.
  if (provider.GetAdapterVendorID() != ui::GraphicsProvider::GpuVendorID::kNvidia) {
    ngx_state_ = NgxState::kUnavailable;
  } else if (InitializeNgx()) {
    rex::graphics::SetDlssAvailability(rex::graphics::DlssAvailability::kAvailable);
  }
  // FSR's DLLs are only loaded once it's chosen (also later, in EndFrame).
  rex::graphics::SetFsrAvailability(rex::graphics::FsrAvailability::kUnavailable);
  if (REXCVAR_GET(fsr_mode) != "off") {
    TryInitializeFsr();
  }
  return true;
}

void D3D12Dlss::OnHostRenderTargetsUnavailable() {
  if (rex::graphics::GetDlssAvailability() != rex::graphics::DlssAvailability::kUnavailable) {
    REXGPU_WARN("DLSS: not available with the pixel shader interlock render target path");
    rex::graphics::SetDlssAvailability(rex::graphics::DlssAvailability::kUnavailable);
  }
  feature_failed_ = true;
  fsr_blocked_ = true;
  if (ffx_) {
    REXGPU_WARN("AMD FSR: not available with the pixel shader interlock render target path");
    ffx_->Shutdown();
    ffx_.reset();
  }
  rex::graphics::SetFsrAvailability(rex::graphics::FsrAvailability::kUnavailable);
  D3D12CommandProcessor::RequestDrawResolutionScaleFromSettings();
}

void D3D12Dlss::TryInitializeFsr() {
  if (fsr_tried_ || fsr_blocked_) {
    return;
  }
  fsr_tried_ = true;
  ffx_ = std::make_unique<D3D12FfxUpscaler>();
  if (!ffx_->Initialize(command_processor_.GetD3D12Provider().GetDevice())) {
    ffx_.reset();
    return;
  }
  rex::graphics::SetFsrAvailability(rex::graphics::FsrAvailability::kAvailable);
}

void D3D12Dlss::Shutdown() {
  rex::graphics::SetDlssAvailability(rex::graphics::DlssAvailability::kUnavailable);
  rex::graphics::SetFsrAvailability(rex::graphics::FsrAvailability::kUnavailable);
  ShutdownNgx();
  if (ffx_) {
    ffx_->Shutdown();
    ffx_.reset();
  }
  fsr_tried_ = false;
  resources_to_release_.clear();
  fsr_color_ = {};
  depth_ = {};
  motion_ = {};
  output_ = {};
  upscaled_ = {};
  hud_depth_ = {};
  frozen_ = {};
  tiles_ = {};
  present_ = {};
  hud_dsv_heap_.Reset();
  hud_rtv_heap_.Reset();
  present_pipeline_.Reset();
  present_root_signature_.Reset();
  compare_pipeline_.Reset();
  compare_root_signature_.Reset();
  downsample_pipeline_.Reset();
  downsample_root_signature_.Reset();
  debug_pipeline_.Reset();
  debug_root_signature_.Reset();
  inputs_pipeline_.Reset();
  inputs_root_signature_.Reset();
}

D3D12Dlss::Backend D3D12Dlss::SelectBackend() const {
  if (!inputs_pipeline_) {
    return Backend::kNone;
  }
#if defined(REX_HAS_DLSS)
  if (ngx_state_ != NgxState::kUnavailable && !feature_failed_ &&
      REXCVAR_GET(dlss_mode) != "off") {
    return Backend::kDlss;
  }
#endif
  if (ffx_ && ffx_->available() && !fsr_failed_ && !fsr_blocked_ &&
      REXCVAR_GET(fsr_mode) != "off") {
    return Backend::kFsr;
  }
  return Backend::kNone;
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
  // Features released (on a mode or size change) give their memory back.
  NVSDK_NGX_Parameter_SetI(ngx_parameters_, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
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
  feature_output_width_ = 0;
  feature_output_height_ = 0;
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
                              uint32_t height, const char* name, D3D12_RESOURCE_FLAGS flags) {
  if (texture.resource) {
    D3D12_RESOURCE_DESC desc = texture.resource->GetDesc();
    if (desc.Format == format && desc.Width == width && desc.Height == height &&
        desc.Flags == flags) {
      return true;
    }
    ReleaseLater(texture);
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
  desc.Flags = flags;
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

bool D3D12Dlss::EnsureHudDepth(DXGI_FORMAT resource_format, uint32_t width, uint32_t height) {
  const DXGI_FORMAT dsv_format = GetDsvFormat(resource_format);
  if (dsv_format == DXGI_FORMAT_UNKNOWN) {
    return false;
  }
  if (hud_depth_.resource) {
    D3D12_RESOURCE_DESC desc = hud_depth_.resource->GetDesc();
    if (desc.Format == resource_format && desc.Width == width && desc.Height == height) {
      return true;
    }
    ReleaseLater(hud_depth_);
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = resource_format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE clear_value = {};
  clear_value.Format = dsv_format;
  hud_depth_.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  if (FAILED(provider.GetDevice()->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
          hud_depth_.state, &clear_value, IID_PPV_ARGS(&hud_depth_.resource)))) {
    REXGPU_ERROR("DLSS: failed to create the HUD depth texture ({}x{})", width, height);
    hud_depth_ = {};
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

void D3D12Dlss::ReleaseLater(Texture& texture) {
  if (texture.resource) {
    resources_to_release_.emplace_back(command_processor_.GetCurrentSubmission(),
                                       std::move(texture.resource));
  }
  texture = {};
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

void D3D12Dlss::DisableUpscaling(Backend backend, const char* reason) {
  if (backend == Backend::kFsr) {
    if (fsr_upscaling_failed_) {
      return;
    }
    fsr_upscaling_failed_ = true;
    REXGPU_WARN("AMD FSR: upscaling doesn't work ({}) - anti-aliasing at the configured "
                "resolution instead",
                reason);
    if (rex::graphics::GetFsrAvailability() == rex::graphics::FsrAvailability::kAvailable) {
      rex::graphics::SetFsrAvailability(rex::graphics::FsrAvailability::kNativeOnly);
    }
  } else {
    if (upscaling_failed_) {
      return;
    }
    upscaling_failed_ = true;
    REXGPU_WARN("DLSS: upscaling doesn't work ({}) - DLAA at the configured resolution instead",
                reason);
    if (rex::graphics::GetDlssAvailability() == rex::graphics::DlssAvailability::kAvailable) {
      rex::graphics::SetDlssAvailability(rex::graphics::DlssAvailability::kDlaaOnly);
    }
  }
  D3D12CommandProcessor::RequestDrawResolutionScaleFromSettings();
}

void D3D12Dlss::DisableFsr() {
  if (fsr_failed_) {
    return;
  }
  fsr_failed_ = true;
  rex::graphics::SetFsrAvailability(rex::graphics::FsrAvailability::kUnavailable);
  D3D12CommandProcessor::RequestDrawResolutionScaleFromSettings();
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

D3D12_RESOURCE_STATES D3D12Dlss::ProcessScene(ID3D12Resource* color, ID3D12Resource* depth,
                                              D3D12_CPU_DESCRIPTOR_HANDLE depth_srv,
                                              uint32_t width, uint32_t height,
                                              uint32_t output_width, uint32_t output_height) {
  D3D12_RESOURCE_STATES color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  frame_scene_taken_ = true;
#if defined(REX_HAS_DLSS) || defined(REX_HAS_FSR_SDK)
  const Backend backend = SelectBackend();
  if (!width || !height || backend == Backend::kNone) {
    return color_state;
  }
#if defined(REX_HAS_DLSS)
  if (backend == Backend::kDlss && !InitializeNgx()) {
    return color_state;
  }
#endif
  const char* backend_name = backend == Backend::kDlss ? "DLSS" : "AMD FSR";
  // A scene the upscaler can't take isn't rendered below the configured scale
  // either.
  const bool upscaling_requested = output_width != width || output_height != height;
  D3D12_RESOURCE_DESC color_desc = color->GetDesc();
  if (color_desc.SampleDesc.Count != 1 || width > color_desc.Width ||
      height > color_desc.Height) {
    if (upscaling_requested) {
      DisableUpscaling(backend, "the scene is multisampled or smaller than expected");
    }
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
        REXGPU_WARN("{}: the scene's color format {} isn't supported", backend_name,
                    int(output_format));
      }
      if (upscaling_requested) {
        DisableUpscaling(backend, "unsupported scene color format");
      }
      return color_state;
    }
  }

  // Upscaling: DLSS only at its own ratios (Quality, Performance, Ultra
  // Performance), FSR at up to 3x; with the output's aspect ratio. The debug
  // views show the inputs at the render resolution.
  int32_t debug_view = REXCVAR_GET(dlss_debug_view);
  bool upscale =
      (output_width != width || output_height != height) &&
      !(backend == Backend::kDlss ? upscaling_failed_ : fsr_upscaling_failed_) &&
      (debug_view == 0 || debug_view == 3);
  uint32_t quality = 0;
#if defined(REX_HAS_DLSS)
  quality = uint32_t(NVSDK_NGX_PerfQuality_Value_DLAA);
#endif
  if (upscale) {
    if (uint64_t(output_width) * height != uint64_t(output_height) * width) {
      upscale = false;
    } else if (backend == Backend::kFsr) {
      upscale = output_width <= 3 * width;
#if defined(REX_HAS_DLSS)
    } else if (3 * width == 2 * output_width) {
      quality = uint32_t(NVSDK_NGX_PerfQuality_Value_MaxQuality);
    } else if (2 * width == output_width) {
      quality = uint32_t(NVSDK_NGX_PerfQuality_Value_MaxPerf);
    } else if (3 * width == output_width) {
      quality = uint32_t(NVSDK_NGX_PerfQuality_Value_UltraPerformance);
#endif
    } else {
      upscale = false;
    }
    if (!upscale) {
      // Switching between the upscalers, the render scale of the other one may
      // still be there for a few frames - nothing for this frame then.
      if (command_processor_.IsDrawResolutionScaleChangePending()) {
        return color_state;
      }
      DisableUpscaling(backend,
                       fmt::format("{}x{} to {}x{} isn't a {} ratio", width, height,
                                   output_width, output_height, backend_name)
                           .c_str());
    }
  }
  if (!upscale) {
    output_width = width;
    output_height = height;
    debug_view = debug_view == 3 ? 0 : debug_view;
  }
  DXGI_FORMAT depth_dsv_format = upscale ? GetDsvFormat(depth->GetDesc().Format) : DXGI_FORMAT_UNKNOWN;
  if (upscale && depth_dsv_format == DXGI_FORMAT_UNKNOWN) {
    DisableUpscaling(backend, "unknown depth format");
    return color_state;
  }
  if (!EnsureTexture(depth_, DXGI_FORMAT_R32_FLOAT, width, height, "depth") ||
      !EnsureTexture(motion_, DXGI_FORMAT_R16G16_FLOAT, width, height, "motion vector") ||
      !EnsureTexture(output_, output_format, width, height, "output")) {
    return color_state;
  }
  if (upscale &&
      (!EnsureTexture(upscaled_, output_format, output_width, output_height, "upscaled",
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
       !EnsureHudDepth(depth->GetDesc().Format, output_width, output_height))) {
    return color_state;
  }

  // Camera motion: this frame's clip space -> the previous frame's.
  bool reset = !previous_processed_ || width != previous_width_ || height != previous_height_ ||
               output_width != previous_output_width_ || output_height != previous_output_height_;
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
  Texture& dlss_output = upscale ? upscaled_ : output_;
  Transition(depth_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(motion_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(dlss_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  command_processor.SubmitBarriers();

  bool create = false;
  if (debug_view == 1 || debug_view == 2) {
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
  } else if (backend == Backend::kFsr) {
    if (!RecordFsr(color, color_desc.Format, width, height, output_width, output_height, upscale,
                   reset, dlss_output.resource.Get())) {
      return color_state;
    }
  } else {
#if defined(REX_HAS_DLSS)
    const std::string& preset_name = REXCVAR_GET(dlss_preset);
    uint32_t preset = preset_name == "k"   ? uint32_t(NVSDK_NGX_DLSS_Hint_Render_Preset_K)
                      : preset_name == "l" ? uint32_t(NVSDK_NGX_DLSS_Hint_Render_Preset_L)
                                           : uint32_t(NVSDK_NGX_DLSS_Hint_Render_Preset_M);
    create = !feature_ || feature_width_ != width || feature_height_ != height ||
             feature_output_width_ != output_width || feature_output_height_ != output_height ||
             feature_output_format_ != output_format || feature_preset_ != preset;
    if (create) {
      if (feature_) {
        features_to_release_.emplace_back(command_processor.GetCurrentSubmission(), feature_);
        feature_ = nullptr;
      }
      feature_width_ = width;
      feature_height_ = height;
      feature_output_width_ = output_width;
      feature_output_height_ = output_height;
      feature_output_format_ = output_format;
      feature_preset_ = preset;
      reset = true;
      if (upscale) {
        // The render size has to be in the mode's range (it's the optimal
        // size of the mode at DLSS's own ratios).
        unsigned int optimal_width = 0, optimal_height = 0, max_width = 0, max_height = 0,
                     min_width = 0, min_height = 0;
        float sharpness = 0.0f;
        NVSDK_NGX_Result result = NGX_DLSS_GET_OPTIMAL_SETTINGS(
            ngx_parameters_, output_width, output_height, NVSDK_NGX_PerfQuality_Value(quality),
            &optimal_width, &optimal_height, &max_width, &max_height, &min_width, &min_height,
            &sharpness);
        REXGPU_INFO("DLSS: mode {} for {}x{}: optimal {}x{}, range {}x{} to {}x{} ({:08X})",
                    quality, output_width, output_height, optimal_width, optimal_height,
                    min_width, min_height, max_width, max_height, uint32_t(result));
      }
    }
    int32_t jitter_sign = REXCVAR_GET(dlss_jitter_sign);
    float jitter_x = (jitter_sign & 1) ? -jitter_[0] : jitter_[0];
    float jitter_y = (jitter_sign & 2) ? -jitter_[1] : jitter_[1];
    ID3D12Resource* depth_resource = depth_.resource.Get();
    ID3D12Resource* motion_resource = motion_.resource.Get();
    ID3D12Resource* output_resource = dlss_output.resource.Get();
    command_list.ExternalCallback([this, create, upscale, preset, quality, color, depth_resource,
                                   motion_resource, output_resource, width, height, output_width,
                                   output_height, jitter_x, jitter_y,
                                   reset](ID3D12GraphicsCommandList* d3d_command_list) {
      dlss_output_valid_ = false;
      if (create) {
        // The preset of every mode, as which one NGX reads isn't documented.
        for (const char* preset_parameter : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality}) {
          NVSDK_NGX_Parameter_SetUI(ngx_parameters_, preset_parameter, preset);
        }
        NVSDK_NGX_DLSS_Create_Params create_params = {};
        create_params.Feature.InWidth = width;
        create_params.Feature.InHeight = height;
        create_params.Feature.InTargetWidth = output_width;
        create_params.Feature.InTargetHeight = output_height;
        create_params.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value(quality);
        // Motion vectors at the render resolution, without the jitter; LDR
        // color; depth 0 = near.
        create_params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSS_EXT(d3d_command_list, 1, 1, &feature_,
                                                            ngx_parameters_, &create_params);
        if (NVSDK_NGX_FAILED(result) || !feature_) {
          REXGPU_ERROR("DLSS: failed to create the feature ({}x{} to {}x{}, {:08X})", width,
                       height, output_width, output_height, uint32_t(result));
          feature_ = nullptr;
          if (upscale) {
            DisableUpscaling(Backend::kDlss, "the feature couldn't be created");
          } else {
            feature_failed_ = true;
            rex::graphics::SetDlssAvailability(rex::graphics::DlssAvailability::kUnavailable);
            D3D12CommandProcessor::RequestDrawResolutionScaleFromSettings();
          }
          return;
        }
        if (upscale) {
          REXGPU_INFO("DLSS: upscaling {}x{} to {}x{}, preset {}", width, height, output_width,
                      output_height, char('A' + preset - 1));
        } else {
          REXGPU_INFO("DLSS: DLAA at {}x{}, preset {}", width, height, char('A' + preset - 1));
        }
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
        if (upscale) {
          DisableUpscaling(Backend::kDlss, "the evaluation failed");
        } else {
          feature_failed_ = true;
          rex::graphics::SetDlssAvailability(rex::graphics::DlssAvailability::kUnavailable);
          D3D12CommandProcessor::RequestDrawResolutionScaleFromSettings();
        }
        return;
      }
      dlss_output_valid_ = true;
    });
    command_processor.InvalidateStateAfterExternalCommands();
#endif
  }

  if (upscale) {
    // The upscaled scene back at the render resolution, for the frame the game
    // goes on with.
    ui::d3d12::util::DescriptorCpuGpuHandlePair downsample_descriptors[2];
    if (!command_processor.RequestOneUseSingleViewDescriptors(2, downsample_descriptors)) {
      return color_state;
    }
    CreateTexture2DSrv(device, upscaled_.resource.Get(), output_format,
                       downsample_descriptors[0].first);
    CreateTexture2DUav(device, output_.resource.Get(), output_format,
                       downsample_descriptors[1].first);
    Transition(upscaled_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command_processor.SubmitBarriers();
    DownsampleConstants downsample_constants = {};
    downsample_constants.size[0] = width;
    downsample_constants.size[1] = height;
    downsample_constants.ratio[0] = float(output_width) / float(width);
    downsample_constants.ratio[1] = float(output_height) / float(height);
    downsample_constants.source_size_inv[0] = 1.0f / float(output_width);
    downsample_constants.source_size_inv[1] = 1.0f / float(output_height);
    command_processor.SetExternalPipeline(downsample_pipeline_.Get());
    command_list.D3DSetComputeRootSignature(downsample_root_signature_.Get());
    command_list.D3DSetComputeRoot32BitConstants(
        0, sizeof(downsample_constants) / sizeof(uint32_t), &downsample_constants, 0);
    command_list.D3DSetComputeRootDescriptorTable(1, downsample_descriptors[0].second);
    command_list.D3DSetComputeRootDescriptorTable(2, downsample_descriptors[1].second);
    command_list.D3DDispatch(group_count_x, group_count_y, 1);
  }

  // The output (or the upscaled scene back at the render resolution) replaces
  // the scene in the color render target.
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
  if (debug_view == 1 || debug_view == 2) {
    command_list.D3DCopyTextureRegion(&copy_dest, 0, 0, 0, &copy_source, &copy_box);
  } else {
    // Only if DLSS made its output (a failure leaves the game's own scene).
    command_list.ExternalCallback(
        [this, copy_dest, copy_source, copy_box](ID3D12GraphicsCommandList* d3d_command_list) {
          if (dlss_output_valid_) {
            d3d_command_list->CopyTextureRegion(&copy_dest, 0, 0, 0, &copy_source, &copy_box);
          }
        });
  }

  if (upscale) {
    // The HUD gets drawn over the output-size picture too.
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = output_format;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(upscaled_.resource.Get(), &rtv_desc,
                                   hud_rtv_heap_->GetCPUDescriptorHandleForHeapStart());
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
    dsv_desc.Format = depth_dsv_format;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = hud_dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(hud_depth_.resource.Get(), &dsv_desc, dsv);
    Transition(upscaled_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(hud_depth_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    command_processor.SubmitBarriers();
    command_list.D3DClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                          0.0f, 0, 0, nullptr);
    hud_state_ = HudState::kMirroring;
    hud_render_target_ = color;
    hud_color_format_ = output_format;
    hud_depth_format_ = depth_dsv_format;
    hud_width_ = output_width;
    hud_height_ = output_height;
    frame_render_width_ = width;
    frame_render_height_ = height;
    frame_upscaled_ = true;
  }

  frame_processed_ = true;
  previous_width_ = width;
  previous_height_ = height;
  previous_output_width_ = output_width;
  previous_output_height_ = output_height;
#endif
  return color_state;
}

bool D3D12Dlss::RecordFsr(ID3D12Resource* color, DXGI_FORMAT color_format, uint32_t width,
                          uint32_t height, uint32_t output_width, uint32_t output_height,
                          bool upscale, bool reset, ID3D12Resource* output) {
#if defined(REX_HAS_FSR_SDK)
  D3D12CommandProcessor& command_processor = command_processor_;
  DeferredCommandList& command_list = command_processor.GetDeferredCommandList();

  // The color: perceptual for 8 and 10 bits, linear for 16 bits (gamma render
  // targets are stored linear), HDR for floats.
  uint32_t create_flags = D3D12FfxUpscaler::kCreateAutoExposure;
  bool non_linear_srgb = false;
  switch (color_format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      create_flags |= D3D12FfxUpscaler::kCreateNonLinearColor;
      non_linear_srgb = true;
      break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      create_flags |= D3D12FfxUpscaler::kCreateHighDynamicRange;
      break;
    default:
      break;
  }
  CameraParameters camera;
  if (frame_camera_valid_) {
    camera = GetCameraParameters(frame_camera_);
  }
  if (camera.valid && camera.inverted) {
    create_flags |= D3D12FfxUpscaler::kCreateDepthInverted;
  }
  if (camera.valid && camera.infinite) {
    create_flags |= D3D12FfxUpscaler::kCreateDepthInfinite;
  }
  if (!fsr_camera_logged_ && frame_camera_valid_) {
    fsr_camera_logged_ = true;
    if (camera.valid) {
      REXGPU_INFO("AMD FSR: scene color format {}, camera near {} far {} fov {:.3f}{}{}",
                  int(color_format), camera.near_plane, camera.far_plane, camera.fov_y,
                  camera.inverted ? ", depth inverted" : "", camera.infinite ? ", infinite" : "");
    } else {
      const std::array<float, 16>& m = frame_camera_;
      REXGPU_INFO("AMD FSR: scene color format {}, camera parameters not found in [{} {} {} {} / "
                  "{} {} {} {} / {} {} {} {} / {} {} {} {}] - defaults",
                  int(color_format), m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9],
                  m[10], m[11], m[12], m[13], m[14], m[15]);
    }
  }
  const bool debug_view = REXCVAR_GET(fsr_debug_view);
  if (debug_view) {
    create_flags |= D3D12FfxUpscaler::kCreateDebugVisualization;
  }
  uint64_t version = 0;
  const std::string& version_name = REXCVAR_GET(fsr_version);
  if (version_name == "4" || version_name == "3") {
    version = ffx_->FindVersion(uint32_t(version_name[0] - '0'));
  }

  bool created = false;
  if (!ffx_->EnsureContext(output_width, output_height, create_flags, version,
                           command_processor.GetCurrentSubmission(), created)) {
    if (upscale) {
      DisableUpscaling(Backend::kFsr, "the context couldn't be created");
    } else {
      DisableFsr();
    }
    return false;
  }
  rex::graphics::SetFsrProviderName(ffx_->provider_name());

  // The scene's color at its own size: the render target is larger, and FSR
  // takes the size of the resources it's given.
  if (!EnsureTexture(fsr_color_, color_format, width, height, "FSR color",
                     D3D12_RESOURCE_FLAG_NONE)) {
    return false;
  }
  Transition(fsr_color_, D3D12_RESOURCE_STATE_COPY_DEST);
  command_processor.PushTransitionBarrier(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                          D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor.SubmitBarriers();
  D3D12_TEXTURE_COPY_LOCATION copy_dest;
  copy_dest.pResource = fsr_color_.resource.Get();
  copy_dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  copy_dest.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION copy_source;
  copy_source.pResource = color;
  copy_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  copy_source.SubresourceIndex = 0;
  D3D12_BOX copy_box = {0, 0, 0, width, height, 1};
  command_list.D3DCopyTextureRegion(&copy_dest, 0, 0, 0, &copy_source, &copy_box);
  command_processor.PushTransitionBarrier(color, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(fsr_color_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  command_processor.SubmitBarriers();

  const auto now = std::chrono::steady_clock::now();
  float frame_time_ms = 16.67f;
  if (fsr_previous_time_.time_since_epoch().count()) {
    frame_time_ms = std::clamp(
        std::chrono::duration<float, std::milli>(now - fsr_previous_time_).count(), 1.0f, 100.0f);
  }
  fsr_previous_time_ = now;

  D3D12FfxUpscaler::DispatchParams params;
  params.color = fsr_color_.resource.Get();
  params.depth = depth_.resource.Get();
  params.motion_vectors = motion_.resource.Get();
  params.output = output;
  params.render_width = width;
  params.render_height = height;
  params.output_width = output_width;
  params.output_height = output_height;
  const int32_t jitter_sign = REXCVAR_GET(fsr_jitter_sign);
  params.jitter_x = (jitter_sign & 1) ? -jitter_[0] : jitter_[0];
  params.jitter_y = (jitter_sign & 2) ? -jitter_[1] : jitter_[1];
  params.sharpness = float(REXCVAR_GET(fsr_sharpness));
  params.frame_time_ms = frame_time_ms;
  params.reset = reset || created || previous_backend_ != Backend::kFsr;
  params.camera_near = camera.near_plane;
  params.camera_far = camera.far_plane;
  params.camera_fov_y = camera.fov_y;
  params.non_linear_srgb = non_linear_srgb;
  params.debug_view = debug_view;
  // The context as it is now (a new one may replace it before this runs).
  void* context = ffx_->context();
  const D3D12FfxUpscaler* ffx = ffx_.get();
  command_list.ExternalCallback(
      [this, ffx, context, params, upscale](ID3D12GraphicsCommandList* d3d_command_list) {
        dlss_output_valid_ = ffx->Dispatch(context, d3d_command_list, params);
        if (!dlss_output_valid_) {
          if (upscale) {
            DisableUpscaling(Backend::kFsr, "the dispatch failed");
          } else {
            DisableFsr();
          }
        }
      });
  // FidelityFX bound its own descriptor heap, and the pipeline is gone.
  command_processor.InvalidateStateAfterExternalCommands();
  return true;
#else
  return false;
#endif
}

void D3D12Dlss::BindHudTargets(bool with_depth) {
  Transition(upscaled_, D3D12_RESOURCE_STATE_RENDER_TARGET);
  Transition(hud_depth_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  command_processor_.SubmitBarriers();
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = hud_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = hud_dsv_heap_->GetCPUDescriptorHandleForHeapStart();
  command_processor_.GetDeferredCommandList().D3DOMSetRenderTargets(1, &rtv, FALSE,
                                                                    with_depth ? &dsv : nullptr);
}

void D3D12Dlss::InvalidateHud(const char* reason, uint64_t vertex_shader_hash,
                              uint64_t pixel_shader_hash) {
  if (hud_state_ != HudState::kMirroring) {
    return;
  }
  hud_state_ = HudState::kInvalid;
  if (hud_invalid_logged_.size() < 64) {
    std::string key =
        fmt::format("{} (vs {:016X}, ps {:016X})", reason, vertex_shader_hash, pixel_shader_hash);
    if (hud_invalid_logged_.insert(key).second) {
      REXGPU_INFO("Scene upscaler: a frame presented without upscaling - a HUD draw {}", key);
    }
  }
}

D3D12_RESOURCE_STATES D3D12Dlss::FreezeHud(ID3D12Resource* color) {
  D3D12_RESOURCE_STATES color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if (hud_state_ != HudState::kMirroring) {
    return color_state;
  }
  if (!EnsureTexture(frozen_, hud_color_format_, frame_render_width_, frame_render_height_,
                     "frozen frame", D3D12_RESOURCE_FLAG_NONE)) {
    hud_state_ = HudState::kInvalid;
    return color_state;
  }
  hud_state_ = HudState::kFrozen;
  Transition(frozen_, D3D12_RESOURCE_STATE_COPY_DEST);
  command_processor_.PushTransitionBarrier(color, color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  color_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
  command_processor_.SubmitBarriers();
  D3D12_TEXTURE_COPY_LOCATION copy_dest;
  copy_dest.pResource = frozen_.resource.Get();
  copy_dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  copy_dest.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION copy_source;
  copy_source.pResource = color;
  copy_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  copy_source.SubresourceIndex = 0;
  D3D12_BOX copy_box = {0, 0, 0, frame_render_width_, frame_render_height_, 1};
  command_processor_.GetDeferredCommandList().D3DCopyTextureRegion(&copy_dest, 0, 0, 0,
                                                                   &copy_source, &copy_box);
  Transition(frozen_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  return color_state;
}

bool D3D12Dlss::ComposeUpscaledOutput(ID3D12Resource* frame,
                                      const D3D12_SHADER_RESOURCE_VIEW_DESC& frame_srv,
                                      uint32_t frame_width, uint32_t frame_height,
                                      uint32_t output_width, uint32_t output_height,
                                      uint32_t edge_pixels, ID3D12Resource*& output_out,
                                      D3D12_SHADER_RESOURCE_VIEW_DESC& output_srv_out,
                                      bool& upscaled_out) {
  upscaled_out = false;
  if (!frame || !frame_width || !frame_height || !output_width || !output_height) {
    return false;
  }
  if (!EnsureTexture(present_, kPresentFormat, output_width, output_height, "presented")) {
    return false;
  }
  // The upscaled picture is only usable once the copy of the render target
  // ended it, at the size presented now.
  bool use_upscaled = hud_state_ == HudState::kFrozen && frame_upscaled_ &&
                      hud_width_ == output_width &&
                      hud_height_ == output_height && frame_render_width_ == frame_width &&
                      frame_render_height_ == frame_height;
  const uint32_t tiles_width = (frame_width + 7) / 8;
  const uint32_t tiles_height = (frame_height + 7) / 8;
  if (use_upscaled && !EnsureTexture(tiles_, DXGI_FORMAT_R32_UINT, tiles_width, tiles_height,
                                     "comparison")) {
    use_upscaled = false;
  }

  D3D12CommandProcessor& command_processor = command_processor_;
  ID3D12Device* device = command_processor.GetD3D12Provider().GetDevice();
  DeferredCommandList& command_list = command_processor.GetDeferredCommandList();
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[7];
  if (!command_processor.RequestOneUseSingleViewDescriptors(7, descriptors)) {
    return false;
  }
  D3D12_RESOURCE_DESC frame_desc = frame->GetDesc();

  if (use_upscaled) {
    // Which tiles of the frame are the render target at the freeze.
    device->CreateShaderResourceView(frame, &frame_srv, descriptors[0].first);
    CreateTexture2DSrv(device, frozen_.resource.Get(), hud_color_format_, descriptors[1].first);
    CreateTexture2DUav(device, tiles_.resource.Get(), DXGI_FORMAT_R32_UINT, descriptors[2].first);
    Transition(frozen_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(tiles_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command_processor.SubmitBarriers();
    CompareConstants compare_constants = {};
    compare_constants.size[0] = frame_width;
    compare_constants.size[1] = frame_height;
    // Copies of the same pixels - just some room for rounding.
    compare_constants.tolerance = 2.5f / 255.0f;
    compare_constants.border = edge_pixels;
    command_processor.SetExternalPipeline(compare_pipeline_.Get());
    command_list.D3DSetComputeRootSignature(compare_root_signature_.Get());
    command_list.D3DSetComputeRoot32BitConstants(0, sizeof(compare_constants) / sizeof(uint32_t),
                                                 &compare_constants, 0);
    command_list.D3DSetComputeRootDescriptorTable(1, descriptors[0].second);
    command_list.D3DSetComputeRootDescriptorTable(2, descriptors[1].second);
    command_list.D3DSetComputeRootDescriptorTable(3, descriptors[2].second);
    command_list.D3DDispatch(tiles_width, tiles_height, 1);
    Transition(tiles_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(upscaled_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  // The picture presented.
  CreateTexture2DSrv(device, use_upscaled ? upscaled_.resource.Get() : nullptr,
                     use_upscaled ? hud_color_format_ : DXGI_FORMAT_R8G8B8A8_UNORM,
                     descriptors[3].first);
  device->CreateShaderResourceView(frame, &frame_srv, descriptors[4].first);
  CreateTexture2DSrv(device, use_upscaled ? tiles_.resource.Get() : nullptr, DXGI_FORMAT_R32_UINT,
                     descriptors[5].first);
  CreateTexture2DUav(device, present_.resource.Get(), kPresentFormat, descriptors[6].first);
  Transition(present_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  command_processor.SubmitBarriers();
  PresentConstants present_constants = {};
  present_constants.size[0] = output_width;
  present_constants.size[1] = output_height;
  present_constants.frame_size[0] = frame_width;
  present_constants.frame_size[1] = frame_height;
  present_constants.frame_ratio[0] = float(frame_width) / float(output_width);
  present_constants.frame_ratio[1] = float(frame_height) / float(output_height);
  present_constants.frame_texture_size_inv[0] = 1.0f / float(frame_desc.Width);
  present_constants.frame_texture_size_inv[1] = 1.0f / float(frame_desc.Height);
  present_constants.mode = use_upscaled ? (REXCVAR_GET(dlss_debug_view) == 3 ? 2 : 1) : 0;
  command_list.D3DSetComputeRootSignature(present_root_signature_.Get());
  // Only the frame itself if DLSS didn't make its output in this frame.
  command_list.ExternalCallback(
      [this, present_constants](ID3D12GraphicsCommandList* d3d_command_list) mutable {
        if (!dlss_output_valid_) {
          present_constants.mode = 0;
        }
        d3d_command_list->SetComputeRoot32BitConstants(
            0, sizeof(present_constants) / sizeof(uint32_t), &present_constants, 0);
      });
  // Work after external commands needs its pipeline set again.
  command_processor.InvalidateStateAfterExternalCommands();
  command_processor.SetExternalPipeline(present_pipeline_.Get());
  command_list.D3DSetComputeRootDescriptorTable(1, descriptors[3].second);
  command_list.D3DSetComputeRootDescriptorTable(2, descriptors[4].second);
  command_list.D3DSetComputeRootDescriptorTable(3, descriptors[5].second);
  command_list.D3DSetComputeRootDescriptorTable(4, descriptors[6].second);
  command_list.D3DDispatch((output_width + 7) / 8, (output_height + 7) / 8, 1);
  Transition(present_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  output_out = present_.resource.Get();
  output_srv_out = {};
  output_srv_out.Format = kPresentFormat;
  output_srv_out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  output_srv_out.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  output_srv_out.Texture2D.MipLevels = 1;
  upscaled_out = use_upscaled;
  return true;
}

void D3D12Dlss::OnDrawResolutionScaleChanged() {
  // A new history at the new sizes, and the resources of the old ones go.
  previous_processed_ = false;
  previous_camera_valid_ = false;
  jitter_[0] = 0.0f;
  jitter_[1] = 0.0f;
  jitter_index_ = 0;
  lod_bias_ = 0;
  ReleaseLater(upscaled_);
  ReleaseLater(hud_depth_);
  ReleaseLater(frozen_);
  ReleaseLater(tiles_);
  ReleaseLater(present_);
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
  if (ffx_) {
    ffx_->ReleaseRetired(completed_submission);
  }
  // FSR chosen while running: its DLLs are loaded now, and the render scale is
  // chosen again for it.
  if (!fsr_tried_ && !fsr_blocked_ && REXCVAR_GET(fsr_mode) != "off") {
    TryInitializeFsr();
    if (ffx_) {
      D3D12CommandProcessor::RequestDrawResolutionScaleFromSettings();
    }
  }
  // The upscaler not used anymore gives its memory back.
  const Backend backend = SelectBackend();
  if (backend != previous_backend_) {
    const uint64_t current_submission = command_processor_.GetCurrentSubmission();
    if (backend != Backend::kFsr && ffx_) {
      ffx_->RetireContext(current_submission);
    }
#if defined(REX_HAS_DLSS)
    if (backend != Backend::kDlss && feature_) {
      features_to_release_.emplace_back(current_submission, feature_);
      feature_ = nullptr;
    }
#endif
    previous_backend_ = backend;
  }

  previous_processed_ = frame_processed_;
  previous_camera_ = frame_camera_;
  previous_camera_valid_ = frame_processed_ && frame_camera_valid_;

  // Only jitter the next frame if DLSS ran in this one - in frames it doesn't
  // see (menus without a 3D scene) the jitter would just make the picture
  // wobble. Upscaling wants more phases: 8 * (output / render)^2.
  const bool enabled = IsEnabled();
  if (frame_processed_ && enabled && REXCVAR_GET(dlss_jitter)) {
    uint32_t phases = kMinJitterPhases;
    if (frame_upscaled_ && frame_render_width_) {
      float ratio = float(hud_width_) / float(frame_render_width_);
      phases = std::max(phases, uint32_t(std::ceil(8.0f * ratio * ratio)));
    }
    if (phases != jitter_phases_) {
      jitter_phases_ = phases;
      jitter_index_ = 0;
    }
    jitter_index_ = (jitter_index_ + 1) % jitter_phases_;
    jitter_[0] = Halton(jitter_index_ + 1, 2) - 0.5f;
    jitter_[1] = Halton(jitter_index_ + 1, 3) - 0.5f;
  } else {
    jitter_[0] = 0.0f;
    jitter_[1] = 0.0f;
  }
  // Upscaling: textures sharper by the render to output ratio (NVIDIA's mip
  // bias, the rest of it being texture_lod_bias).
  lod_bias_ = 0;
  if (frame_upscaled_ && enabled && frame_render_width_ && hud_width_) {
    lod_bias_ = int32_t(
        std::lround(std::log2(double(frame_render_width_) / double(hud_width_)) * 32.0));
  }

  frame_scene_drawn_ = false;
  frame_scene_taken_ = false;
  frame_processed_ = false;
  frame_upscaled_ = false;
  frame_cameras_.clear();
  frame_camera_valid_ = false;
  hud_state_ = HudState::kNone;
  hud_render_target_ = nullptr;
}

}  // namespace rex::graphics::d3d12
