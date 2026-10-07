/**
 * @file        rex/ui/image_encode.h
 * @brief       Host-side PNG writer for captured frames (screenshots).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <filesystem>
#include <string>

#include <rex/ui/presenter.h>

namespace rex::ui {

/// Writes `image` (R8 G8 B8 X8, as Presenter::CaptureGuestOutput returns it)
/// to `path` as a 24-bit PNG, creating missing parent folders. The file only
/// appears once it's complete (written next to it, then renamed). Any thread.
/// Windows only for now (WIC); elsewhere it fails with a message in `error`.
bool WriteImagePng(const RawImage& image, const std::filesystem::path& path, std::string* error);

}  // namespace rex::ui
