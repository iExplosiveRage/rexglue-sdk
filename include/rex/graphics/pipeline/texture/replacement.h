/**
 * @file        rex/graphics/pipeline/texture/replacement.h
 *
 * @brief       Texture dump and replacement pipeline.
 *
 *              Textures are identified by a stable content hash (XXH3 over the
 *              raw guest bytes as seen in physical memory — tiled and
 *              big-endian, exactly as the Xbox GPU would read them).  This
 *              makes the hash address-independent and run-independent.
 *
 *              Dump layout  (relative to the configured textures folder):
 *                  dump/<hash16>_<w>x<h>_<fmt_name>.dds
 *
 *              Replacement layout (scanned once at init, hot-reloaded on demand via Rescan()):
 *                  replace/<hash16>.dds   (RGBA8/BGRA8 or BC1/BC2/BC3)
 *                  replace/<hash16>.png   (RGBA8; loaded via stb_image)
 *
 *              Dump DDS format:
 *                - Compressed formats (DXT1/DXT3/DXT5/DXN/CTX1/DXT3A/DXT5A):
 *                    FOURCC DDS with raw BC blocks — lossless, directly usable
 *                    as a base for replacement authoring.
 *                - Uncompressed formats: RGBA8 unorm after untiling + endian
 *                    swap + channel expansion to 8bpc.
 *
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>

namespace rex::thread {
class Thread;
}  // namespace rex::thread

// CVARs controlling the dump/replace pipeline (defined in cache.cpp).
REXCVAR_DECLARE(bool, texture_dump_enabled);
REXCVAR_DECLARE(bool, texture_replace_enabled);
REXCVAR_DECLARE(bool, texture_replace_preload);
REXCVAR_DECLARE(int32_t, texture_replace_ram_mb);
REXCVAR_DECLARE(std::string, texture_folder);

namespace rex::graphics {

// ---------------------------------------------------------------------------
// Replacement descriptor returned to the injection path
// ---------------------------------------------------------------------------
struct TextureReplacementData {
  // Decoded pixels: RGBA8 unorm, row-major, tightly packed (no row padding).
  std::vector<uint8_t> pixels;
  uint32_t width = 0;
  uint32_t height = 0;
  // Number of mip levels present in the replacement file (>= 1).
  uint32_t mip_levels = 1;
  // Smaller levels made from `pixels` (level 1 first, down to 1x1), RGBA8 like
  // it.
  std::vector<std::vector<uint8_t>> mips;
};

// ---------------------------------------------------------------------------
// TextureReplacement
// ---------------------------------------------------------------------------
class TextureReplacement {
 public:
  explicit TextureReplacement(std::filesystem::path textures_dir);
  ~TextureReplacement();

  TextureReplacement(const TextureReplacement&) = delete;
  TextureReplacement& operator=(const TextureReplacement&) = delete;

  // Rescans textures/replace/ and rebuilds the hash→path index.
  void Rescan();

  // ---------------------------------------------------------------------------
  // Dump path
  // ---------------------------------------------------------------------------
  // Untiles, endian-swaps, and writes the guest texture to a DDS file.
  //   guest_bytes  : raw physical memory at base_page << 12
  //   guest_size   : GetGuestBaseSize() bytes
  //   width/height : texel dimensions
  //   pitch_blocks : TextureKey::pitch (pitch in units of 32 texels)
  //   tiled        : TextureKey::tiled
  //   format       : TextureKey::format
  //   endianness   : TextureKey::endianness
  void DumpTexture(uint64_t content_hash, uint32_t width, uint32_t height, uint32_t pitch_blocks,
                   bool tiled, xenos::TextureFormat format, xenos::Endian endianness,
                   const uint8_t* guest_bytes, uint32_t guest_size) const;

  // ---------------------------------------------------------------------------
  // Injection path
  // ---------------------------------------------------------------------------
  // Returns a pointer into the internal cache, or nullptr if not found.
  // The pointer is valid until the next call to Rescan(). The first call
  // starts decoding all the files in the background (texture_replace_preload),
  // ones not decoded yet are loaded here.
  [[nodiscard]] const TextureReplacementData* FindReplacement(uint64_t content_hash) const;
  // Whether there's a replacement for the hash, and its size - from the file's
  // header, without decoding it. The same as FindReplacement's width and
  // height when that succeeds.
  bool FindReplacementSize(uint64_t content_hash, uint32_t& width, uint32_t& height) const;
  // A replacement that couldn't be used (e.g. a format the GPU upload doesn't
  // take): it isn't offered again, so the original texture is used.
  void MarkFailed(uint64_t content_hash) const;

  // The smaller mips of an RGBA8 image down to 1x1: each texel is the average
  // of 2x2 of the level above, weighted by alpha so the color of transparent
  // texels doesn't bleed into the edges of cutouts.
  static std::vector<std::vector<uint8_t>> BuildMips(const uint8_t* pixels, uint32_t width,
                                                     uint32_t height);

  // ---------------------------------------------------------------------------
  // Hash
  // ---------------------------------------------------------------------------
  static uint64_t HashGuestData(const uint8_t* data, size_t size);
  // Cheap check of whether the content behind a cached hash is still the same
  // (samples of it, or all of a small one).
  static uint64_t FingerprintGuestData(const uint8_t* data, size_t size);

  std::filesystem::path dump_dir() const { return textures_dir_ / "dump"; }
  std::filesystem::path replace_dir() const { return textures_dir_ / "replace"; }

 private:
  // Decodes a replacement file and makes its mips.
  static bool LoadFile(const std::filesystem::path& path, TextureReplacementData& out);
  // RAM an image file takes decoded (RGBA8 with its mips), from its header; 0
  // if it can't be read.
  static uint64_t DecodedSize(const std::filesystem::path& path);
  // An image file's size from its header (PNG or DDS); false if unreadable.
  static bool ReadImageSize(const std::filesystem::path& path, uint32_t& width, uint32_t& height);
  // Bytes of a decoded texture in pixel_cache_.
  static uint64_t CachedSize(const TextureReplacementData& data);
  // Adds a decoded texture to pixel_cache_ (cache_mutex_ held).
  TextureReplacementData* AddToPixelCache(uint64_t hash, TextureReplacementData&& data) const;
  // Drops the least recently used decoded textures, other than `keep`, while
  // pixel_cache_ is over texture_replace_ram_mb (cache_mutex_ held). Only
  // FindReplacement calls it: the pointers it returned before are used before
  // it's called again.
  void TrimPixelCache(uint64_t keep) const;
  // Decodes the indexed files on background threads.
  void StartPreload() const;
  void StopPreload();
  void PreloadWorker() const;

  std::filesystem::path textures_dir_;
  std::unordered_map<uint64_t, std::filesystem::path> replacements_;

  // Textures that have been loaded from disk are cached here so that
  // FindReplacement doesn't touch the filesystem again while they're in RAM.
  // Elements are never moved, so pointers to them stay valid while others are
  // added; the least recently used ones are dropped past texture_replace_ram_mb
  // (TrimPixelCache).
  mutable std::unordered_map<uint64_t, TextureReplacementData> pixel_cache_;
  // Per cached texture: its size and when it was last asked for.
  struct PixelCacheUse {
    uint64_t bytes = 0;
    uint64_t last_use = 0;
  };
  mutable std::unordered_map<uint64_t, PixelCacheUse> pixel_cache_use_;
  mutable uint64_t pixel_cache_bytes_ = 0;
  mutable uint64_t pixel_cache_clock_ = 0;
  // texture_replace_ram_mb in bytes, or the whole preloaded pack if that's
  // bigger (it fit the budget when the preload started).
  mutable uint64_t pixel_cache_budget_ = 0;
  // Hashes that failed to load are remembered so we don't retry every frame.
  mutable std::unordered_set<uint64_t> failed_cache_;
  // Sizes of the replacement files read so far (FindReplacementSize).
  mutable std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> size_cache_;
  // Guards the caches, shared with the preload threads.
  mutable std::mutex cache_mutex_;
  // Hashes being decoded right now (by the preload or FindReplacement), and
  // the signal for when one of them is done.
  mutable std::unordered_set<uint64_t> preload_in_progress_;
  mutable std::condition_variable preload_done_;

  mutable bool preload_started_ = false;
  mutable std::vector<std::pair<uint64_t, std::filesystem::path>> preload_queue_;
  mutable std::atomic<size_t> preload_next_{0};
  mutable std::atomic<size_t> preload_remaining_{0};
  mutable std::atomic<bool> preload_stop_{false};
  mutable std::chrono::steady_clock::time_point preload_start_time_;
  mutable std::vector<std::unique_ptr<rex::thread::Thread>> preload_threads_;

  static bool WriteDDS_RGBA8(const std::filesystem::path& path, uint32_t width, uint32_t height,
                             const uint8_t* rgba8_rows, uint32_t row_pitch_bytes);

  static bool WriteDDS_BC(const std::filesystem::path& path, uint32_t width, uint32_t height,
                          const uint8_t* bc_blocks, uint32_t bytes_per_block, uint32_t fourcc);

  static bool ReadDDS(const std::filesystem::path& path, TextureReplacementData& out);

  static bool ReadPNG(const std::filesystem::path& path, TextureReplacementData& out);
};

}  // namespace rex::graphics
