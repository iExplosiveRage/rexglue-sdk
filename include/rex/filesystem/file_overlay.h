/**
 * @file        rex/filesystem/file_overlay.h
 *
 * @brief       Serving a host file to the guest with other contents, without
 *              changing it on disk - e.g. a game archive with modded files.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace rex::filesystem {

// What the guest reads instead of a host file. The guest sees size() as the
// file's size and gets its bytes from Read(); it can't write the file.
class FileOverlay {
 public:
  virtual ~FileOverlay() = default;

  virtual uint64_t size() const = 0;

  // Reads up to `length` bytes at `offset` (fewer at the end of the file).
  // Called from any guest thread, possibly several at once.
  virtual bool Read(uint64_t offset, void* buffer, size_t length, size_t* out_bytes_read) = 0;
};

// Asked for every file of a host path device as it is listed (normally when
// the device is mounted, so set it before the runtime is set up): returns the
// overlay to serve for that host file, or nullptr to serve the file itself.
using FileOverlayProvider =
    std::function<std::shared_ptr<FileOverlay>(const std::filesystem::path& host_path)>;

void SetFileOverlayProvider(FileOverlayProvider provider);

// The provider's answer for a host file (nullptr without a provider).
std::shared_ptr<FileOverlay> FindFileOverlay(const std::filesystem::path& host_path);

}  // namespace rex::filesystem
