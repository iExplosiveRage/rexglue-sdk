/**
 * @file        net/online.h
 * @brief       Online play without a VPN: the lobby (WebSocket) and the
 *              ICE transport (libjuice) behind the game's XNet / XSession
 *              calls.
 *
 * With `online_lobby_url` set, the host's XSessionCreate registers a room on
 * the lobby, XSessionSearchEx returns the lobby's rooms, and joining one
 * connects the two PCs with ICE (STUN hole punching, TURN relay). Every PC has
 * a virtual IPv4 (198.18.x.y) that the game sees as its own and its peer's
 * address; datagrams the game sends to a virtual address go through the ICE
 * link, framed as in burstlimit_online_service/PROTOCOL.md.
 *
 * Without a lobby URL (or with REX_XNET_IP / REX_XNET_SEARCH_IP set, the
 * Radmin scripts) nothing here is active and the XNet layer works exactly as
 * before (plain UDP to real addresses).
 *
 * Design: C:/rex/_online_tests/transport_design.md
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rex::net::online {

// --- App configuration (call at startup) --------------------------------------

/// The compatibility string sent to the lobby (rooms are only listed between
/// identical strings), e.g. the release tag + git hash. The cvar
/// online_version overrides it.
void SetGameVersion(std::string_view version);

/// Game cvars both players must share (for example the frame driver step).
/// The guest adopts the host's values for the session, and both sides lock
/// them until the session is deleted.
void SetSyncedCvars(std::vector<std::string> names);

/// The player's name: the cvar online_name, else the OS user name (printable
/// ASCII, at most 15 characters), else "Player". Used as the gamertag and the
/// lobby name.
std::string PlayerName();

// --- On-screen notices ----------------------------------------------------------

struct Notice {
  std::string text;
  double age_seconds = 0.0;
  double duration_seconds = 0.0;
};

/// The latest notice (lobby errors, failed joins, locked settings) while it
/// should still be shown. Thread-safe; meant for an overlay drawn every frame.
bool GetNotice(Notice* out);

// --- Mode -----------------------------------------------------------------------

/// True when the lobby + ICE transport is used. Decided once, on the first
/// call (the game's first VDP socket).
bool IsLobbyMode();

/// This PC's virtual address (host byte order, 198.18.A.B).
uint32_t VirtualIp();

/// Whether an address (host byte order) is in the virtual range.
bool IsVirtualIp(uint32_t ip);

// --- Socket layer (xam_net.cpp / xsocket.cpp) -----------------------------------

void OnGameSocketOpened();
void OnGameSocketClosed();

/// sendto in lobby mode. True when the datagram was taken (virtual or loopback
/// destination); the caller then reports the full length as sent. Ports and
/// addresses are as the game sees them (host byte order values).
bool SendTo(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t* data,
            size_t size);

struct Datagram {
  uint32_t from_ip = 0;
  uint16_t from_port = 0;
  std::vector<uint8_t> data;
};

/// Next datagram received for a guest port, oldest first. False when none.
bool RecvFrom(uint16_t dst_port, Datagram* out);

// --- Session layer (xgi_app.cpp) ------------------------------------------------

void OnUserSetContext(uint32_t user_index, uint32_t context_id, uint32_t value);
void OnUserSetProperty(uint32_t user_index, uint32_t property_id, const uint8_t* value_be,
                       uint32_t size);

/// Host XSessionCreate in lobby mode: fills the 0x3C-byte XSESSION_INFO (guest
/// layout, big-endian) and registers the room.
void OnHostCreate(uint8_t* session_info, uint32_t flags, uint32_t public_slots,
                  uint32_t private_slots);
/// Host XSessionModify: the rules changed.
void OnHostModify();
/// Joiner XSessionCreate in lobby mode, with the XSESSION_INFO of the chosen
/// search result.
void OnJoinCreate(const uint8_t* session_info);
/// XSessionDelete (either side).
void OnSessionDelete();

struct SearchResult {
  uint8_t xnkid[8] = {};
  uint32_t host_ip = 0;  // virtual, host byte order
  std::string host_name;
  uint32_t players = 1;
  uint32_t max_players = 2;
  std::vector<std::pair<uint32_t, uint32_t>> contexts;
  std::vector<std::pair<uint32_t, int64_t>> properties;  // numeric
};

/// XSessionSearchEx in lobby mode: asks the lobby for its rooms (bounded wait,
/// online_search_wait_ms) and returns those matching the search contexts.
std::vector<SearchResult> Search(uint32_t max_results,
                                 const std::vector<std::pair<uint32_t, uint32_t>>& contexts);

/// QoS round trip estimate for a search result (ms), 0 when unknown.
uint32_t EstimateRttMs(const uint8_t* xnkid);

// --- Game side channel (lobby mode) ---------------------------------------------
//
// Side-channel kinds 16..31 belong to the game: frames of its own (for example
// per-frame inputs) that travel next to the game's datagrams on the peer link,
// without going through the guest's sockets.

constexpr uint8_t kGameSideKindFirst = 16;
constexpr uint8_t kGameSideKindLast = 31;

/// Sends a game side-channel frame to the session's peer. Thread-safe. On a
/// direct path, `copies` more copies follow a few ms apart (none on a TURN
/// relay, whose bandwidth is paid). False when no peer link is up.
bool SendGameSide(uint8_t kind, const uint8_t* body, size_t size, int copies = 0);

/// Receives the game side-channel frames (kinds 16..31). Runs on the network
/// thread: keep it short. An empty function removes it.
void SetGameSideHandler(std::function<void(uint8_t kind, const uint8_t* body, size_t size)> handler);

/// Smoothed round trip time to the session's peer (ms), 0 when unknown.
uint32_t PeerRttMs();

}  // namespace rex::net::online
