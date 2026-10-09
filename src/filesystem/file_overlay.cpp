/**
 * @file        filesystem/file_overlay.cpp
 *
 * @brief       File overlays. See rex/filesystem/file_overlay.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/filesystem/file_overlay.h>

#include <mutex>

namespace rex::filesystem {

namespace {

std::mutex& ProviderMutex() {
  static std::mutex mutex;
  return mutex;
}

FileOverlayProvider& Provider() {
  static FileOverlayProvider provider;
  return provider;
}

}  // namespace

void SetFileOverlayProvider(FileOverlayProvider provider) {
  std::lock_guard<std::mutex> lock(ProviderMutex());
  Provider() = std::move(provider);
}

std::shared_ptr<FileOverlay> FindFileOverlay(const std::filesystem::path& host_path) {
  FileOverlayProvider provider;
  {
    std::lock_guard<std::mutex> lock(ProviderMutex());
    provider = Provider();
  }
  return provider ? provider(host_path) : nullptr;
}

}  // namespace rex::filesystem
