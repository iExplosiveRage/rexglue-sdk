/**
 * @file        graphics/d3d12/ffx_upscaler.cpp
 * @brief       AMD FSR 4 / FSR 3.1.5 through AMD's FidelityFX API DLLs, for the
 *              3D scene upscaler (D3D12Dlss)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ffx_upscaler.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/logging.h>

#if defined(REX_HAS_FSR_SDK)
// AMD's FidelityFX SDK v2.3.0 headers (cmake/rexglue_fsr_sdk.cmake) - not the
// ones of the FidelityFX SDK the presenter is built with. The functions are
// only called through the pointers from the loader DLL: there's no import
// library, and ffx_api_loader.h isn't used (it needs _WINDOWS).
#include "api/include/dx12/ffx_api_dx12.h"
#include "upscalers/include/ffx_upscale.h"
#endif

namespace rex::graphics::d3d12 {

#if defined(REX_HAS_FSR_SDK)
namespace {

// FidelityFX messages (any thread, also the GPU submission's).
void FfxMessage(uint32_t type, const wchar_t* message) {
  if (!message) {
    return;
  }
  int length = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
  std::string text(length > 0 ? size_t(length - 1) : 0, '\0');
  if (length > 1) {
    WideCharToMultiByte(CP_UTF8, 0, message, -1, text.data(), length, nullptr, nullptr);
  }
  if (type == FFX_API_MESSAGE_TYPE_ERROR) {
    REXGPU_ERROR("AMD FSR: {}", text);
  } else {
    REXGPU_WARN("AMD FSR: {}", text);
  }
}

// The major version of an upscaler version (its name, or the id's version
// bits: major << 22 | minor << 12 | patch, bit 31 = from the driver).
uint32_t MajorVersion(uint64_t id, const std::string& name) {
  if (!name.empty() && name[0] >= '0' && name[0] <= '9') {
    return uint32_t(std::strtoul(name.c_str(), nullptr, 10));
  }
  return (uint32_t(id) & 0x7FFFFFFFu) >> 22;
}

}  // namespace
#endif

D3D12FfxUpscaler::~D3D12FfxUpscaler() {
  Shutdown();
}

bool D3D12FfxUpscaler::Initialize(ID3D12Device* device) {
  available_ = false;
  device_ = device;
#if defined(REX_HAS_FSR_SDK)
  if (!device) {
    return false;
  }
  if (!create_context_) {
    // From the executable's folder only - and it loads the upscaler DLL from
    // there.
    const std::filesystem::path path =
        rex::filesystem::GetExecutableFolder() / L"amd_fidelityfx_loader_dx12.dll";
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
      REXGPU_WARN("AMD FSR: not available - amd_fidelityfx_loader_dx12.dll couldn't be loaded "
                  "({})",
                  uint32_t(GetLastError()));
      return false;
    }
    // Never unloaded: FidelityFX may keep threads or resources until the end.
    void* create_context = reinterpret_cast<void*>(GetProcAddress(module, "ffxCreateContext"));
    void* destroy_context = reinterpret_cast<void*>(GetProcAddress(module, "ffxDestroyContext"));
    void* query = reinterpret_cast<void*>(GetProcAddress(module, "ffxQuery"));
    void* dispatch = reinterpret_cast<void*>(GetProcAddress(module, "ffxDispatch"));
    if (!create_context || !destroy_context || !query || !dispatch) {
      REXGPU_WARN("AMD FSR: not available - the loader DLL lacks the FidelityFX functions");
      return false;
    }
    create_context_ = create_context;
    destroy_context_ = destroy_context;
    query_ = query;
    dispatch_ = dispatch;
  }

  // The upscaler versions this GPU has, newest first.
  versions_.clear();
  auto query = reinterpret_cast<PfnFfxQuery>(query_);
  uint64_t count = 0;
  ffxQueryDescGetVersions versions_query = {};
  versions_query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  versions_query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  versions_query.device = device;
  versions_query.outputCount = &count;
  if (query(nullptr, &versions_query.header) != FFX_API_RETURN_OK || !count) {
    REXGPU_WARN("AMD FSR: not available - no upscaler for this GPU");
    return false;
  }
  std::vector<uint64_t> ids(static_cast<size_t>(count));
  std::vector<const char*> names(static_cast<size_t>(count));
  versions_query.versionIds = ids.data();
  versions_query.versionNames = names.data();
  if (query(nullptr, &versions_query.header) != FFX_API_RETURN_OK) {
    REXGPU_WARN("AMD FSR: not available - the upscaler versions couldn't be listed");
    return false;
  }
  std::string list;
  for (uint64_t i = 0; i < count && i < ids.size(); ++i) {
    versions_.emplace_back(ids[i], names[i] ? names[i] : "");
    list += fmt::format("{}{} ({:016X})", list.empty() ? "" : ", ", versions_.back().second,
                        ids[i]);
  }
  REXGPU_INFO("AMD FSR: upscaler versions for this GPU: {}", list);
  available_ = !versions_.empty();
  return available_;
#else
  return false;
#endif
}

void D3D12FfxUpscaler::Shutdown() {
#if defined(REX_HAS_FSR_SDK)
  for (auto& retired : retired_) {
    DestroyContext(retired.second);
  }
  retired_.clear();
  if (context_) {
    DestroyContext(context_);
    context_ = nullptr;
  }
#endif
  context_width_ = 0;
  context_height_ = 0;
  provider_name_.clear();
  provider_major_ = 0;
  available_ = false;
}

uint64_t D3D12FfxUpscaler::FindVersion(uint32_t major) const {
#if defined(REX_HAS_FSR_SDK)
  for (const auto& version : versions_) {
    if (MajorVersion(version.first, version.second) == major) {
      return version.first;
    }
  }
#endif
  return 0;
}

bool D3D12FfxUpscaler::EnsureContext(uint32_t output_width, uint32_t output_height,
                                     uint32_t create_flags, uint64_t version,
                                     uint64_t submission, bool& created_out) {
  created_out = false;
#if defined(REX_HAS_FSR_SDK)
  if (!available_) {
    return false;
  }
  if (context_ && context_width_ == output_width && context_height_ == output_height &&
      context_flags_ == create_flags && context_version_ == version) {
    return true;
  }
  RetireContext(submission);

  ffxCreateContextDescUpscale create_desc = {};
  create_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  create_desc.flags = 0;
  if (create_flags & kCreateHighDynamicRange) {
    create_desc.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
  }
  if (create_flags & kCreateDepthInverted) {
    create_desc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
  }
  if (create_flags & kCreateDepthInfinite) {
    create_desc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
  }
  if (create_flags & kCreateAutoExposure) {
    create_desc.flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
  }
  if (create_flags & kCreateNonLinearColor) {
    create_desc.flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
  }
  if (create_flags & kCreateDebugVisualization) {
    create_desc.flags |= FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
  }
  if (create_flags & kCreateDebugChecking) {
    create_desc.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
  }
  // Any render size up to the output's (the render scale changes without a
  // new context).
  create_desc.maxRenderSize.width = output_width;
  create_desc.maxRenderSize.height = output_height;
  create_desc.maxUpscaleSize.width = output_width;
  create_desc.maxUpscaleSize.height = output_height;
  create_desc.fpMessage = FfxMessage;
  ffxCreateBackendDX12Desc backend_desc = {};
  backend_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend_desc.device = device_;
  // Required since SDK 2.1: the API version this was built against.
  ffxCreateContextDescUpscaleVersion api_version_desc = {};
  api_version_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
  api_version_desc.version = FFX_UPSCALER_VERSION;
  ffxOverrideVersion override_desc = {};
  override_desc.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
  override_desc.versionId = version;
  create_desc.header.pNext = &backend_desc.header;
  backend_desc.header.pNext = &api_version_desc.header;
  api_version_desc.header.pNext = version ? &override_desc.header : nullptr;

  ffxContext context = nullptr;
  ffxReturnCode_t result =
      reinterpret_cast<PfnFfxCreateContext>(create_context_)(&context, &create_desc.header,
                                                             nullptr);
  if (result != FFX_API_RETURN_OK || !context) {
    REXGPU_ERROR("AMD FSR: failed to create the context for {}x{} ({})", output_width,
                 output_height, uint32_t(result));
    provider_name_.clear();
    provider_major_ = 0;
    return false;
  }
  context_ = context;
  context_width_ = output_width;
  context_height_ = output_height;
  context_flags_ = create_flags;
  context_version_ = version;
  created_out = true;

  ffxQueryGetProviderVersion provider = {};
  provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  if (reinterpret_cast<PfnFfxQuery>(query_)(&context_, &provider.header) == FFX_API_RETURN_OK &&
      provider.versionName) {
    provider_name_ = provider.versionName;
  } else {
    provider_name_.clear();
  }
  provider_major_ = MajorVersion(provider.versionId, provider_name_);
  REXGPU_INFO("AMD FSR {}: context for up to {}x{} (version {:016X}, flags {:03X})",
              provider_name_.empty() ? "?" : provider_name_, output_width, output_height,
              provider.versionId, create_desc.flags);
  return true;
#else
  return false;
#endif
}

bool D3D12FfxUpscaler::Dispatch(void* context, ID3D12GraphicsCommandList* command_list,
                                const DispatchParams& params) const {
#if defined(REX_HAS_FSR_SDK)
  if (!context || !command_list) {
    return false;
  }
  ffxDispatchDescUpscale dispatch = {};
  dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  dispatch.commandList = command_list;
  // The states the resources are in (FidelityFX puts them back in them).
  dispatch.color = ffxApiGetResourceDX12(params.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  dispatch.depth = ffxApiGetResourceDX12(params.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  dispatch.motionVectors =
      ffxApiGetResourceDX12(params.motion_vectors, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  dispatch.exposure = ffxApiGetResourceDX12(nullptr);
  dispatch.reactive = ffxApiGetResourceDX12(nullptr);
  dispatch.transparencyAndComposition = ffxApiGetResourceDX12(nullptr);
  dispatch.output = ffxApiGetResourceDX12(params.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  dispatch.jitterOffset.x = params.jitter_x;
  dispatch.jitterOffset.y = params.jitter_y;
  // Already in render pixels.
  dispatch.motionVectorScale.x = 1.0f;
  dispatch.motionVectorScale.y = 1.0f;
  dispatch.renderSize.width = params.render_width;
  dispatch.renderSize.height = params.render_height;
  dispatch.upscaleSize.width = params.output_width;
  dispatch.upscaleSize.height = params.output_height;
  dispatch.enableSharpening = params.sharpness > 0.0f;
  dispatch.sharpness = std::clamp(params.sharpness, 0.0f, 1.0f);
  dispatch.frameTimeDelta = params.frame_time_ms;
  dispatch.preExposure = 1.0f;
  dispatch.reset = params.reset;
  dispatch.cameraNear = params.camera_near;
  dispatch.cameraFar = params.camera_far;
  dispatch.cameraFovAngleVertical = params.camera_fov_y;
  dispatch.viewSpaceToMetersFactor = 1.0f;
  dispatch.flags = 0;
  if (params.non_linear_srgb) {
    dispatch.flags |= FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
  }
  if (params.debug_view) {
    dispatch.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
  }
  ffxContext ffx_context = context;
  ffxReturnCode_t result =
      reinterpret_cast<PfnFfxDispatch>(dispatch_)(&ffx_context, &dispatch.header);
  if (result != FFX_API_RETURN_OK) {
    REXGPU_ERROR("AMD FSR: dispatch failed ({})", uint32_t(result));
    return false;
  }
  return true;
#else
  return false;
#endif
}

void D3D12FfxUpscaler::RetireContext(uint64_t submission) {
  if (context_) {
    retired_.emplace_back(submission, context_);
    context_ = nullptr;
  }
  context_width_ = 0;
  context_height_ = 0;
}

void D3D12FfxUpscaler::ReleaseRetired(uint64_t completed_submission) {
  std::erase_if(retired_, [this, completed_submission](const auto& retired) {
    if (retired.first > completed_submission) {
      return false;
    }
    DestroyContext(retired.second);
    return true;
  });
}

void D3D12FfxUpscaler::DestroyContext(void* context) {
#if defined(REX_HAS_FSR_SDK)
  if (context && destroy_context_) {
    ffxContext ffx_context = context;
    reinterpret_cast<PfnFfxDestroyContext>(destroy_context_)(&ffx_context, nullptr);
  }
#endif
}

}  // namespace rex::graphics::d3d12
