/**
 * @file        ui/rex_app.cpp
 * @brief       ReXApp implementation - compiled as part of the consumer executable
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/rex_app.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/ui/flags.h>
#include <rex/kernel/crt/heap.h>
#include <rex/filesystem.h>
#include <rex/logging/sink.h>
#include <rex/logging.h>
#include <rex/perf/frame_rate.h>
#include <rex/ui/overlay/achievement_toast.h>
#include <rex/ui/overlay/achievements_overlay.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/overlay_text.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/string/numeric.h>
#include <rex/system.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/debug_command_pipe.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/image_encode.h>
#include <rex/ui/keybinds.h>
#include <rex/version.h>

#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <vector>
#include <filesystem>
#include <string_view>

#include <rex/platform.h>
#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(gpu_plugin, "", "GPU",
                      "GPU emulation plugin to load at startup (e.g. 'xenos'); empty disables "
                      "GPU emulation")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(quick_menu_buttons, "back+start", "UI",
                      "Controller buttons that open the settings menu (F1 on the keyboard): "
                      "back+start, l3+r3, none")
    .allowed({"back+start", "l3+r3", "none"});

REXCVAR_DEFINE_BOOL(debug_overlay, false, "UI", "Show the frame rate overlay (F3)");

REXCVAR_DEFINE_STRING(debug_command_pipe, "", "Debug",
                      "Test automation: run console lines sent to the local named pipe "
                      "\\\\.\\pipe\\<name>, each acknowledged on the pipe (empty = off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .debug_only();

namespace rex {

namespace {

// The running app's presenter, for the screenshot command. UI thread.
std::function<ui::Presenter*()> g_screenshot_presenter;

// screenshot <path.png>: saves the frame the presenter shows - the guest
// output, at its render resolution, without the overlays - as a PNG. Needs
// neither the window in front nor visible. The capture is taken here (UI
// thread); the PNG is written on its own thread, and a debug command pipe
// acknowledges the line once the file is complete.
void ScreenshotCommand(std::string_view args) {
  ui::CommandCompletion done = ui::DeferCommandCompletion();
  while (!args.empty() && (args.front() == ' ' || args.front() == '\t')) {
    args.remove_prefix(1);
  }
  while (!args.empty() && (args.back() == ' ' || args.back() == '\t')) {
    args.remove_suffix(1);
  }
  if (args.size() >= 2 && args.front() == '"' && args.back() == '"') {
    args = args.substr(1, args.size() - 2);
  }
  if (args.empty()) {
    REXLOG_WARN("screenshot: usage: screenshot <path.png>");
    done(false, "usage: screenshot <path.png>");
    return;
  }
  std::string path_text(args);
  ui::Presenter* presenter = g_screenshot_presenter ? g_screenshot_presenter() : nullptr;
  auto image = std::make_shared<ui::RawImage>();
  if (!presenter || !presenter->CaptureGuestOutput(*image)) {
    REXLOG_WARN("screenshot: no game frame to capture yet ({})", path_text);
    done(false, "no game frame yet");
    return;
  }
  std::filesystem::path path(std::u8string(path_text.begin(), path_text.end()));
  std::thread([image, path = std::move(path), path_text = std::move(path_text),
               done = std::move(done)]() {
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    if (!ui::WriteImagePng(*image, path, &error)) {
      REXLOG_WARN("screenshot: could not save {}: {}", path_text, error);
      done(false, error);
      return;
    }
    std::error_code ec;
    const uintmax_t bytes = std::filesystem::file_size(path, ec);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    REXLOG_INFO("screenshot: saved {} ({}x{}, {} bytes, {} ms)", path_text, image->width,
                image->height, ec ? 0 : bytes, ms);
    done(true, fmt::format("{}x{}", image->width, image->height));
  }).detach();
}

}  // namespace

}  // namespace rex

REXCVAR_DEFINE_COMMAND_ARGS(screenshot, rex::ScreenshotCommand, "Debug",
                            "Save the game frame (guest output, render resolution) as PNG: "
                            "<path.png>");

namespace rex {

namespace {

// screenshot_ui <path.png>: like screenshot, but the next frame shown in the
// window - the game with the overlays (menus, panels) drawn over it - at the
// window's size. The window has to be painting (not minimized).
void ScreenshotUiCommand(std::string_view args) {
  ui::CommandCompletion done = ui::DeferCommandCompletion();
  while (!args.empty() && (args.front() == ' ' || args.front() == '\t')) {
    args.remove_prefix(1);
  }
  while (!args.empty() && (args.back() == ' ' || args.back() == '\t')) {
    args.remove_suffix(1);
  }
  if (args.size() >= 2 && args.front() == '"' && args.back() == '"') {
    args = args.substr(1, args.size() - 2);
  }
  if (args.empty()) {
    REXLOG_WARN("screenshot_ui: usage: screenshot_ui <path.png>");
    done(false, "usage: screenshot_ui <path.png>");
    return;
  }
  std::string path_text(args);
  ui::Presenter* presenter = g_screenshot_presenter ? g_screenshot_presenter() : nullptr;
  // Answered once: by the capture, or after a few seconds without a painted
  // frame.
  struct Pending {
    std::atomic<bool> answered{false};
    ui::CommandCompletion done;
  };
  auto pending = std::make_shared<Pending>();
  pending->done = std::move(done);
  auto answer = [pending](bool ok, std::string message) {
    if (!pending->answered.exchange(true)) {
      pending->done(ok, std::move(message));
    }
  };
  std::filesystem::path path(std::u8string(path_text.begin(), path_text.end()));
  const bool requested =
      presenter &&
      presenter->RequestPresentedFrameCapture(
          [answer, path, path_text](std::shared_ptr<ui::RawImage> image) {
            if (!image) {
              REXLOG_WARN("screenshot_ui: the frame couldn't be read ({})", path_text);
              answer(false, "capture failed");
              return;
            }
            std::thread([image, path, path_text, answer]() {
              std::string error;
              if (!ui::WriteImagePng(*image, path, &error)) {
                REXLOG_WARN("screenshot_ui: could not save {}: {}", path_text, error);
                answer(false, error);
                return;
              }
              REXLOG_INFO("screenshot_ui: saved {} ({}x{})", path_text, image->width,
                          image->height);
              answer(true, fmt::format("{}x{}", image->width, image->height));
            }).detach();
          });
  if (!requested) {
    REXLOG_WARN("screenshot_ui: not supported by this renderer ({})", path_text);
    answer(false, "not supported");
    return;
  }
  std::thread([answer]() {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    answer(false, "no frame painted (minimized?)");
  }).detach();
}

}  // namespace

}  // namespace rex

REXCVAR_DEFINE_COMMAND_ARGS(screenshot_ui, rex::ScreenshotUiCommand, "Debug",
                            "Save the next frame shown in the window, with the overlays, as "
                            "PNG: <path.png>");

namespace rex {

namespace {

// perf_report [seconds]: measures the game for that long (default 10) and logs
// the average guest frame rate, frame time percentiles and the texture cache
// activity per second. Test automation: a debug command pipe acknowledges the
// line with the summary when the measurement is done.
void PerfReportCommand(std::string_view args) {
  ui::CommandCompletion done = ui::DeferCommandCompletion();
  double seconds = 10.0;
  {
    std::string text(args);
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() && value > 0.0) {
      seconds = std::min(value, 600.0);
    }
  }
  std::thread([seconds, done = std::move(done)]() {
    using clock = std::chrono::steady_clock;
    perf::TextureCacheStats& tc = perf::GetTextureCacheStats();
    auto snap = [&tc]() {
      return std::array<uint64_t, 9>{
          tc.created.load(),             tc.destroyed.load(),
          tc.loaded.load(),              tc.replacement_uploads.load(),
          tc.replacement_lookups.load(), tc.replacement_hashes.load(),
          tc.replacement_hash_ns.load(), tc.replacement_decodes.load(),
          tc.replacement_decode_ns.load()};
    };
    const auto before = snap();
    const uint64_t start_swaps = perf::GetGuestSwapCount();
    const auto start = clock::now();
    std::vector<float> frame_ms;
    uint64_t seen_swaps = start_swaps;
    float recent[255];
    uint64_t peak_memory = 0;
    while (std::chrono::duration<double>(clock::now() - start).count() < seconds) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const uint64_t swaps = perf::GetGuestSwapCount();
      const size_t count = perf::GetGuestFrameTimes(recent, 255);
      const size_t take = size_t(std::min<uint64_t>(swaps - seen_swaps, count));
      frame_ms.insert(frame_ms.end(), recent + (count - take), recent + count);
      seen_swaps = swaps;
      peak_memory = std::max(peak_memory, tc.memory_bytes.load());
    }
    const double elapsed = std::chrono::duration<double>(clock::now() - start).count();
    const uint64_t frames = perf::GetGuestSwapCount() - start_swaps;
    const auto after = snap();
    std::sort(frame_ms.begin(), frame_ms.end());
    auto percentile = [&frame_ms](double p) -> double {
      if (frame_ms.empty()) {
        return 0.0;
      }
      return frame_ms[std::min(frame_ms.size() - 1, size_t(p * double(frame_ms.size())))];
    };
    auto per_second = [&](size_t i) { return double(after[i] - before[i]) / elapsed; };
    const perf::RenderInfo render = perf::GetRenderInfo();
    const std::string summary = fmt::format(
        "fps {:.1f} frames {} in {:.1f} s | frame ms p50 {:.2f} p95 {:.2f} p99 {:.2f} max {:.2f} | "
        "scale {}x{} (target {}x{}) | textures/s created {:.1f} destroyed {:.1f} loaded {:.1f} "
        "repl_uploads {:.1f} repl_lookups {:.1f} repl_hashes {:.1f} ({:.2f} ms/s) repl_decodes "
        "{:.1f} ({:.2f} ms/s) | cache {} MB (peak {} MB) limits {}/{} MB | vram {} / {} MB",
        double(frames) / elapsed, frames, elapsed, percentile(0.5), percentile(0.95),
        percentile(0.99), frame_ms.empty() ? 0.0 : double(frame_ms.back()), render.scale_x,
        render.scale_y, render.requested_scale_x, render.requested_scale_y, per_second(0),
        per_second(1), per_second(2), per_second(3), per_second(4), per_second(5),
        double(after[6] - before[6]) * 1.0e-6 / elapsed, per_second(7),
        double(after[8] - before[8]) * 1.0e-6 / elapsed, tc.memory_bytes.load() >> 20,
        peak_memory >> 20, tc.limit_soft_mb.load(), tc.limit_hard_mb.load(),
        tc.vram_usage_bytes.load() >> 20, tc.vram_budget_bytes.load() >> 20);
    REXLOG_INFO("perf_report: {}", summary);
    done(true, summary);
  }).detach();
}

}  // namespace

}  // namespace rex

REXCVAR_DEFINE_COMMAND_ARGS(perf_report, rex::PerfReportCommand, "Debug",
                            "Measure the frame rate and texture cache activity: [seconds]");

namespace rex {

namespace {

// hitch_start / hitch_stop: records every guest frame time in between (the
// pipe stays free meanwhile, unlike perf_report) and stop answers with the
// slow frames - for stutter tests across menus and loads.
struct HitchRecorder {
  std::mutex mutex;
  std::thread thread;
  std::atomic<bool> running{false};
  std::vector<float> frame_ms;
  std::array<uint64_t, 4> before{};
  std::chrono::steady_clock::time_point start;
};

HitchRecorder& Hitches() {
  static HitchRecorder recorder;
  return recorder;
}

std::array<uint64_t, 4> HitchStats() {
  perf::TextureCacheStats& tc = perf::GetTextureCacheStats();
  return {tc.replacement_decodes.load(), tc.replacement_decode_ns.load(),
          tc.replacement_async_loads.load(), tc.replacement_uploads.load()};
}

void HitchStartCommand() {
  HitchRecorder& r = Hitches();
  if (r.running.exchange(true)) {
    return;
  }
  if (r.thread.joinable()) {
    r.thread.join();
  }
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    r.frame_ms.clear();
    r.before = HitchStats();
    r.start = std::chrono::steady_clock::now();
  }
  r.thread = std::thread([&r]() {
    uint64_t seen = perf::GetGuestSwapCount();
    float recent[255];
    while (r.running.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const uint64_t swaps = perf::GetGuestSwapCount();
      const size_t count = perf::GetGuestFrameTimes(recent, 255);
      const size_t take = size_t(std::min<uint64_t>(swaps - seen, count));
      seen = swaps;
      std::lock_guard<std::mutex> lock(r.mutex);
      r.frame_ms.insert(r.frame_ms.end(), recent + (count - take), recent + count);
    }
  });
}

void HitchStopCommand() {
  HitchRecorder& r = Hitches();
  ui::CommandCompletion done = ui::DeferCommandCompletion();
  if (!r.running.exchange(false)) {
    done(false, "not recording (hitch_start first)");
    return;
  }
  if (r.thread.joinable()) {
    r.thread.join();
  }
  std::vector<float> frames;
  std::array<uint64_t, 4> before;
  double seconds;
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    frames = r.frame_ms;
    before = r.before;
    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - r.start).count();
  }
  const std::array<uint64_t, 4> after = HitchStats();
  int over_25 = 0, over_50 = 0, over_100 = 0;
  float worst = 0.0f;
  for (float ms : frames) {
    over_25 += ms > 25.0f;
    over_50 += ms > 50.0f;
    over_100 += ms > 100.0f;
    worst = std::max(worst, ms);
  }
  std::vector<float> sorted = frames;
  std::sort(sorted.begin(), sorted.end(), std::greater<float>());
  std::string top;
  for (size_t i = 0; i < sorted.size() && i < 8; ++i) {
    top += fmt::format("{}{:.0f}", i ? " " : "", sorted[i]);
  }
  const std::string summary = fmt::format(
      "{:.0f} s, {} frames | slow frames >25 ms {} >50 ms {} >100 ms {} | worst {:.1f} ms | top "
      "[{}] | sync decodes {} ({:.0f} ms) async loads {} replacement uploads {}",
      seconds, frames.size(), over_25, over_50, over_100, double(worst), top,
      after[0] - before[0], double(after[1] - before[1]) * 1.0e-6, after[2] - before[2],
      after[3] - before[3]);
  REXLOG_INFO("hitch_report: {}", summary);
  done(true, summary);
}

}  // namespace

}  // namespace rex

REXCVAR_DEFINE_COMMAND(quick_menu_toggle, rex::ReXApp::ToggleQuickMenuCommand, "Debug",
                       "Open or close the settings menu (F1)");
REXCVAR_DEFINE_COMMAND(hitch_start, rex::HitchStartCommand, "Debug",
                       "Start recording frame times (stutter test); hitch_stop reports");
REXCVAR_DEFINE_COMMAND(hitch_stop, rex::HitchStopCommand, "Debug",
                       "Stop recording frame times and report the slow frames");

namespace rex {

namespace {

// gpu_profile [seconds]: GPU time per part of a frame from timestamp queries
// (D3D12), plus the command processor's waits for the GPU. Values are averages
// per guest frame in milliseconds.
void GpuProfileCommand(std::string_view args) {
  ui::CommandCompletion done = ui::DeferCommandCompletion();
  double seconds = 10.0;
  {
    std::string text(args);
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() && value > 0.0) {
      seconds = std::min(value, 600.0);
    }
  }
  std::thread([seconds, done = std::move(done)]() {
    using clock = std::chrono::steady_clock;
    perf::GpuProfileStats& stats = perf::GetGpuProfileStats();
    constexpr size_t kPasses = size_t(perf::GpuPass::kCount);
    struct Snapshot {
      uint64_t pass_ns[kPasses], pass_count[kPasses];
      uint64_t submission_ns, submissions, unprofiled, paint_ns, paints, gap_ns, fence_waits,
          fence_wait_ns, occlusion_queries, draws, resolves, clears, upload_batches, upload_bytes,
          texture_loads, texture_load_bytes, cp_submissions, present_calls,
          present_cpu_ns, swaps;
    };
    auto snap = [&stats]() {
      Snapshot s;
      for (size_t i = 0; i < kPasses; ++i) {
        s.pass_ns[i] = stats.pass_ns[i].load();
        s.pass_count[i] = stats.pass_count[i].load();
      }
      s.submission_ns = stats.submission_ns.load();
      s.submissions = stats.submissions.load();
      s.unprofiled = stats.submissions_unprofiled.load();
      s.paint_ns = stats.paint_ns.load();
      s.paints = stats.paints.load();
      s.gap_ns = stats.gap_ns.load();
      s.fence_waits = stats.fence_waits.load();
      s.fence_wait_ns = stats.fence_wait_ns.load();
      s.occlusion_queries = stats.occlusion_queries.load();
      s.draws = stats.draws.load();
      s.resolves = stats.resolves.load();
      s.clears = stats.clears_in_place.load();
      s.upload_batches = stats.memory_upload_batches.load();
      s.upload_bytes = stats.memory_upload_bytes.load();
      s.texture_loads = stats.texture_loads.load();
      s.texture_load_bytes = stats.texture_load_bytes.load();
      s.cp_submissions = stats.cp_submissions.load();
      s.present_calls = stats.present_calls.load();
      s.present_cpu_ns = stats.present_cpu_ns.load();
      s.swaps = perf::GetGuestSwapCount();
      return s;
    };
    stats.enabled.store(true);
    // Results arrive a few frames late - let the pipeline fill first.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const Snapshot before = snap();
    const auto start = clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    const Snapshot after = snap();
    const double elapsed = std::chrono::duration<double>(clock::now() - start).count();
    stats.enabled.store(false);
    const double frames = double(std::max<uint64_t>(after.swaps - before.swaps, 1));
    auto ms = [frames](uint64_t a, uint64_t b) { return double(b - a) * 1.0e-6 / frames; };
    std::string parts;
    double total = 0.0;
    for (size_t i = 0; i < kPasses; ++i) {
      const double value = ms(before.pass_ns[i], after.pass_ns[i]);
      total += value;
      if (after.pass_count[i] == before.pass_count[i]) {
        continue;
      }
      parts += fmt::format(" {} {:.3f}", perf::GetGpuPassName(perf::GpuPass(i)), value);
    }
    const double busy =
        ms(before.submission_ns, after.submission_ns) + ms(before.paint_ns, after.paint_ns);
    const std::string summary = fmt::format(
        "fps {:.1f} frame {:.2f} ms | gpu ms/frame:{} | sum {:.3f} busy {:.3f} gaps {:.3f} | "
        "per frame: submissions {:.1f} (unprofiled {:.2f}) paints {:.2f} draws {:.0f} resolves "
        "{:.1f} clears_in_place {:.1f} occlusion {:.1f} uploads {:.1f} ({:.2f} MB) texture loads {:.1f} ({:.1f} MB) | cpu: fence waits {:.2f} ({:.3f} ms) present {:.3f} ms",
        frames / elapsed, elapsed * 1000.0 / frames, parts, total, busy,
        ms(before.gap_ns, after.gap_ns), double(after.cp_submissions - before.cp_submissions) / frames,
        double(after.unprofiled - before.unprofiled) / frames,
        double(after.paints - before.paints) / frames, double(after.draws - before.draws) / frames,
        double(after.resolves - before.resolves) / frames,
        double(after.clears - before.clears) / frames,
        double(after.occlusion_queries - before.occlusion_queries) / frames,
        double(after.upload_batches - before.upload_batches) / frames,
        double(after.upload_bytes - before.upload_bytes) / frames / 1048576.0,
        double(after.texture_loads - before.texture_loads) / frames,
        double(after.texture_load_bytes - before.texture_load_bytes) / frames / 1048576.0,
        double(after.fence_waits - before.fence_waits) / frames,
        ms(before.fence_wait_ns, after.fence_wait_ns),
        after.present_calls > before.present_calls
            ? double(after.present_cpu_ns - before.present_cpu_ns) * 1.0e-6 /
                  double(after.present_calls - before.present_calls)
            : 0.0);
    REXLOG_INFO("gpu_profile: {}", summary);
    done(true, summary);
  }).detach();
}

}  // namespace

}  // namespace rex

REXCVAR_DEFINE_COMMAND_ARGS(gpu_profile, rex::GpuProfileCommand, "Debug",
                            "Measure the GPU time of each part of a frame: [seconds]");

namespace rex {

namespace {

uint16_t QuickMenuComboButtons() {
  const std::string buttons = REXCVAR_GET(quick_menu_buttons);
  if (buttons == "none") {
    return 0;
  }
  if (buttons == "l3+r3") {
    return rex::input::X_INPUT_GAMEPAD_LEFT_THUMB | rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB;
  }
  return rex::input::X_INPUT_GAMEPAD_BACK | rex::input::X_INPUT_GAMEPAD_START;
}

}  // namespace

// --- ReXApp ---

ReXApp::~ReXApp() = default;

ReXApp::ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
               std::string_view usage)
    : WindowedApp(ctx, name, usage), ppc_info_(ppc_info) {}

std::unique_ptr<ui::ImGuiDialog> ReXApp::CreateAchievementsOverlay() {
  if (!runtime_ || !runtime_->kernel_state() || !imgui_drawer_ || !immediate_drawer_) {
    return nullptr;
  }
  return std::make_unique<ui::AchievementsOverlayDialog>(
      imgui_drawer_.get(), immediate_drawer_.get(), runtime_.get(), &achievements());
}

std::unique_ptr<ui::AchievementNotificationDialog> ReXApp::CreateAchievementNotificationDialog() {
  if (!imgui_drawer_ || !immediate_drawer_ || !runtime_) {
    return nullptr;
  }
  return std::make_unique<ui::AchievementToastDialog>(imgui_drawer_.get(), immediate_drawer_.get(),
                                                      runtime_.get());
}

system::AchievementManager& ReXApp::achievements() const {
  assert_not_null(runtime_);
  assert_not_null(runtime_->kernel_state());
  return runtime_->kernel_state()->achievements();
}

bool ReXApp::OnInitialize() {
  if (!SetupEnvironment())
    return false;
  if (!SetupPresentation())
    return false;
  SetupTestControl();

  auto paths = OnFinalizePaths(resolved_defaults_, MakeResumeCallback());
  if (!paths) {
    // Async: consumer will invoke resume when ready. OnInitialize returns
    // true so the event loop keeps pumping (wizard dialogs render).
    return true;
  }

  if (!ConstructRuntime(*paths))
    return false;
  LaunchModule();
  return true;
}

bool ReXApp::SetupEnvironment() {
  auto exe_dir = rex::filesystem::GetExecutableFolder();

  std::filesystem::path game_dir;
  std::string game_data_cvar = REXCVAR_GET(game_data_root);
  if (!game_data_cvar.empty()) {
    game_dir = game_data_cvar;
  }

  // User data: cvar override, or platform user directory
  std::filesystem::path user_dir;
  std::string user_data_cvar = REXCVAR_GET(user_data_root);
  if (!user_data_cvar.empty()) {
    user_dir = user_data_cvar;
  } else {
    user_dir = rex::filesystem::GetUserFolder() / GetName();
  }

  // Update data: cvar override, or empty (opt-in)
  std::filesystem::path update_dir;
  std::string update_data_cvar = REXCVAR_GET(update_data_root);
  if (!update_data_cvar.empty()) {
    update_dir = update_data_cvar;
  }

  // Cache: cvar override, or user_dir/cache
  std::filesystem::path cache_dir;
  std::string cache_root_cvar = REXCVAR_GET(cache_root);
  if (!cache_root_cvar.empty()) {
    cache_dir = cache_root_cvar;
  } else {
    cache_dir = user_dir / "cache";
  }

  std::filesystem::path metadata_dir;
  std::string metadata_root_cvar = REXCVAR_GET(metadata_root);
  if (!metadata_root_cvar.empty()) {
    metadata_dir = metadata_root_cvar;
  }

  PathConfig path_config{game_dir,  user_dir,     update_dir,
                         cache_dir, metadata_dir, exe_dir / (std::string(GetName()) + ".toml")};
  OnConfigurePaths(path_config);
  game_data_root_ = path_config.game_data_root;
  user_data_root_ = path_config.user_data_root;
  update_data_root_ = path_config.update_data_root;
  cache_root_ = path_config.cache_root;
  metadata_root_ = path_config.metadata_root;
  config_path_ = path_config.config_path;
  resolved_defaults_ = std::move(path_config);

  // Load config FIRST so log cvars have final values
  if (std::filesystem::exists(config_path_))
    rex::cvar::LoadConfig(config_path_);

  std::string log_level_str = REXCVAR_GET(log_level);
  if (REXCVAR_GET(log_verbose) && log_level_str == "info")
    log_level_str = "trace";

  auto log_config =
      rex::BuildLogConfig(log_level_str, rex::ParseCategoryLevelsFromConfig(config_path_));
  log_config.app_name = std::string(GetName());
  log_config.log_dir = exe_dir / "logs";
  OnConfigureLogging(log_config);
  rex::ApplyLogCvarOverrides(log_config);
  rex::InitLogging(log_config);
  rex::RegisterLogLevelCallback();

  log_sink_ = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(log_sink_);

  OnPostInitLogging();

  if (std::filesystem::exists(config_path_))
    REXLOG_DEBUG("Loaded config: {}", config_path_.filename().string());

  REXLOG_DEBUG("{} starting", GetName());
  if (!game_data_root_.empty()) {
    REXLOG_DEBUG("  Game directory: {}", game_data_root_.string());
  }
  if (!user_data_root_.empty()) {
    REXLOG_DEBUG("  User data:      {}", user_data_root_.string());
  }
  if (!update_data_root_.empty()) {
    REXLOG_DEBUG("  Update data:    {}", update_data_root_.string());
  }
  REXLOG_DEBUG("  Cache root:     {}", cache_root_.string());
  if (!metadata_root_.empty()) {
    REXLOG_DEBUG("  Metadata root:  {}", metadata_root_.string());
  }

  return true;
}

bool ReXApp::ConstructRuntime(const PathConfig& paths) {
  if (paths.game_data_root.empty()) {
    auto msg = std::string("--game_data_root was not provided.");
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }
  if (!std::filesystem::is_directory(paths.game_data_root)) {
    auto msg = fmt::format("--game_data_root does not exist: {}", paths.game_data_root.string());
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  game_data_root_ = paths.game_data_root;
  user_data_root_ = paths.user_data_root;
  update_data_root_ = paths.update_data_root;
  cache_root_ = paths.cache_root;
  metadata_root_ = paths.metadata_root;

  runtime_ =
      std::make_unique<rex::Runtime>(paths.game_data_root, paths.user_data_root,
                                     paths.update_data_root, paths.cache_root, paths.metadata_root);
  runtime_->set_app_context(&app_context());

  // Window and ImGui drawer already exist from SetupPresentation; publish them
  // to the runtime before Setup so hooks and native rendering see them.
  if (window_) {
    runtime_->set_display_window(window_.get());
  }
  if (imgui_drawer_) {
    runtime_->set_imgui_drawer(imgui_drawer_.get());
  }

  auto status = runtime_->Setup(ppc_info_, std::move(config_));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    return false;
  }

  if (window_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->AttachWindow(window_.get());
  }

  if (ppc_info_.register_modules) {
    ppc_info_.register_modules(runtime_->kernel_state());
  }

  if (auto* input_sys = static_cast<rex::input::InputSystem*>(runtime_->input_system())) {
    input_sys->SetActiveCallback([this]() {
      if (window_ && !window_->HasFocus())
        return false;
      // The quick menu reads the controllers itself (the guest is blocked
      // meanwhile), even with the mouse over it.
      if (quick_menu_open_.load(std::memory_order_relaxed))
        return true;
      if (!imgui_drawer_ ||
          (!debug_overlay_ && !console_overlay_ && !settings_overlay_ && !achievements_overlay_))
        return true;
      return !imgui_drawer_->GetIO().WantCaptureMouse;
    });
    if (!quick_menu_config_.sections.empty()) {
      input_sys->SetUIToggleCombo(QuickMenuComboButtons(), [this]() {
        app_context().CallInUIThreadDeferred([this]() { ToggleQuickMenu(); });
      });
      rex::cvar::RegisterChangeCallback(
          "quick_menu_buttons", [input_sys](std::string_view, std::string_view) {
            input_sys->SetUIToggleComboButtons(QuickMenuComboButtons());
          });
    }
  }

  std::string xex_image = "game:\\default.xex";
  OnLoadXexImage(xex_image);

  // Mirrors the game:\ / d:\ -> game_data_root mapping in Runtime::SetupVfs.
  {
    constexpr std::string_view kGameDevice = "game:\\";
    constexpr std::string_view kDDevice = "d:\\";
    std::string_view tail = xex_image;
    if (tail.starts_with(kGameDevice)) {
      tail.remove_prefix(kGameDevice.size());
    } else if (tail.starts_with(kDDevice)) {
      tail.remove_prefix(kDDevice.size());
    }
    std::string host_tail{tail};
    std::replace(host_tail.begin(), host_tail.end(), '\\', '/');
    auto xex_host = paths.game_data_root / host_tail;
    if (!std::filesystem::is_regular_file(xex_host)) {
      auto msg = fmt::format("Entrypoint XEX not found: {}", xex_host.string());
      REXLOG_ERROR("{}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      return false;
    }
  }

  status = runtime_->LoadXexImage(xex_image);
  if (XFAILED(status)) {
    auto msg = fmt::format("Failed to load XEX ({}): {:08X}", xex_image, status);
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  OnPostLoadXexImage();

  if (ppc_info_.rexcrt_heap) {
    if (!rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime_->memory())) {
      REXLOG_ERROR("Failed to initialize rexcrt heap");
      return false;
    }
  }

  OnPostSetup();

  return true;
}

bool ReXApp::SetupPresentation() {
  config_.gpu_plugin = REXCVAR_GET(gpu_plugin);
  config_.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
  config_.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
  config_.kernel_init = rex::kernel::InitializeKernel;

  OnPreSetup(config_);

  if (!config_.graphics && !config_.gpu_plugin.empty()) {
    config_.graphics = rex::system::LoadGpuPlugin(config_.gpu_plugin);
    if (!config_.graphics) {
      // Fatal by design: no silent headless fallback.
      auto msg =
          fmt::format("Failed to load GPU plugin '{}'. See log for details.", config_.gpu_plugin);
      REXLOG_ERROR("{}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      return false;
    }
  }

  if (config_.graphics) {
    X_STATUS status = config_.graphics->SetupPresentation(&app_context());
    if (XFAILED(status)) {
      REXLOG_ERROR("Graphics presentation setup failed: {:08X}", status);
      return false;
    }
  }

  // Create window
  window_ = rex::ui::Window::Create(app_context(), GetName());
  if (!window_) {
    REXLOG_ERROR("Failed to create window");
    return false;
  }

  // Set window title with SDK build stamp
  std::string title = std::string(GetName()) + " " + REXGLUE_BUILD_TITLE;
  window_->SetTitle(title);

  window_->AddListener(this);
  window_->AddInputListener(this, 0);

  if (REXCVAR_GET(fullscreen)) {
    window_->SetFullscreen(true);
  }
  window_->SetMonitor(REXCVAR_GET(monitor));

  auto on_window_cvar = [this](const char* name, std::function<void(std::string_view)> apply) {
    rex::cvar::RegisterChangeCallback(
        name, [this, apply = std::move(apply)](std::string_view, std::string_view value) {
          app_context().CallInUIThread([this, apply, value = std::string(value)] {
            if (window_) {
              apply(value);
            }
          });
        });
  };

  on_window_cvar("fullscreen", [this](std::string_view value) {
    window_->SetFullscreen(rex::string::from_string<bool>(value, false));
  });
  on_window_cvar("fullscreen_exclusive",
                 [this](std::string_view) { window_->RefreshFullscreen(); });
  on_window_cvar("monitor", [this](std::string_view value) {
    window_->SetMonitor(rex::string::from_string<int32_t>(value, 0));
  });
  auto apply_window_size = [this](std::string_view) {
    uint32_t width = 0;
    uint32_t height = 0;
    rex::ui::Window::ResolveConfiguredLogicalSize(width, height);
    window_->SetDesiredLogicalSize(width, height);
  };
  on_window_cvar("window_width", apply_window_size);
  on_window_cvar("window_height", apply_window_size);
  on_window_cvar("resolution", [this, apply_window_size](std::string_view value) {
    apply_window_size(value);
    window_->RefreshFullscreen();
  });

  window_->Open();

  auto* graphics_system = config_.graphics.get();
  if (graphics_system && graphics_system->presenter()) {
    // SDK mode: the emulated-Xenos presenter drives the overlays.
    auto* presenter = graphics_system->presenter();
    auto* provider = graphics_system->provider();
    if (provider) {
      immediate_drawer_ = provider->CreateImmediateDrawer();
      if (immediate_drawer_) {
        immediate_drawer_->SetPresenter(presenter);
        SetupOverlays(presenter, immediate_drawer_.get());
      }
    }
    window_->SetPresenter(presenter);

    // Presentation effects (FSR, CAS, dither) apply live. Deferred so the
    // presenter isn't touched from inside the cvar change (which the settings
    // overlay makes in the middle of a UI paint).
    for (const char* name :
         {"present_effect", "present_cas_additional_sharpness", "present_fsr_max_upsampling_passes",
          "present_fsr_sharpness_reduction", "present_fsr_quality_mode", "present_dither",
          "present_allow_overscan_cutoff", "dlss_mode", "fsr_mode"}) {
      // config_.graphics is handed over to the runtime during setup.
      rex::cvar::RegisterChangeCallback(name, [this](std::string_view, std::string_view) {
        app_context().CallInUIThreadDeferred([this] {
          auto* graphics = runtime_ ? runtime_->graphics_system() : config_.graphics.get();
          if (graphics && graphics->presenter()) {
            graphics->presenter()->RefreshGuestOutputPaintConfigFromCvarsFromUIThread();
          }
        });
      });
    }
  } else if (!graphics_system) {
    // Detached mode: the app brings its own renderer and drives its own paint
    // loop. ReXApp owns the returned drawer via immediate_drawer_.
    immediate_drawer_ = OnCreateImmediateDrawer();
    if (immediate_drawer_) {
      SetupOverlays(/*presenter=*/nullptr, immediate_drawer_.get());
      // No window_->SetPresenter, no drawer SetPresenter: the app owns the
      // surface and the present cadence.
    }
  }

  return true;
}

namespace {
std::atomic<ReXApp*> g_quick_menu_app{nullptr};
}  // namespace

void ReXApp::ToggleQuickMenuCommand() {
  if (ReXApp* app = g_quick_menu_app.load()) {
    app->app_context().CallInUIThreadDeferred([app]() { app->ToggleQuickMenu(); });
  }
}

void ReXApp::SetupOverlays(rex::ui::Presenter* presenter, rex::ui::ImmediateDrawer* drawer) {
  g_quick_menu_app.store(this);
  OnConfigureQuickMenu(quick_menu_config_);
  imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(
      window_.get(), 64,
      [this](ImFontAtlas* atlas) {
        ui::overlay_text::AddFonts(atlas);
        OnConfigureFonts(atlas);
      },
      [this](ImGuiStyle& imgui_style, rex::ui::Style& ui_style) {
        OnConfigureStyle(imgui_style, ui_style);
      });
  // presenter is nullptr in detached mode; ImGuiDrawer tolerates that and the
  // gated eager font upload in SetImmediateDrawer is skipped (font uploads
  // lazily on the first Draw instead).
  imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, drawer);
  ui::overlay_text::SetPixelScale(float(window_->GetDpi()) / float(window_->GetMediumDpi()));
  rex::ui::RegisterBind("bind_debug_overlay", "F3", "Toggle debug overlay", [] {
    rex::cvar::SetFlagByName("debug_overlay", REXCVAR_GET(debug_overlay) ? "false" : "true");
  });
  rex::cvar::RegisterChangeCallback("debug_overlay", [this](std::string_view, std::string_view) {
    app_context().CallInUIThreadDeferred([this]() { ApplyDebugOverlaySetting(); });
  });
  ApplyDebugOverlaySetting();
  if (!quick_menu_config_.sections.empty()) {
    rex::ui::RegisterBind("bind_quick_menu", "F1", "Toggle the settings menu",
                          [this] { ToggleQuickMenu(); });
  }
  rex::ui::RegisterBind("bind_console", "Backtick", "Toggle console overlay", [this] {
    if (console_overlay_) {
      console_overlay_.reset();
      REXLOG_INFO("Console overlay closed");
    } else {
      console_overlay_ = std::make_unique<ui::ConsoleDialog>(imgui_drawer_.get(), log_sink_);
      REXLOG_INFO("Console overlay opened");
    }
  });
  rex::ui::RegisterBind("bind_settings", "F4", "Toggle settings overlay", [this] {
    if (settings_overlay_) {
      settings_overlay_.reset();
    } else {
      settings_overlay_ = std::make_unique<ui::SettingsDialog>(imgui_drawer_.get(), config_path_);
    }
  });
  rex::ui::RegisterBind("bind_achievements", "F7", "Toggle achievements overlay", [this] {
    if (achievements_overlay_) {
      achievements_overlay_.reset();
    } else {
      achievements_overlay_ = CreateAchievementsOverlay();
    }
  });

  OnCreateDialogs(imgui_drawer_.get());
}

void ReXApp::LaunchModule() {
  app_context().CallInUIThreadDeferred([this]() {
    // Register the achievement notification callback now that the runtime and
    // KernelState are guaranteed to exist. Done here (not OnCreateDialogs)
    // because KernelState is null during SetupPresentation.
    if (!achievement_notification_) {
      achievement_notification_ =
          std::shared_ptr<ui::AchievementNotificationDialog>(CreateAchievementNotificationDialog());
    }
    if (achievement_notification_ && achievement_notification_listener_ == 0 && runtime_ &&
        runtime_->kernel_state()) {
      std::weak_ptr<ui::AchievementNotificationDialog> notification = achievement_notification_;
      achievement_notification_listener_ = achievements().RegisterNotificationCallback(
          [notification](const rex::system::AchievementEvent& event) {
            if (auto dialog = notification.lock()) {
              dialog->Push(event);
            }
          });
    }

    OnPreLaunchModule();

    auto main_thread = runtime_->PrepareModuleLaunch();
    if (!main_thread) {
      REXLOG_ERROR("Failed to launch module");
      app_context().QuitFromUIThread();
      return;
    }

    auto* graphics_system = runtime_->graphics_system();
    if (graphics_system && !runtime_->cache_root().empty()) {
      uint32_t title_id = runtime_->kernel_state()->title_id();
      if (title_id != 0) {
        REXLOG_INFO("Initializing shader storage for title {:08X}...", title_id);
        graphics_system->InitializeShaderStorage(runtime_->cache_root(), title_id, true);
      }
    }

    OnPostLaunchModule(main_thread.get());
    main_thread->Resume();

    module_thread_ = std::thread([this, main_thread = std::move(main_thread)]() mutable {
      main_thread->Wait(0, 0, 0, nullptr);
      OnGuestThreadExit(main_thread.get());
      REXLOG_INFO("Execution complete");
      if (!shutting_down_.load(std::memory_order_acquire)) {
        app_context().CallInUIThread([this]() { app_context().QuitFromUIThread(); });
      }
    });
  });
}

std::function<void(PathConfig)> ReXApp::MakeResumeCallback() {
  return [this](PathConfig paths) {
    if (shutting_down_.load(std::memory_order_acquire))
      return;
    if (!ConstructRuntime(std::move(paths))) {
      app_context().QuitFromUIThread();
      return;
    }
    LaunchModule();
  };
}

void ReXApp::OnKeyDown(ui::KeyEvent& e) {
  // A key setting in the quick menu waiting for a key gets it, not the binds.
  if (ui::QuickMenuDialog::CaptureKey(e)) {
    e.set_handled(true);
    return;
  }
  // While a text box (the console) has the keyboard, the keys that type or
  // edit text are for it; the function keys and the keys of the console and
  // the settings menu (wherever they're bound) still work.
  if (window_ && window_->IsTextInputActive()) {
    const auto key = e.virtual_key();
    const std::string owner = rex::ui::FindBindForKey(key);
    const bool command_key =
        (key >= ui::VirtualKey::kF1 && key <= ui::VirtualKey::kF24) ||
        owner == "bind_console" || owner == "bind_quick_menu" ||
        key == ui::VirtualKey::kEscape || key == ui::VirtualKey::kPause ||
        key == ui::VirtualKey::kSnapshot || key == ui::VirtualKey::kScroll;
    if (!command_key) {
      return;
    }
  }
  rex::ui::ProcessKeyEvent(e);
}

void ReXApp::OnClosing(ui::UIEvent& e) {
  (void)e;
  REXLOG_INFO("Window closing, shutting down...");
  shutting_down_.store(true, std::memory_order_release);
  if (runtime_ && runtime_->kernel_state()) {
    runtime_->kernel_state()->TerminateTitle();
  }
  // Hard-exit rather than run subsystem teardown, which can deadlock on a host
  // lock still held by a straggler TerminateTitle left running. Flush (not
  // ShutdownLogging, which frees loggers a straggler may still use); the OS
  // reclaims the rest.
  REXLOG_INFO("Title terminated; hard-exiting process.");
  rex::FlushLogging();
  std::_Exit(0);
}

bool ReXApp::OnCloseRequested(ui::UIEvent& e) {
  (void)e;
  return OnWindowCloseRequested();
}

void ReXApp::OnResize(ui::UISetupEvent& e) {
  (void)e;
  if (!window_) {
    return;
  }
  OnWindowPixelSizeChanged(window_->GetActualPhysicalWidth(), window_->GetActualPhysicalHeight());
  OnWindowResized(window_->GetActualLogicalWidth(), window_->GetActualLogicalHeight());
}

void ReXApp::OnDpiChanged(ui::UISetupEvent& e) {
  (void)e;
  if (!window_) {
    return;
  }
  const float scale = float(window_->GetDpi()) / float(window_->GetMediumDpi());
  ui::overlay_text::SetPixelScale(scale);
  OnDpiScaleChanged(scale);
}

void ReXApp::OnGotFocus(ui::UISetupEvent& e) {
  (void)e;
  OnWindowFocusChanged(true);
}

void ReXApp::OnLostFocus(ui::UISetupEvent& e) {
  (void)e;
  OnWindowFocusChanged(false);
}

void ReXApp::OnMinimized(ui::UIEvent& e) {
  (void)e;
  OnWindowMinimized();
}

void ReXApp::OnRestored(ui::UIEvent& e) {
  (void)e;
  OnWindowRestored();
}

void ReXApp::OnDestroy() {
  // No more pipe lines or screenshots once teardown starts.
  if (command_pipe_) {
    command_pipe_->Stop();
    command_pipe_.reset();
  }
  g_screenshot_presenter = nullptr;

  // Notify subclass before cleanup
  OnShutdown();

  // Unregister overlay keybinds before destroying dialogs
  rex::ui::UnregisterBind("bind_debug_overlay");
  rex::ui::UnregisterBind("bind_console");
  rex::ui::UnregisterBind("bind_settings");
  rex::ui::UnregisterBind("bind_achievements");
  rex::ui::UnregisterBind("bind_quick_menu");
  rex::cvar::UnregisterChangeCallbacks("debug_overlay");

  // ImGui cleanup (reverse of setup)
  if (achievement_notification_listener_ != 0) {
    if (runtime_ && runtime_->kernel_state()) {
      achievements().UnregisterCallback(achievement_notification_listener_);
    }
    achievement_notification_listener_ = 0;
  }
  CloseQuickMenu();
  achievement_notification_.reset();
  achievements_overlay_.reset();
  settings_overlay_.reset();
  console_overlay_.reset();
  debug_overlay_.reset();
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  // immediate_drawer_ was already unlinked from imgui_drawer_ above. Detach it
  // from its presenter so SDK mode runs OnLeavePresenter() before disposal; in
  // detached mode the drawer never had a presenter, so SetPresenter(nullptr) is
  // a no-op.
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
  if (runtime_) {
    runtime_->set_display_window(nullptr);
    runtime_->set_imgui_drawer(nullptr);
  }
  // Window/runtime cleanup
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  if (module_thread_.joinable()) {
    module_thread_.join();
  }
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
  window_.reset();
  runtime_.reset();
}

void ReXApp::SetGuestFrameStats(ui::DebugOverlayDialog::FrameStatsProvider provider) {
  frame_stats_provider_ = provider;
  if (debug_overlay_) {
    debug_overlay_->SetStatsProvider(provider);
  }
}

void ReXApp::SetupTestControl() {
  g_screenshot_presenter = [this]() -> ui::Presenter* {
    // config_.graphics is handed over to the runtime during setup.
    auto* graphics = runtime_ ? runtime_->graphics_system() : config_.graphics.get();
    return graphics ? graphics->presenter() : nullptr;
  };
  const std::string pipe_name = REXCVAR_GET(debug_command_pipe);
  if (pipe_name.empty() || command_pipe_) {
    return;
  }
  // Lines run where the console runs them: on the UI thread.
  ui::WindowedAppContext* context = &app_context();
  command_pipe_ = std::make_unique<ui::DebugCommandPipe>();
  if (!command_pipe_->Start(pipe_name, [context](std::function<void()> function) {
        return context->CallInUIThread(std::move(function));
      })) {
    command_pipe_.reset();
  }
}

void ReXApp::ApplyDebugOverlaySetting() {
  if (!imgui_drawer_ || shutting_down_.load(std::memory_order_acquire)) {
    return;
  }
  if (!REXCVAR_GET(debug_overlay)) {
    debug_overlay_.reset();
  } else if (!debug_overlay_) {
    debug_overlay_ =
        std::make_unique<ui::DebugOverlayDialog>(imgui_drawer_.get(), frame_stats_provider_);
  }
}

void ReXApp::ToggleQuickMenu() {
  if (quick_menu_) {
    CloseQuickMenu();
    return;
  }
  if (!imgui_drawer_ || quick_menu_config_.sections.empty() ||
      shutting_down_.load(std::memory_order_acquire)) {
    return;
  }
  auto* input_sys =
      runtime_ ? static_cast<rex::input::InputSystem*>(runtime_->input_system()) : nullptr;

  ui::QuickMenuDialog::Callbacks callbacks;
  callbacks.read_pad = [input_sys]() {
    ui::QuickMenuDialog::PadState pad;
    if (!input_sys) {
      // Before the game runs (a start screen): the controllers from SDL.
      return ui::ReadGamepadsBeforeGame();
    }
    // Any controller can drive the menu; the stick pushed the furthest wins.
    int stick_distance = 0;
    for (uint32_t user_index = 0; user_index < rex::input::kMaxGuestUsers; ++user_index) {
      rex::input::X_INPUT_STATE state = {};
      if (input_sys->GetStateForUI(user_index, &state) != X_ERROR_SUCCESS) {
        continue;
      }
      pad.buttons |= static_cast<uint16_t>(state.gamepad.buttons);
      const int16_t thumb_lx = state.gamepad.thumb_lx;
      const int16_t thumb_ly = state.gamepad.thumb_ly;
      const int distance = std::abs(int(thumb_lx)) + std::abs(int(thumb_ly));
      if (distance > stick_distance) {
        stick_distance = distance;
        pad.thumb_lx = thumb_lx;
        pad.thumb_ly = thumb_ly;
      }
    }
    return pad;
  };
  callbacks.defer = [this](std::function<void()> function) {
    app_context().CallInUIThreadDeferred(std::move(function));
  };
  callbacks.close = [this]() { CloseQuickMenu(); };
  callbacks.changed = [this]() { rex::cvar::SaveConfig(config_path_); };

  quick_menu_ =
      std::make_unique<ui::QuickMenuDialog>(imgui_drawer_.get(), quick_menu_config_, std::move(callbacks));
  quick_menu_open_.store(true, std::memory_order_relaxed);
  // The game doesn't see the controllers while the menu is open.
  if (input_sys) {
    input_sys->AddUIInputBlocker();
    quick_menu_blocks_input_ = true;
  }
}

void ReXApp::CloseQuickMenu() {
  if (!quick_menu_) {
    return;
  }
  quick_menu_.reset();
  // Unblocked while still counting as open, so the buttons held right now are
  // read (and kept from the guest) even with the mouse over an overlay.
  if (quick_menu_blocks_input_) {
    quick_menu_blocks_input_ = false;
    if (auto* input_sys =
            runtime_ ? static_cast<rex::input::InputSystem*>(runtime_->input_system()) : nullptr) {
      input_sys->RemoveUIInputBlocker();
    }
  }
  quick_menu_open_.store(false, std::memory_order_relaxed);
}

}  // namespace rex
