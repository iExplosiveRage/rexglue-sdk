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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/logging.h>
#include <rex/net/online.h>
#include <rex/net/session.h>
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

// XSessionSearchEx in lobby mode: the lobby's rooms as XSESSION_SEARCHRESULTs.
// The game's list reads only the properties (host name 0x40008109, PUID
// 0x20008107, 0x1000001E) and contexts (3 rounds, 4 time, 5 drama, 6
// location) of each result, plus its XSESSION_INFO to join.
X_HRESULT XgiApp::FillLobbySearchResults(uint32_t results_ptr, uint32_t buffer_size,
                                         uint32_t num_results, uint32_t num_props,
                                         uint32_t props_ptr, uint32_t num_ctx, uint32_t ctx_ptr) {
  constexpr uint32_t kHeaderSize = 0x08;
  constexpr uint32_t kResultSize = 0x5C;
  constexpr uint32_t kPropertySize = 24;
  constexpr uint32_t kContextSize = 8;
  constexpr uint32_t kHostName = 0x40008109;
  constexpr uint32_t kPuid = 0x20008107;

  std::vector<std::pair<uint32_t, uint32_t>> contexts;
  if (ctx_ptr && num_ctx) {
    const uint8_t* ctx = memory_->TranslateVirtual(ctx_ptr);
    for (uint32_t i = 0; i < num_ctx && i < 32; ++i) {
      contexts.emplace_back(memory::load_and_swap<uint32_t>(ctx + i * kContextSize),
                            memory::load_and_swap<uint32_t>(ctx + i * kContextSize + 4));
    }
  }
  {
    // What the search sends (0x1000001E and the 0x10000027/28 range):
    // logged, not used to filter.
    std::string text;
    if (props_ptr && num_props) {
      const uint8_t* props = memory_->TranslateVirtual(props_ptr);
      for (uint32_t i = 0; i < num_props && i < 16; ++i) {
        const uint8_t* prop = props + i * kPropertySize;
        text += fmt::format(" {:08X}={}", memory::load_and_swap<uint32_t>(prop),
                            static_cast<int32_t>(memory::load_and_swap<uint32_t>(prop + 16)));
      }
    }
    std::string context_text;
    for (const auto& [id, value] : contexts) {
      context_text += fmt::format(" {:X}={}", id, value);
    }
    REXKRNL_INFO("[BurstSearch] lobby search: props{} contexts{}", text, context_text);
  }

  const auto rooms = rex::net::online::Search(num_results, contexts);
  uint8_t* results = memory_->TranslateVirtual(results_ptr);
  const uint32_t slots = std::min<uint32_t>(static_cast<uint32_t>(rooms.size()), num_results);
  uint32_t data_offset = (kHeaderSize + slots * kResultSize + 7) & ~7u;
  uint32_t count = 0;
  for (uint32_t i = 0; i < slots; ++i) {
    const auto& room = rooms[i];
    std::u16string name(room.host_name.begin(), room.host_name.end());
    const uint32_t name_bytes = static_cast<uint32_t>(name.size() + 1) * 2;
    const uint32_t prop_count = 2 + static_cast<uint32_t>(room.properties.size());
    const uint32_t ctx_count = static_cast<uint32_t>(room.contexts.size());
    const uint32_t need =
        prop_count * kPropertySize + ctx_count * kContextSize + ((name_bytes + 7) & ~7u);
    if (data_offset + need > buffer_size) {
      break;
    }
    uint8_t* result = results + kHeaderSize + i * kResultSize;
    std::memcpy(result + 0x00, room.xnkid, 8);
    const uint32_t ip = room.host_ip;
    const uint8_t ip_bytes[4] = {uint8_t(ip >> 24), uint8_t(ip >> 16), uint8_t(ip >> 8),
                                 uint8_t(ip)};
    std::memcpy(result + 0x08, ip_bytes, 4);  // XNADDR.ina
    std::memcpy(result + 0x0C, ip_bytes, 4);  // XNADDR.inaOnline
    memory::store_and_swap<uint16_t>(result + 0x10, 3074);
    result[0x12] = 0x02;
    result[0x13] = 0x52;
    result[0x14] = ip_bytes[1];
    result[0x15] = ip_bytes[2];
    result[0x16] = ip_bytes[3];
    result[0x17] = 0x01;
    for (uint32_t k = 0; k < 16; ++k) {
      result[0x2C + k] = static_cast<uint8_t>(k);  // XNKEY (unused by the game)
    }
    const uint32_t open = room.max_players > room.players ? room.max_players - room.players : 0;
    memory::store_and_swap<uint32_t>(result + 0x3C, open);
    memory::store_and_swap<uint32_t>(result + 0x40, 0);
    memory::store_and_swap<uint32_t>(result + 0x44, room.players);
    memory::store_and_swap<uint32_t>(result + 0x48, 0);

    const uint32_t props_offset = data_offset;
    const uint32_t ctx_offset = props_offset + prop_count * kPropertySize;
    const uint32_t name_offset = ctx_offset + ctx_count * kContextSize;
    uint8_t* prop = results + props_offset;
    // X_PROPERTY_GAMER_HOSTNAME (WSTRING, big-endian UTF-16 with the NUL).
    memory::store_and_swap<uint32_t>(prop, kHostName);
    prop[8] = 4;
    memory::store_and_swap<uint32_t>(prop + 16, name_bytes);
    memory::store_and_swap<uint32_t>(prop + 20, results_ptr + name_offset);
    prop += kPropertySize;
    // X_PROPERTY_GAMER_PUID (every install shares the XUID).
    memory::store_and_swap<uint32_t>(prop, kPuid);
    prop[8] = 2;
    memory::store_and_swap<uint64_t>(prop + 16, 0xB13EBABEBABEBABEull);
    prop += kPropertySize;
    for (const auto& [id, value] : room.properties) {
      memory::store_and_swap<uint32_t>(prop, id);
      if ((id >> 28) == 2) {
        prop[8] = 2;
        memory::store_and_swap<uint64_t>(prop + 16, static_cast<uint64_t>(value));
      } else {
        prop[8] = 1;
        memory::store_and_swap<uint32_t>(prop + 16, static_cast<uint32_t>(value));
      }
      prop += kPropertySize;
    }
    uint8_t* ctx = results + ctx_offset;
    for (const auto& [id, value] : room.contexts) {
      memory::store_and_swap<uint32_t>(ctx, id);
      memory::store_and_swap<uint32_t>(ctx + 4, value);
      ctx += kContextSize;
    }
    uint8_t* text = results + name_offset;
    for (size_t k = 0; k <= name.size(); ++k) {
      memory::store_and_swap<uint16_t>(text + k * 2, k < name.size() ? name[k] : 0);
    }
    memory::store_and_swap<uint32_t>(result + 0x4C, prop_count);
    memory::store_and_swap<uint32_t>(result + 0x50, ctx_count);
    memory::store_and_swap<uint32_t>(result + 0x54, results_ptr + props_offset);
    memory::store_and_swap<uint32_t>(result + 0x58, ctx_count ? results_ptr + ctx_offset : 0);
    data_offset += need;
    ++count;
  }
  memory::store_and_swap<uint32_t>(results + 0x00, count);
  memory::store_and_swap<uint32_t>(results + 0x04, count ? results_ptr + kHeaderSize : 0);
  REXKRNL_WARN("[BurstSearch] lobby: {} session(s) written to the game's list", count);
  return X_E_SUCCESS;
}

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
      rex::net::online::OnUserSetContext(user_index, context_id, context_value);
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t property_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t value_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t value_ptr = memory::load_and_swap<uint32_t>(buffer + 24);
      REXKRNL_DEBUG("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})", user_index, property_id,
                    value_size, value_ptr);
      if (value_ptr && value_size && value_size <= 64) {
        rex::net::online::OnUserSetProperty(user_index, property_id,
                                            memory_->TranslateVirtual(value_ptr), value_size);
      }
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
      rex::net::SetGameSessionOpen(true);
      REXKRNL_INFO("[BurstSession] session open ({})", (flags & 0x01u) ? "host" : "join");

      if (rex::net::online::IsLobbyMode()) {
        // Lobby mode: the host's XSESSION_INFO carries its virtual address
        // and a fresh XNKID, and the room goes to the lobby; a joiner keeps
        // the search result's info and starts the lobby join + ICE. Both
        // complete at once: the game re-creates every tick while a create is
        // pending, and waits for the host's reply afterwards instead.
        if (!session_info_ptr) {
          return X_E_FAIL;
        }
        uint8_t* session_info = memory_->TranslateVirtual(session_info_ptr);
        if (flags & 0x01u) {
          if (!nonce_ptr) {
            return X_E_FAIL;
          }
          rex::net::online::OnHostCreate(session_info, flags, num_slots_public,
                                         num_slots_private);
          static uint64_t next_lobby_nonce = 0x42555253544C1001ull;
          memory::store_and_swap<uint64_t>(memory_->TranslateVirtual(nonce_ptr),
                                           next_lobby_nonce++);
        } else {
          rex::net::online::OnJoinCreate(session_info);
        }
        return X_E_SUCCESS;
      }

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
      rex::net::SetGameSessionOpen(false);
      rex::net::online::OnSessionDelete();
      REXKRNL_INFO("[BurstSession] session closed");

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
      if (flags & 0x01u) {
        rex::net::online::OnHostModify();
      }

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

      if (rex::net::online::IsLobbyMode()) {
        return FillLobbySearchResults(search_results_ptr, results_buffer_size, num_results,
                                      num_props, props_ptr, num_ctx, ctx_ptr);
      }

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

namespace rex::net {

namespace {
std::atomic<bool> g_game_session_open{false};
}  // namespace

bool IsGameSessionOpen() {
  return g_game_session_open.load(std::memory_order_acquire);
}

void SetGameSessionOpen(bool open) {
  g_game_session_open.store(open, std::memory_order_release);
}

}  // namespace rex::net
