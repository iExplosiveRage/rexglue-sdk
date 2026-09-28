/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/logging.h>
#include <rex/thread.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length || buffer_length == 24);
      // dword r3 user index
      // dword (unwritten?)
      // qword 0
      // dword r4 context enum
      // dword r5 value
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t context_value = memory::load_and_swap<uint32_t>(buffer + 20);
      REXKRNL_DEBUG("XGIUserSetContextEx({:08X}, {:08X}, {:08X})", user_index, context_id,
                    context_value);
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t property_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t value_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t value_ptr = memory::load_and_swap<uint32_t>(buffer + 24);
      REXKRNL_DEBUG("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})", user_index, property_id,
                    value_size, value_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      // Raw dump so we can confirm the actual buffer layout the game sends.
      uint32_t raw0 = buffer_length >= 4 ? memory::load_and_swap<uint32_t>(buffer + 0) : 0;
      uint32_t raw4 = buffer_length >= 8 ? memory::load_and_swap<uint32_t>(buffer + 4) : 0;
      REXKRNL_INFO("XGIUserWriteAchievements called: buf_len={} raw[0]={:08X} raw[4]={:08X}",
                   buffer_length, raw0, raw4);

      assert_true(!buffer_length || buffer_length == 8);
      uint32_t achievement_count = raw0;
      uint32_t achievements_ptr = raw4;

      // Empirically confirmed from log: each entry is {u32 padding/user_index, u32 id, ...}.
      // The achievement ID sits at offset 4, not 0. Stride 8 covers the observed fields.
      constexpr uint32_t kEntryIdOffset = 4;
      constexpr uint32_t kEntryStride = 8;
      constexpr uint32_t kMaxAchievements = 1000;

      if (achievements_ptr && achievement_count > 0) {
        if (achievement_count > kMaxAchievements) {
          REXKRNL_WARN("XGIUserWriteAchievements: count={} unreasonable, ignoring",
                       achievement_count);
          return X_E_FAIL;
        }
        uint32_t span_end = achievements_ptr + achievement_count * kEntryStride - 1;
        if (!memory_->LookupHeap(achievements_ptr) || !memory_->LookupHeap(span_end)) {
          REXKRNL_WARN("XGIUserWriteAchievements: ptr {:08X} OOB", achievements_ptr);
          return X_E_FAIL;
        }
        auto* base = memory_->TranslateVirtual(achievements_ptr);
        for (uint32_t i = 0; i < achievement_count; ++i) {
          uint32_t id = memory::load_and_swap<uint32_t>(base + i * kEntryStride + kEntryIdOffset);
          REXKRNL_INFO("XGIUserWriteAchievements: id={} ({})", id, i);
          kernel_state_->UnlockAchievement(id);
        }
      } else {
        REXKRNL_INFO("XGIUserWriteAchievements: skipped (count={} ptr={:08X})", achievement_count,
                     achievements_ptr);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_slots_public = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t num_slots_private = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t user_xuid = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t nonce_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG(
          "XGISessionCreateImpl({:08X}, {:08X}, {}, {}, {:08X}, {:08X}, {:08X})",
          session_ptr, flags, num_slots_public, num_slots_private, user_xuid, session_info_ptr,
          nonce_ptr);

      // Bit 0 means this side is hosting. When joining, the game has already
      // copied the host XSESSION_INFO from XSessionSearchEx, so preserve it.
      if ((flags & 0x01u) == 0) {
        REXKRNL_WARN(
            "[BurstSession] join-side: preserving search XSESSION_INFO "
            "(flags={:08X}, info={:08X})",
            flags, session_info_ptr);
        return X_E_SUCCESS;
      }

      auto session_info = memory_->TranslateVirtual(session_info_ptr);
      auto nonce_out = memory_->TranslateVirtual(nonce_ptr);

      if (!session_info || !nonce_out) {
        REXKRNL_WARN("[BurstSession] invalid output pointer(s): info={:08X} nonce={:08X}",
                     session_info_ptr, nonce_ptr);
        return X_E_FAIL;
      }

      uint8_t ip[4] = {127, 0, 0, 1};
      const char* configured_ip = std::getenv("REX_XNET_IP");

      unsigned p0 = 0, p1 = 0, p2 = 0, p3 = 0;
      if (configured_ip &&
          std::sscanf(configured_ip, "%u.%u.%u.%u", &p0, &p1, &p2, &p3) == 4 &&
          p0 <= 255 && p1 <= 255 && p2 <= 255 && p3 <= 255) {
        ip[0] = static_cast<uint8_t>(p0);
        ip[1] = static_cast<uint8_t>(p1);
        ip[2] = static_cast<uint8_t>(p2);
        ip[3] = static_cast<uint8_t>(p3);
      } else {
        REXKRNL_WARN("[BurstSession] REX_XNET_IP missing or invalid; using loopback.");
      }

      // XSESSION_INFO:
      // +0x00 XNKID session ID        (preserved for now)
      // +0x08 XNADDR host address     (0x24 bytes)
      // +0x2C XNKEY exchange key      (preserved for now)
      // XNKID: 8 raw big-endian bytes. 0xAE marks an Xbox LIVE peer session.
      // It is deterministic from the host Radmin IP so the client can rebuild it later.
      session_info[0x00] = 0xAE;
      session_info[0x01] = ip[0];
      session_info[0x02] = ip[1];
      session_info[0x03] = ip[2];
      session_info[0x04] = ip[3];
      session_info[0x05] = 0x52;  // R
      session_info[0x06] = 0x58;  // X
      session_info[0x07] = 0x01;

      // XNKEY identity exchange key: bytes 00..0F, same basic shape Xenia uses.
      for (uint32_t i = 0; i < 16; ++i) {
        session_info[0x2C + i] = static_cast<uint8_t>(i);
      }

      REXKRNL_WARN(
          "[BurstSession] sessionID=AE{:02X}{:02X}{:02X}{:02X}525801 (online identity key)",
          static_cast<uint32_t>(ip[0]), static_cast<uint32_t>(ip[1]),
          static_cast<uint32_t>(ip[2]), static_cast<uint32_t>(ip[3]));

      std::memset(session_info + 0x08, 0, 0x24);

      // XNADDR.ina and XNADDR.inaOnline are already network-order bytes.
      std::memcpy(session_info + 0x08, ip, 4);
      std::memcpy(session_info + 0x0C, ip, 4);
      memory::store_and_swap<uint16_t>(session_info + 0x10, 3074);

      session_info[0x12] = 0x02;
      session_info[0x13] = 0x52;
      session_info[0x14] = ip[1];
      session_info[0x15] = ip[2];
      session_info[0x16] = ip[3];
      session_info[0x17] = 0x01;

      static uint64_t next_nonce = 0x42555253544C0001ull;
      const uint64_t nonce = next_nonce++;
      memory::store_and_swap<uint64_t>(nonce_out, nonce);

      REXKRNL_WARN(
          "[BurstSession] host XNADDR={}.{}.{}.{}:{} nonce={:016X}",
          ip[0], ip[1], ip[2], ip[3], 3074, nonce);

      return X_E_SUCCESS;
    }
    case 0x000B0011: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XGISessionDelete({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return X_E_SUCCESS;
    }
    case 0x000B0012: {
      assert_true(!buffer_length || buffer_length == 20);
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t user_index_array = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t private_slots_array = memory::load_and_swap<uint32_t>(buffer + 16);
      bool is_local = xuid_array_ptr == 0;

      REXKRNL_DEBUG("{}({:08X}, {}, {}, {:08X}, {:08X})",
                    is_local ? "XGISessionJoinLocal" : "XGISessionJoinRemote", session_ptr,
                    user_count, xuid_array_ptr, user_index_array, private_slots_array);
      return X_E_SUCCESS;
    }
    case 0x000B0014: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XSessionStart({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      // send high scores?
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XSessionEnd({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return X_E_SUCCESS;
    }
    case 0x000B0016: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearch({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X})", proc_index,
                    user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0018: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t maxPublicSlots = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t maxPrivateSlots = memory::load_and_swap<uint16_t>(buffer + 12);

      REXKRNL_DEBUG("XSessionModify({:08X}, {:08X}, {:08X}, {:08X})", obj_ptr, flags,
                    maxPublicSlots, maxPrivateSlots);

      return X_E_SUCCESS;
    }
    case 0x000B001C: {
      assert_true(!buffer_length || buffer_length == 36);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 32);

      REXKRNL_DEBUG("XSessionSearchEx({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X}, {})",
                    proc_index, user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr, num_users);

      constexpr uint32_t kHeaderSize = 0x08;
      constexpr uint32_t kResultSize = 0x5C;
      constexpr uint32_t kMinimumSize = kHeaderSize + kResultSize;

      auto results = memory_->TranslateVirtual(search_results_ptr);
      const char* host_ip = std::getenv("REX_XNET_SEARCH_IP");

      if (!results || results_buffer_size < kMinimumSize) {
        REXKRNL_WARN("[BurstSearch] invalid result buffer: ptr={:08X} size={}",
                     search_results_ptr, results_buffer_size);
        return X_E_FAIL;
      }

      std::memset(results, 0, results_buffer_size);

      // Only inject a test lobby when explicitly enabled at launch.
      if (!host_ip) {
        memory::store_and_swap<uint32_t>(results + 0x00, 0);
        memory::store_and_swap<uint32_t>(results + 0x04, 0);
        return X_E_SUCCESS;
      }

      unsigned p0 = 0, p1 = 0, p2 = 0, p3 = 0;
      if (std::sscanf(host_ip, "%u.%u.%u.%u", &p0, &p1, &p2, &p3) != 4 ||
          p0 > 255 || p1 > 255 || p2 > 255 || p3 > 255) {
        REXKRNL_WARN("[BurstSearch] invalid REX_XNET_SEARCH_IP: {}", host_ip);
        return X_E_FAIL;
      }

      uint8_t ip[4] = {
          static_cast<uint8_t>(p0), static_cast<uint8_t>(p1),
          static_cast<uint8_t>(p2), static_cast<uint8_t>(p3)};

      // XSESSION_SEARCHRESULT_HEADER.
      memory::store_and_swap<uint32_t>(results + 0x00, 1);
      memory::store_and_swap<uint32_t>(results + 0x04, search_results_ptr + kHeaderSize);

      uint8_t* result = results + kHeaderSize;

      // XSESSION_INFO.sessionID: online session, deterministic from host IP.
      result[0x00] = 0xAE;
      result[0x01] = ip[0];
      result[0x02] = ip[1];
      result[0x03] = ip[2];
      result[0x04] = ip[3];
      result[0x05] = 0x52;
      result[0x06] = 0x58;
      result[0x07] = 0x01;

      // XSESSION_INFO.hostAddress / XNADDR.
      std::memcpy(result + 0x08, ip, 4);
      std::memcpy(result + 0x0C, ip, 4);
      memory::store_and_swap<uint16_t>(result + 0x10, 3074);

      result[0x12] = 0x02;
      result[0x13] = 0x52;
      result[0x14] = ip[1];
      result[0x15] = ip[2];
      result[0x16] = ip[3];
      result[0x17] = 0x01;

      // XSESSION_INFO.keyExchangeKey.
      for (uint32_t i = 0; i < 16; ++i) {
        result[0x2C + i] = static_cast<uint8_t>(i);
      }

      // XSESSION_SEARCHRESULT slot data.
      memory::store_and_swap<uint32_t>(result + 0x3C, 1); // open public
      memory::store_and_swap<uint32_t>(result + 0x40, 0); // open private
      memory::store_and_swap<uint32_t>(result + 0x44, 1); // filled public
      memory::store_and_swap<uint32_t>(result + 0x48, 0); // filled private

      REXKRNL_WARN(
          "[BurstSearch] injected one lobby: {}.{}.{}.{}:3074 result={:08X}",
          ip[0], ip[1], ip[2], ip[3], search_results_ptr + kHeaderSize);

      return X_E_SUCCESS;
    }
    case 0x000B001D: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t details_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_details_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionGetDetails({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr,
                    details_buffer_size, session_details_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B001E: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionMigrateHost({:08X}, {:08X}, {}, {}, {}, {})", obj_ptr,
                    session_info_ptr, user_index, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0019: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XSessionGetInvitationData - unimplemented({}, {:08X})", user_index,
                    session_info_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001A: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);
      uint32_t session_duration_sec = memory::load_and_swap<uint32_t>(buffer + 16);  // 300
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XSessionArbitrationRegister({:08X}, {:08X}, {:016X}, {:08X}, {:08X}, {:08X})",
                    obj_ptr, flags, session_nonce, session_duration_sec, results_buffer_size,
                    results_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001B: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByID({}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B001F: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t array_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionModifySkill({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr, array_count,
                    xuid_array_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0020: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t view_id = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XUserResetStatsView({:08X}, {})", user_index, view_id);

      return X_E_SUCCESS;
    }
    case 0x000B0021: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t title_id = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t xuids_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t specs_count = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t specs_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t results_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XUserReadStats({}, {}, {:08X}, {}, {:08X}, {}, {:08X})", title_id, xuids_count,
                    xuids_ptr, specs_count, specs_ptr, results_size, results_ptr);

      REXKRNL_WARN("[BurstLiveStats] XUserReadStats unavailable; returning logon-not-logged-on");
      return 0x80151802u;
    }
    case 0x000B0025: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionWriteStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0026: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionFlushStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0036: {
      // Called after opening xbox live arcade and clicking on xbox live v5759
      // to 5787 and called after clicking xbox live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      REXKRNL_DEBUG("XInvalidateGamerTileCache, unimplemented");
      return X_E_FAIL;
    }
    case 0x000B003D: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t AnId_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t AnId_buffer_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t block = memory::load_and_swap<uint32_t>(buffer + 12);

      REXKRNL_DEBUG("XUserGetANID({:08X}, {:08X}, {:08X}, {:08X})", user_index, AnId_buffer_size,
                    AnId_buffer_ptr, block);

      return X_E_SUCCESS;
    }
    case 0x000B0041: {
      assert_true(!buffer_length || buffer_length == 32);
      // 00000000 2789fecc 00000000 00000000 200491e0 00000000 200491f0 20049340
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      auto context = context_ptr ? memory_->TranslateVirtual(context_ptr) : nullptr;
      uint32_t context_id = context ? memory::load_and_swap<uint32_t>(context + 0) : 0;
      REXKRNL_DEBUG("XGIUserGetContext({:08X}, {:08X}, {:08X}))", user_index, context_ptr,
                    context_id);
      uint32_t value = 0;
      if (context) {
        memory::store_and_swap<uint32_t>(context + 4, value);
      }
      return X_E_FAIL;
    }
    case 0x000B0060: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByIds({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0065: {
      assert_true(!buffer_length || buffer_length == 52);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_weighted_properties = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_weighted_contexts = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 24);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 26);
      uint32_t non_weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      uint32_t non_weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 32);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 36);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 40);
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 44);
      uint32_t weighted_search = memory::load_and_swap<uint32_t>(buffer + 48);

      REXKRNL_DEBUG(
          "XSessionSearchWeighted({:08X}, {:08X}, {:08X}, {}, {}, {:08X}, {:08X}, {}, {}, {:08X}, "
          "{:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
          proc_index, user_index, num_results, num_weighted_properties, num_weighted_contexts,
          weighted_search_properties_ptr, weighted_search_contexts_ptr, num_props, num_ctx,
          non_weighted_search_properties_ptr, non_weighted_search_contexts_ptr, results_buffer_size,
          search_results_ptr, num_users, weighted_search);

      return X_E_SUCCESS;
    }
    case 0x000B0071: {
      REXKRNL_DEBUG("XGI 0x000B0071, unimplemented");
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
