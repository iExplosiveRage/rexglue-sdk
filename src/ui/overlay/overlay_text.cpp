/**
 * @file        ui/overlay/overlay_text.cpp
 *
 * @brief       Overlay text. See overlay_text.h for details.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/overlay_text.h>

#include <atomic>
#include <cfloat>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <rex/platform.h>

namespace rex::ui::overlay_text {

namespace {

// The renderer can't bake glyphs on demand, so text is scaled from the closest
// of these sizes.
constexpr float kSizeSmall = 36.0f;
constexpr float kSizeLarge = 72.0f;
ImFont* font_small = nullptr;
ImFont* font_large = nullptr;
std::atomic<float> pixel_scale{1.0f};

}  // namespace

void AddFonts(ImFontAtlas* atlas) {
  if (!atlas || font_small) {
    return;
  }
  static const ImWchar kGlyphRanges[] = {0x0020, 0x00FF, 0};
  // Bold sans fonts of Linux distributions (Fedora, Arch, Debian / Ubuntu) -
  // for native builds, and through Wine's Z: drive for Windows builds running
  // under Wine / Proton, which don't have the Windows fonts.
  static const char* const kLinuxFontPaths[] = {
      "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf",
      "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
      "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
      "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/noto/NotoSans-Bold.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
      "/usr/share/fonts/open-sans/OpenSans-Bold.ttf",
  };
  std::vector<std::string> font_paths;
#if REX_PLATFORM_WIN32
  font_paths = {
      "C:\\Windows\\Fonts\\segoeuib.ttf",
      "C:\\Windows\\Fonts\\seguisb.ttf",
      "C:\\Windows\\Fonts\\arialbd.ttf",
  };
  for (const char* path : kLinuxFontPaths) {
    font_paths.push_back(std::string("Z:") + path);
  }
#else
  font_paths.assign(std::begin(kLinuxFontPaths), std::end(kLinuxFontPaths));
  font_paths.push_back("/System/Library/Fonts/Supplemental/Arial Bold.ttf");
#endif
  for (const std::string& font_path : font_paths) {
    const char* path = font_path.c_str();
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
      continue;
    }
    ImFontConfig config;
    font_small = atlas->AddFontFromFileTTF(path, kSizeSmall, &config, kGlyphRanges);
    font_large = atlas->AddFontFromFileTTF(path, kSizeLarge, &config, kGlyphRanges);
    break;
  }
}

void SetPixelScale(float scale) {
  pixel_scale.store(scale > 0.1f ? scale : 1.0f, std::memory_order_relaxed);
}

ImFont* Font(float size) {
  if (font_large && size * pixel_scale.load(std::memory_order_relaxed) > kSizeSmall * 1.3f) {
    return font_large;
  }
  return font_small ? font_small : ImGui::GetFont();
}

ImVec2 Measure(float size, std::string_view text) {
  return Font(size)->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data(), text.data() + text.size());
}

void Draw(ImDrawList* draw_list, float size, ImVec2 position, ImU32 color, std::string_view text,
          float wrap_width) {
  draw_list->AddText(Font(size), size, position, color, text.data(), text.data() + text.size(),
                     wrap_width);
}

}  // namespace rex::ui::overlay_text
