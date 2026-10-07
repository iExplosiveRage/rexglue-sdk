/**
 * @file        system/net/lobby_client.h
 * @brief       WebSocket connection to the online lobby (text frames only).
 *              Connects in the background and reconnects with backoff; the
 *              owner speaks the lobby protocol on top.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace rex::net::online {

class LobbyClient {
 public:
  struct Callbacks {
    /// The socket opened (true) or closed / failed to open (false, with why).
    std::function<void(bool open, const std::string& why)> on_state;
    /// One complete text message.
    std::function<void(std::string text)> on_message;
  };

  static std::unique_ptr<LobbyClient> Create();
  virtual ~LobbyClient() = default;

  /// False when this platform has no WebSocket client.
  virtual bool supported() const = 0;

  /// Starts (or resumes) connecting to `url` (ws:// or wss://). Callbacks run
  /// on the client's receive thread.
  virtual void Start(const std::string& url, Callbacks callbacks) = 0;

  /// Closes the connection and stops reconnecting until the next Start.
  virtual void Stop() = 0;

  /// Queues a text message. Messages queued while the socket is closed are
  /// dropped when it closes; the owner re-sends its state after reconnecting.
  virtual void Send(std::string text) = 0;

  /// Round trip of the last {"t":"ping"} / {"t":"pong"} exchange, 0 if none yet.
  virtual uint32_t rtt_ms() const = 0;

  /// To be called when a {"t":"pong"} arrives (the owner parses messages).
  virtual void OnPong() = 0;
};

}  // namespace rex::net::online
