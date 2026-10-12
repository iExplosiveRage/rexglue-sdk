/**
 * @file        system/net/http_client.h
 * @brief       Small blocking HTTP(S) client for the online lobby's REST
 *              endpoints (identity registration, leaderboard, player cards).
 */
#pragma once

#include <cstdint>
#include <string>

namespace rex::net::online {

struct HttpResponse {
  int status = 0;  // 0 = no answer (unreachable, timeout)
  std::string body;
};

/// One request, waiting at most `timeout_ms`. `url` is http:// or https://.
/// `body` empty = no body; otherwise sent as application/json.
HttpResponse HttpRequest(const std::string& method, const std::string& url,
                         const std::string& body, int timeout_ms);

/// The lobby's REST base (http[s]://host[:port]/v1) from its WebSocket URL
/// (ws[s]://host[:port]/v1/ws). Empty when the URL isn't of that form.
std::string LobbyRestBase(const std::string& ws_url);

}  // namespace rex::net::online
