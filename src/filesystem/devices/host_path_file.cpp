/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/devices/host_path_file.h>

namespace rex::filesystem {

HostPathFile::HostPathFile(uint32_t file_access, HostPathEntry* entry,
                           std::unique_ptr<rex::filesystem::FileHandle> file_handle)
    : File(file_access, entry), file_handle_(std::move(file_handle)) {}

HostPathFile::~HostPathFile() = default;

void HostPathFile::Destroy() {
  delete this;
}

X_STATUS HostPathFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                size_t* out_bytes_read) {
  if (!(file_access_ & (FileAccess::kGenericRead | FileAccess::kFileReadData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->Read(byte_offset, buffer.data(), buffer.size(), out_bytes_read)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_written) {
  if (!(file_access_ &
        (FileAccess::kGenericWrite | FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->Write(byte_offset, buffer.data(), buffer.size(), out_bytes_written)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::SetLength(size_t length) {
  if (!(file_access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->SetLength(length)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

HostPathOverlayFile::HostPathOverlayFile(uint32_t file_access, HostPathEntry* entry,
                                         std::shared_ptr<FileOverlay> overlay)
    : File(file_access, entry), overlay_(std::move(overlay)) {}

HostPathOverlayFile::~HostPathOverlayFile() = default;

void HostPathOverlayFile::Destroy() {
  delete this;
}

X_STATUS HostPathOverlayFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                       size_t* out_bytes_read) {
  if (!(file_access_ & (FileAccess::kGenericRead | FileAccess::kFileReadData))) {
    return X_STATUS_ACCESS_DENIED;
  }
  size_t bytes_read = 0;
  if (!overlay_->Read(byte_offset, buffer.data(), buffer.size(), &bytes_read)) {
    return X_STATUS_END_OF_FILE;
  }
  if (out_bytes_read) {
    *out_bytes_read = bytes_read;
  }
  return bytes_read || buffer.empty() ? X_STATUS_SUCCESS : X_STATUS_END_OF_FILE;
}

X_STATUS HostPathOverlayFile::WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                                        size_t* out_bytes_written) {
  (void)buffer;
  (void)byte_offset;
  (void)out_bytes_written;
  return X_STATUS_ACCESS_DENIED;
}

X_STATUS HostPathOverlayFile::SetLength(size_t length) {
  (void)length;
  return X_STATUS_ACCESS_DENIED;
}

}  // namespace rex::filesystem
