#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>

namespace rex {
namespace kernel {
namespace xam {
namespace apps {

class XgiApp : public system::xam::App {
 public:
  explicit XgiApp(system::KernelState* kernel_state);

  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                uint32_t buffer_length) override;

 private:
  // XSessionSearchEx with the online lobby (rex/net/online.h).
  X_HRESULT FillLobbySearchResults(uint32_t results_ptr, uint32_t buffer_size,
                                   uint32_t num_results, uint32_t num_props, uint32_t props_ptr,
                                   uint32_t num_ctx, uint32_t ctx_ptr);
};

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
