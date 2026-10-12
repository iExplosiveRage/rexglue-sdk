/**
 * @file        system/net/online_stats_internal.h
 * @brief       Glue between the lobby service (online.cpp) and the identity /
 *              ranked / stats layer (online_stats.cpp).
 */
#pragma once

#include <string>

#include <inja/third_party/include/nlohmann/json.hpp>

namespace rex::net::online {

// online.cpp
/// Sends a text frame on the lobby socket (worker thread; dropped when the
/// lobby isn't connected).
void SendLobbyText(std::string text);
/// Shows a short on-screen notice (the online notice overlay).
void PostOnlineNotice(std::string text, double seconds);
/// The lobby's WebSocket URL ("" before lobby mode is decided / off).
std::string LobbyUrl();

namespace stats::internal {

/// Lobby mode is on: load the identity, register one in the background when
/// this lobby has none.
void OnLobbyMode(const std::string& ws_url);
/// The `auth` object for the lobby hello ({id, key}), null without one. Waits
/// up to `wait_ms` for a registration in progress.
nlohmann::json HelloAuth(int wait_ms);
/// Lobby messages the stats layer cares about: welcome, joined, peer_joined,
/// result_ack, battle_result, me, error.
void OnLobbyMessage(const std::string& type, const nlohmann::json& message);
/// The lobby connection closed (the match record stays until the session ends).
void OnLobbyClosed();
/// The game session ended (XSessionDelete): the match record is over.
void OnSessionEnded();

}  // namespace stats::internal
}  // namespace rex::net::online
