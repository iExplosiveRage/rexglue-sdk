/**
 * @file        system/net/lobby_client_stub.cpp
 * @brief       No lobby client on this platform: lobby mode falls back to the
 *              direct (LAN / VPN) path. Linux players run the Windows build
 *              under Wine, which has the WinHTTP client.
 */

#include "lobby_client.h"

namespace rex::net::online {
namespace {

class StubLobbyClient final : public LobbyClient {
 public:
  bool supported() const override { return false; }
  void Start(const std::string&, Callbacks callbacks) override {
    if (callbacks.on_state) {
      callbacks.on_state(false, "no WebSocket client on this platform");
    }
  }
  void Stop() override {}
  void Send(std::string) override {}
  uint32_t rtt_ms() const override { return 0; }
  void OnPong() override {}
};

}  // namespace

std::unique_ptr<LobbyClient> LobbyClient::Create() {
  return std::make_unique<StubLobbyClient>();
}

}  // namespace rex::net::online
