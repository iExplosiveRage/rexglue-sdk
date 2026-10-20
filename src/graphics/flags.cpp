/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/graphics/flags.h>
#include <rex/logging.h>
#include <rex/ui/renderdoc_api.h>

REXCVAR_DEFINE_BOOL(gpu_allow_invalid_fetch_constants, false, "GPU",
                    "Allow invalid fetch constants");
REXCVAR_DEFINE_BOOL(native_2x_msaa, true, "GPU", "Enable native 2x MSAA");
REXCVAR_DEFINE_BOOL(gpu_native_renderer, false, "GPU",
                    "Native renderer (experimental): the game's own threads run its GPU commands "
                    "at the D3D call that wrote them, with host render targets, instead of an "
                    "emulated command processor thread. Needs a title with native renderer "
                    "hooks. Applies at the next start. Off = the emulated path.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// On by default: Burst Limit's depth of field in fights reads a render target
// through another one at the same EDRAM range, so skipping the copies (false)
// smears the picture with depth_of_field on. Off is kept for A/B and perf
// experiments (0.03-0.15 ms per frame).
REXCVAR_DEFINE_BOOL(gpu_native_edram_transfers, true, "GPU/Debug",
                    "With gpu_native_renderer: copy between render targets sharing EDRAM ranges "
                    "like the emulated path (false skips them: faster, but wrong for effects "
                    "that read one render target through another; live)");
// Native render target pool: with gpu_native_renderer, render targets are
// looked up by guest surface (EDRAM base, pitch, MSAA, format) - a draw that
// binds the same surfaces as the last one, with no other surface having
// touched the EDRAM since, skips the EDRAM ownership map; direct resolves find
// their source surface by key. Live, for A/B.
REXCVAR_DEFINE_BOOL(gpu_native_rt_pool, true, "GPU",
                    "With gpu_native_renderer: render targets looked up by guest surface "
                    "(skips the EDRAM ownership map while nothing aliases; live)");
REXCVAR_DEFINE_BOOL(depth_float24_round, false, "GPU", "Round float24 depth values");
REXCVAR_DEFINE_BOOL(depth_float24_convert_in_pixel_shader, false, "GPU",
                    "Convert float24 depth in pixel shader");
REXCVAR_DEFINE_BOOL(depth_transfer_not_equal_test, true, "GPU",
                    "Use not-equal test for depth transfer");
REXCVAR_DEFINE_BOOL(gamma_render_target_as_unorm16, true, "GPU",
                    "Use R16G16B16A16_UNORM for gamma render targets (more accurate than sRGB)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(dump_shaders, "", "GPU", "Path to dump shaders to");
REXCVAR_DEFINE_BOOL(use_fuzzy_alpha_epsilon, false, "GPU",
                    "Use approximate compare for alpha test values to prevent "
                    "flickering on NVIDIA graphics cards");
REXCVAR_DEFINE_BOOL(gpu_debug_markers, false, "GPU",
                    "Insert debug markers into GPU command streams for tools "
                    "like PIX and RenderDoc. Automatically enabled when "
                    "RenderDoc is detected.");

bool IsGpuDebugMarkersEnabled() {
  static bool cached = false;
  static bool result = false;
  if (!cached) {
    cached = true;
    if (REXCVAR_GET(gpu_debug_markers)) {
      result = true;
      REXLOG_INFO("GPU debug markers enabled via CVar");
    } else {
      auto renderdoc_api = rex::ui::RenderDocAPI::CreateIfConnected();
      if (renderdoc_api) {
        result = true;
        REXLOG_INFO("GPU debug markers auto-enabled (RenderDoc detected)");
      }
    }
  }
  return result;
}
