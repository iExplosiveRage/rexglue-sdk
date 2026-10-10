/**
 * @file        system/net/online.cpp
 * @brief       Online lobby + ICE session bridge. See include/rex/net/online.h
 *              and C:/rex/_online_tests/transport_design.md.
 *
 * Threads: game threads call the public functions; everything that talks to
 * the lobby or owns ICE agents runs on one worker thread (event queue +
 * timers). The datagram fast path (sendto / recvfrom) goes straight to the
 * Transport.
 */

#include <rex/net/online.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <inja/third_party/include/nlohmann/json.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/net/session.h>
#include <rex/version.h>

#include "lobby_client.h"
#include "peer_transport.h"

REXCVAR_DEFINE_STRING(online_lobby_url, "", "Online",
                      "Lobby WebSocket URL (ws:// or wss://.../v1/ws). Empty = LAN / VPN play only "
                      "(REX_XNET_IP / REX_XNET_SEARCH_IP).");
REXCVAR_DEFINE_STRING(online_mode, "auto", "Online",
                      "auto: use the lobby when online_lobby_url is set and REX_XNET_IP / "
                      "REX_XNET_SEARCH_IP are not; lobby / direct force one.")
    .allowed({"auto", "lobby", "direct"});
REXCVAR_DEFINE_STRING(online_name, "", "Online",
                      "Your name for the lobby and the game (1-15 letters). Empty = the Windows "
                      "user name.");
REXCVAR_DEFINE_STRING(online_version, "", "Online",
                      "Test override of the version string rooms must match (empty = the build's).");
REXCVAR_DEFINE_INT32(online_redundancy, 2, "Online",
                     "Extra copies of every online datagram, sent a few ms after it (0 = off).");
REXCVAR_DEFINE_STRING(online_redundancy_delays_ms, "5,12", "Online",
                      "Delay of each extra copy after the datagram, in ms.");
REXCVAR_DEFINE_BOOL(online_ice_relay_only, false, "Online",
                    "Test: connect only through a TURN relay.");
REXCVAR_DEFINE_STRING(online_ice_bind_ip, "", "Online",
                      "Local address for the ICE socket (empty = all).");
REXCVAR_DEFINE_STRING(online_ice_port_range, "", "Online",
                      "Local UDP port range for the ICE socket, e.g. 50000-50100 (empty = any).");
REXCVAR_DEFINE_INT32(online_join_timeout_ms, 9000, "Online",
                     "How long joining waits for the connection to the host (the game gives up "
                     "after 10 s).");
REXCVAR_DEFINE_INT32(online_search_wait_ms, 1500, "Online",
                     "How long a session search waits for the lobby's room list.");
REXCVAR_DEFINE_STRING(online_stun_override, "", "Online",
                      "Test: STUN server host:port instead of the lobby's.");
REXCVAR_DEFINE_STRING(online_turn_override, "", "Online",
                      "Test: TURN server user:password@host:port instead of the lobby's.");
REXCVAR_DEFINE_STRING(online_ice_log, "warn", "Online",
                      "libjuice log level: none, error, warn, info, debug, verbose.");
REXCVAR_DEFINE_INT32(online_sim_latency_ms, 0, "Online",
                     "Test: hold every outgoing online packet this long.");
REXCVAR_DEFINE_INT32(online_sim_jitter_ms, 0, "Online",
                     "Test: plus a random 0..N ms per packet.");
REXCVAR_DEFINE_INT32(online_sim_loss_pct, 0, "Online",
                     "Test: drop this percentage of outgoing online packets.");
REXCVAR_DEFINE_INT32(online_debug_connect_delay_ms, 0, "Online",
                     "Test: report the local ICE connection this many ms late (the peer's first "
                     "frames then arrive before it, as on real networks).");
REXCVAR_DEFINE_INT32(online_debug_turn_server, 0, "Online",
                     "Test: run a TURN server on 127.0.0.1 at this port (user test, password "
                     "test). 0 = off.");

namespace rex::net::online {
namespace {

using json = nlohmann::json;

constexpr uint32_t kVirtualBase = 0xC6120000;  // 198.18.0.0/16
// 2: the game's side-channel kinds 16..31 (per-frame inputs, input delay,
// state hashes) - builds speaking 1 don't have them.
constexpr int kNetVersion = 2;
constexpr uint32_t kContextGameType = 0x800A;
constexpr uint32_t kTitleContexts[] = {3, 4, 5, 6};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string IpString(uint32_t ip) {
  return fmt::format("{}.{}.{}.{}", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

bool ParseIp(const std::string& text, uint32_t* out) {
  unsigned a, b, c, d;
  char extra;
  if (std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 ||
      b > 255 || c > 255 || d > 255) {
    return false;
  }
  *out = (a << 24) | (b << 16) | (c << 8) | d;
  return true;
}

std::string Hex(const uint8_t* data, size_t size) {
  static const char kDigits[] = "0123456789ABCDEF";
  std::string out;
  for (size_t i = 0; i < size; ++i) {
    out += kDigits[data[i] >> 4];
    out += kDigits[data[i] & 15];
  }
  return out;
}

bool FromHex(const std::string& text, uint8_t* out, size_t size) {
  if (text.size() != size * 2) {
    return false;
  }
  for (size_t i = 0; i < size; ++i) {
    unsigned value;
    if (std::sscanf(text.c_str() + i * 2, "%2x", &value) != 1) {
      return false;
    }
    out[i] = uint8_t(value);
  }
  return true;
}

std::string Base64(const uint8_t* data, size_t size) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < size; i += 3) {
    uint32_t chunk = uint32_t(data[i]) << 16;
    if (i + 1 < size) chunk |= uint32_t(data[i + 1]) << 8;
    if (i + 2 < size) chunk |= data[i + 2];
    out += kTable[(chunk >> 18) & 63];
    out += kTable[(chunk >> 12) & 63];
    out += i + 1 < size ? kTable[(chunk >> 6) & 63] : '=';
    out += i + 2 < size ? kTable[chunk & 63] : '=';
  }
  return out;
}

std::string Printable(const std::string& text, size_t max_length) {
  std::string out;
  for (char c : text) {
    if (c >= 0x20 && c <= 0x7E) {
      out += c;
    }
  }
  const size_t begin = out.find_first_not_of(' ');
  if (begin == std::string::npos) {
    return {};
  }
  out = out.substr(begin);
  if (out.size() > max_length) {
    out.resize(max_length);
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

// "stun:host:port?x", "turn:host:port?transport=udp", "host:port".
bool ParseHostPort(std::string url, std::string* host, uint16_t* port, uint16_t default_port) {
  for (const char* scheme : {"stun:", "turn:", "turns:", "stuns:"}) {
    if (url.rfind(scheme, 0) == 0) {
      url = url.substr(std::strlen(scheme));
      break;
    }
  }
  const size_t query = url.find('?');
  if (query != std::string::npos) {
    url.resize(query);
  }
  int value = default_port;
  if (!url.empty() && url[0] == '[') {
    const size_t close = url.find(']');
    if (close == std::string::npos) {
      return false;
    }
    if (close + 1 < url.size() && url[close + 1] == ':') {
      value = std::atoi(url.c_str() + close + 2);
    }
    url = url.substr(1, close - 1);
  } else {
    const size_t colon = url.rfind(':');
    if (colon != std::string::npos) {
      value = std::atoi(url.c_str() + colon + 1);
      url.resize(colon);
    }
  }
  if (url.empty() || value <= 0 || value > 65535) {
    return false;
  }
  *host = url;
  *port = uint16_t(value);
  return true;
}

std::string JsonString(const json& object, const char* key) {
  if (object.is_object()) {
    auto it = object.find(key);
    if (it != object.end() && it->is_string()) {
      return it->get<std::string>();
    }
  }
  return {};
}

int64_t JsonInt(const json& object, const char* key, int64_t fallback) {
  if (object.is_object()) {
    auto it = object.find(key);
    if (it != object.end() && it->is_number()) {
      return it->get<int64_t>();
    }
  }
  return fallback;
}

bool JsonBool(const json& object, const char* key, bool fallback) {
  if (object.is_object()) {
    auto it = object.find(key);
    if (it != object.end() && it->is_boolean()) {
      return it->get<bool>();
    }
  }
  return fallback;
}

// ------------------------------------------------------------ settings sync

// The game's online cvars both sides must share. Locked from session create
// to delete; the guest adopts the host's values first.
class SyncLock {
 public:
  void SetNames(std::vector<std::string> names) {
    std::lock_guard<std::mutex> lock(mutex_);
    names_ = std::move(names);
  }

  json Current() {
    std::vector<std::string> names;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      names = names_;
    }
    json values = json::object();
    for (const auto& name : names) {
      if (rex::cvar::GetFlagInfo(name)) {
        values[name] = rex::cvar::GetFlagByName(name);
      }
    }
    return values;
  }

  // host_values: the room's values to adopt (guest), or null (host).
  void Lock(const json& host_values) {
    std::vector<std::string> names;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      names = names_;
      for (const auto& name : names) {
        if (!registered_.count(name) && rex::cvar::GetFlagInfo(name)) {
          registered_.insert(name);
          rex::cvar::RegisterChangeCallback(
              name, [this](std::string_view changed, std::string_view value) {
                OnChanged(std::string(changed), std::string(value));
              });
        }
      }
    }
    std::vector<std::pair<std::string, std::string>> to_set;
    for (const auto& name : names) {
      if (!rex::cvar::GetFlagInfo(name)) {
        continue;
      }
      const std::string current = rex::cvar::GetFlagByName(name);
      if (host_values.is_object() && host_values.contains(name) && host_values[name].is_string()) {
        const std::string wanted = host_values[name].get<std::string>();
        if (wanted != current) {
          std::lock_guard<std::mutex> lock(mutex_);
          if (!saved_.count(name)) {
            saved_[name] = current;
          }
          to_set.emplace_back(name, wanted);
        }
      }
    }
    // SetFlagByName runs change callbacks under the cvar registry lock, so
    // never call it while holding ours.
    for (const auto& [name, value] : to_set) {
      applying_ = true;
      const bool ok = rex::cvar::SetFlagByName(name, value);
      applying_ = false;
      REXSYS_WARN("[Online] using the host's {}={} (yours: {}) for this session{}", name, value,
                  saved_value(name), ok ? "" : " - FAILED to apply");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& name : names) {
      if (rex::cvar::GetFlagInfo(name)) {
        locked_values_[name] = rex::cvar::GetFlagByName(name);
      }
    }
    locked_ = true;
  }

  void Unlock() {
    std::map<std::string, std::string> restore;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!locked_) {
        return;
      }
      locked_ = false;
      restore.swap(saved_);
      locked_values_.clear();
    }
    for (const auto& [name, value] : restore) {
      applying_ = true;
      rex::cvar::SetFlagByName(name, value);
      applying_ = false;
      REXSYS_WARN("[Online] {} back to your {} after the session", name, value);
    }
  }

  std::function<void(std::string)> notify;

 private:
  std::string saved_value(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = saved_.find(name);
    return it == saved_.end() ? std::string() : it->second;
  }

  void OnChanged(const std::string& name, const std::string& value) {
    if (applying_) {
      return;
    }
    std::string locked_value;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!locked_) {
        return;
      }
      auto it = locked_values_.find(name);
      if (it == locked_values_.end() || it->second == value) {
        return;
      }
      locked_value = it->second;
    }
    // Already inside the registry lock (recursive): put the value back.
    applying_ = true;
    rex::cvar::SetFlagByName(name, locked_value);
    applying_ = false;
    REXSYS_WARN("[Online] {} is locked during an online session (kept {})", name, locked_value);
    if (notify) {
      notify(fmt::format("{} can't change during an online session", name));
    }
  }

  std::mutex mutex_;
  std::vector<std::string> names_;
  std::set<std::string> registered_;
  std::map<std::string, std::string> saved_;
  std::map<std::string, std::string> locked_values_;
  bool locked_ = false;
  static thread_local bool applying_;
};

thread_local bool SyncLock::applying_ = false;

// ------------------------------------------------------------------ service

class Service {
 public:
  static Service& Get() {
    static Service* service = new Service();  // lives as long as the process
    return *service;
  }

  // --- shared with game threads ---
  void SetGameVersion(std::string version) {
    std::lock_guard<std::mutex> lock(shared_mutex_);
    game_version_ = std::move(version);
  }
  std::string Version() {
    std::string base = REXCVAR_GET(online_version);
    if (base.empty()) {
      std::lock_guard<std::mutex> lock(shared_mutex_);
      base = game_version_.empty() ? std::string("rexglue-") + REXGLUE_VERSION_STRING
                                   : game_version_;
    }
    return Printable(base, 58) + "/net" + std::to_string(kNetVersion);
  }

  SyncLock& sync() { return sync_; }

  void PostNotice(std::string text, double seconds = 6.0) {
    REXSYS_INFO("[Online] notice: {}", text);
    std::lock_guard<std::mutex> lock(shared_mutex_);
    notice_ = std::move(text);
    notice_ms_ = NowMs();
    notice_seconds_ = seconds;
  }
  bool GetNotice(Notice* out) {
    std::lock_guard<std::mutex> lock(shared_mutex_);
    if (notice_.empty()) {
      return false;
    }
    const double age = double(NowMs() - notice_ms_) / 1000.0;
    if (age > notice_seconds_) {
      return false;
    }
    out->text = notice_;
    out->age_seconds = age;
    out->duration_seconds = notice_seconds_;
    return true;
  }

  void DecideMode() {
    std::call_once(mode_once_, [this] {
      const std::string url = REXCVAR_GET(online_lobby_url);
      const std::string mode = REXCVAR_GET(online_mode);
      const char* xnet_ip = std::getenv("REX_XNET_IP");
      const char* xnet_search = std::getenv("REX_XNET_SEARCH_IP");
      std::string reason;
      if (mode == "direct") {
        reason = "forced";
      } else if (url.empty()) {
        reason = "no_url";
      } else if (mode == "auto" && xnet_search && *xnet_search) {
        reason = "xnet_search_ip";
      } else if (mode == "auto" && xnet_ip && *xnet_ip) {
        reason = "xnet_ip";
      } else if (!LobbyClient::Create()->supported()) {
        reason = "no_websocket_client";
      }
      if (!reason.empty()) {
        REXSYS_WARN("[Online] mode=direct reason={}", reason);
        return;
      }
      url_ = url;
      lobby_mode_.store(true);
      SetIceLogLevel(REXCVAR_GET(online_ice_log));
      if (const int port = REXCVAR_GET(online_debug_turn_server); port > 0 && port < 65536) {
        StartDebugTurnServer(uint16_t(port));
      }
      REXSYS_WARN("[Online] mode=lobby url={} vip={} name={} version={}", url_, IpString(vip_),
                  PlayerName(), Version());
      std::thread([this] { WorkerLoop(); }).detach();
      PostAt(NowMs() + 1000, [this] { Tick(); });
    });
  }
  bool lobby_mode() {
    DecideMode();
    return lobby_mode_.load();
  }
  uint32_t vip() const { return vip_; }

  void OnContext(uint32_t id, uint32_t value) {
    std::lock_guard<std::mutex> lock(shared_mutex_);
    contexts_[id] = value;
  }
  void OnProperty(uint32_t id, int64_t value) {
    std::lock_guard<std::mutex> lock(shared_mutex_);
    auto it = properties_.find(id);
    if (it == properties_.end() || it->second != value) {
      REXSYS_INFO("[Online] property {:08X} = {}", id, value);
    }
    properties_[id] = value;
  }

  void SocketOpened() {
    if (!lobby_mode()) {
      return;
    }
    Post([this] {
      ++sockets_open_;
      ++idle_token_;
      EnsureLobby();
    });
  }
  void SocketClosed() {
    if (!lobby_mode()) {
      return;
    }
    Post([this] {
      sockets_open_ = std::max(0, sockets_open_ - 1);
      if (sockets_open_ == 0) {
        const uint64_t token = ++idle_token_;
        PostAt(NowMs() + 60000, [this, token] {
          if (token == idle_token_ && sockets_open_ == 0 && !host_.active && !guest_.active &&
              lobby_) {
            REXSYS_INFO("[Lobby] idle: disconnecting");
            lobby_->Stop();
            lobby_started_ = false;
            welcomed_.store(false);
          }
        });
      }
    });
  }

  // Host XSessionCreate.
  void HostCreate(uint8_t* info, uint32_t flags, uint32_t public_slots, uint32_t private_slots) {
    std::random_device random;
    uint8_t kid[8];
    kid[0] = 0xAE;
    for (int i = 1; i < 8; ++i) {
      kid[i] = uint8_t(random());
    }
    std::memcpy(info, kid, 8);
    FillXnAddr(info + 0x08, vip_);
    for (int i = 0; i < 16; ++i) {
      info[0x2C + i] = uint8_t(i);
    }
    sync_.Lock(json());
    const bool listed = !(public_slots == 0 && private_slots > 0);
    std::string session = Base64(info, 0x3C);
    REXSYS_WARN("[Lobby] hosting: XNKID={} vip={} flags={:08X} slots={}/{}{}", Hex(kid, 8),
                IpString(vip_), flags, public_slots, private_slots,
                listed ? "" : " (private session: not listed)");
    Post([this, kid0 = std::vector<uint8_t>(kid, kid + 8), listed, session] {
      if (guest_.active) {
        FailJoin("a new session was created");
      }
      ClosePeer("new session");
      host_ = {};
      host_.active = true;
      host_.listed = listed;
      std::memcpy(host_.kid, kid0.data(), 8);
      host_.session = session;
      EnsureLobby();
      if (welcomed_.load()) {
        SendCreate();
      }
    });
  }

  void HostModify() {
    if (!lobby_mode()) {
      return;
    }
    Post([this] {
      if (!host_.active || host_.room.empty()) {
        return;
      }
      lobby_->Send(json{{"t", "update"}, {"info", BuildInfo()}}.dump());
      REXSYS_INFO("[Lobby] update sent (rules changed)");
    });
  }

  // Joiner XSessionCreate (synchronous: the game re-creates every tick while
  // a create is pending, so the hold is the game's own 10 s reply wait).
  void JoinCreate(const uint8_t* info) {
    const std::string kid = Hex(info, 8);
    KnownRoom room;
    {
      std::lock_guard<std::mutex> lock(shared_mutex_);
      auto it = known_rooms_.find(kid);
      if (it == known_rooms_.end()) {
        REXSYS_WARN("[Lobby] join: unknown session {} (not from the last search)", kid);
        return;
      }
      room = it->second;
      if (joining_kid_ == kid) {
        return;  // already joining this one
      }
      joining_kid_ = kid;
    }
    sync_.Lock(room.info.contains("sync") ? room.info["sync"] : json());
    Transport::Get().Expect(room.vip);
    Post([this, room, kid] {
      if (guest_.active && guest_.room == room.id) {
        return;
      }
      ClosePeer("new join");
      early_signals_.clear();
      guest_ = {};
      guest_.active = true;
      guest_.room = room.id;
      guest_.host_name = room.host_name;
      guest_.host_vip = room.vip;
      guest_.kid = kid;
      guest_.started_ms = NowMs();
      guest_.attempt = ++attempts_;
      const int timeout = std::max(1000, REXCVAR_GET(online_join_timeout_ms));
      const uint64_t attempt = guest_.attempt;
      PostAt(NowMs() + timeout, [this, attempt, timeout] {
        if (guest_.active && guest_.attempt == attempt && !guest_.done) {
          FailJoin(fmt::format("no connection after {} ms", timeout));
        }
      });
      EnsureLobby();
      if (welcomed_.load()) {
        SendJoin();
      }
    });
  }

  void SessionDelete() {
    sync_.Unlock();
    {
      std::lock_guard<std::mutex> lock(shared_mutex_);
      joining_kid_.clear();
    }
    if (!lobby_mode()) {
      return;
    }
    Post([this] {
      if (host_.active) {
        if (!host_.room.empty()) {
          lobby_->Send(R"({"t":"leave"})");
        }
        REXSYS_INFO("[Lobby] leave (host, room {})", host_.room);
        host_ = {};
        std::lock_guard<std::mutex> lock(shared_mutex_);
        own_room_.clear();
      }
      if (guest_.active) {
        if (!guest_.done) {
          Transport::Get().Release(guest_.host_vip, false);
        }
        if (guest_.join_sent) {
          lobby_->Send(R"({"t":"leave"})");
        }
        REXSYS_INFO("[Lobby] leave (guest, room {})", guest_.room);
        guest_ = {};
      }
      if (peer_) {
        for (int i = 0; i < 3; ++i) {
          uint8_t reason = 0;
          Transport::Get().SendSide(peer_, kSideBye, &reason, 1);
        }
        auto closing = peer_;
        PostAt(NowMs() + 500, [this, closing] {
          if (peer_ == closing) {
            ClosePeer("session deleted");
          }
        });
      }
    });
  }

  std::vector<SearchResult> Search(uint32_t max_results,
                                   const std::vector<std::pair<uint32_t, uint32_t>>& contexts) {
    std::vector<SearchResult> results;
    const int64_t start = NowMs();
    if (!welcomed_.load()) {
      REXSYS_WARN("[Lobby] search: lobby unreachable (not connected), 0 results");
      PostNotice("Online lobby not reachable yet. Try again in a moment.");
      Post([this] { EnsureLobby(); });
      return results;
    }
    auto promise = std::make_shared<std::promise<json>>();
    auto future = promise->get_future();
    Post([this, promise] {
      if (!welcomed_.load() || !lobby_) {
        promise->set_value(json());
        return;
      }
      if (search_promise_) {
        search_promise_->set_value(json());
      }
      search_promise_ = promise;
      lobby_->Send(R"({"t":"list"})");
    });
    const int wait = std::max(100, REXCVAR_GET(online_search_wait_ms));
    if (future.wait_for(std::chrono::milliseconds(wait)) != std::future_status::ready) {
      REXSYS_WARN("[Lobby] search: lobby unreachable (no answer in {} ms), 0 results", wait);
      PostNotice("The online lobby didn't answer. Search again.");
      return results;
    }
    const json rooms = future.get();
    if (!rooms.is_array()) {
      REXSYS_WARN("[Lobby] search: lobby unreachable (disconnected), 0 results");
      return results;
    }
    std::map<uint32_t, uint32_t> wanted(contexts.begin(), contexts.end());
    int full = 0, locked = 0, mismatch = 0, own = 0, same_vip = 0, no_info = 0;
    std::string own_room;
    {
      std::lock_guard<std::mutex> lock(shared_mutex_);
      own_room = own_room_;
    }
    std::map<std::string, KnownRoom> known;
    for (const auto& room : rooms) {
      if (!room.is_object()) {
        continue;
      }
      const int64_t players = JsonInt(room, "players", 1);
      const int64_t max = JsonInt(room, "max", 2);
      const std::string id = JsonString(room, "id");
      if (!own_room.empty() && id == own_room) {
        ++own;
        continue;
      }
      if (players >= max) {
        ++full;
        continue;
      }
      if (JsonBool(room, "locked", false)) {
        ++locked;
        continue;
      }
      const json info = room.contains("info") ? room["info"] : json();
      SearchResult result;
      uint32_t host_vip = 0;
      if (!info.is_object() || JsonInt(info, "v", 0) != 1 ||
          !FromHex(JsonString(info, "kid"), result.xnkid, 8) ||
          !ParseIp(JsonString(info, "vip"), &host_vip) || !IsVirtualIp(host_vip)) {
        ++no_info;
        continue;
      }
      if (host_vip == vip_) {
        ++same_vip;
        continue;
      }
      std::map<uint32_t, uint32_t> room_contexts;
      if (info.contains("ctx") && info["ctx"].is_object()) {
        for (const auto& [key, value] : info["ctx"].items()) {
          if (value.is_number_integer()) {
            room_contexts[uint32_t(std::strtoul(key.c_str(), nullptr, 16))] =
                value.get<uint32_t>();
          }
        }
      }
      bool matches = true;
      if (auto it = wanted.find(kContextGameType); it != wanted.end()) {
        auto have = room_contexts.find(kContextGameType);
        matches = (have == room_contexts.end() ? 1u : have->second) == it->second;
      }
      for (uint32_t context : kTitleContexts) {
        auto it = wanted.find(context);
        if (matches && it != wanted.end()) {
          auto have = room_contexts.find(context);
          matches = have != room_contexts.end() && have->second == it->second;
        }
      }
      if (!matches) {
        ++mismatch;
        continue;
      }
      if (results.size() >= max_results) {
        continue;
      }
      result.host_ip = host_vip;
      result.host_name = Printable(JsonString(room, "host"), 15);
      result.players = uint32_t(players);
      result.max_players = uint32_t(max);
      for (uint32_t context : kTitleContexts) {
        if (auto it = room_contexts.find(context); it != room_contexts.end()) {
          result.contexts.emplace_back(context, it->second);
        }
      }
      if (info.contains("prop") && info["prop"].is_object()) {
        for (const auto& [key, value] : info["prop"].items()) {
          if (value.is_number_integer()) {
            result.properties.emplace_back(uint32_t(std::strtoul(key.c_str(), nullptr, 16)),
                                           value.get<int64_t>());
          }
        }
      }
      KnownRoom entry;
      entry.id = id;
      entry.host_name = result.host_name;
      entry.vip = host_vip;
      entry.info = info;
      known[Hex(result.xnkid, 8)] = entry;
      results.push_back(std::move(result));
    }
    {
      std::lock_guard<std::mutex> lock(shared_mutex_);
      for (auto& [kid, entry] : known) {
        known_rooms_[kid] = entry;
      }
    }
    REXSYS_WARN(
        "[Lobby] search: {} rooms, {} shown (full={} locked={} mismatch={} own={} vip={} "
        "noinfo={}) in {} ms",
        rooms.size(), results.size(), full, locked, mismatch, own, same_vip, no_info,
        NowMs() - start);
    return results;
  }

  uint32_t EstimateRtt(const uint8_t* kid) {
    int64_t host_rtt = 0;
    {
      std::lock_guard<std::mutex> lock(shared_mutex_);
      auto it = known_rooms_.find(Hex(kid, 8));
      if (it == known_rooms_.end()) {
        return 0;
      }
      host_rtt = JsonInt(it->second.info, "lrtt", 0);
    }
    const uint32_t own_rtt = lobby_rtt_.load();
    if (!host_rtt && !own_rtt) {
      return 60;
    }
    return uint32_t(std::clamp<int64_t>(host_rtt + own_rtt, 5, 999));
  }

 private:
  struct KnownRoom {
    std::string id;
    std::string host_name;
    uint32_t vip = 0;
    json info;
  };

  Service() {
    std::random_device random;
    uint32_t a = 1 + random() % 254;
    uint32_t b = 1 + random() % 254;
    vip_ = kVirtualBase | (a << 8) | b;
    sync_.notify = [this](std::string text) { PostNotice(std::move(text), 4.0); };
  }

  static void FillXnAddr(uint8_t* xnaddr, uint32_t ip) {
    std::memset(xnaddr, 0, 0x24);
    const uint8_t bytes[4] = {uint8_t(ip >> 24), uint8_t(ip >> 16), uint8_t(ip >> 8),
                              uint8_t(ip)};
    std::memcpy(xnaddr + 0x00, bytes, 4);  // ina (network order)
    std::memcpy(xnaddr + 0x04, bytes, 4);  // inaOnline
    xnaddr[0x08] = 3074 >> 8;              // wPortOnline (big-endian)
    xnaddr[0x09] = 3074 & 0xFF;
    xnaddr[0x0A] = 0x02;  // abEnet: 02 52 ip1 ip2 ip3 01, as XNetGetTitleXnAddr
    xnaddr[0x0B] = 0x52;
    xnaddr[0x0C] = bytes[1];
    xnaddr[0x0D] = bytes[2];
    xnaddr[0x0E] = bytes[3];
    xnaddr[0x0F] = 0x01;
  }

  // --- worker ---
  void Post(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      tasks_.push_back(std::move(task));
    }
    worker_cv_.notify_one();
  }
  void PostAt(int64_t when_ms, std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      timers_.emplace(when_ms, std::move(task));
    }
    worker_cv_.notify_one();
  }
  void WorkerLoop() {
    std::unique_lock<std::mutex> lock(worker_mutex_);
    for (;;) {
      if (!tasks_.empty()) {
        auto task = std::move(tasks_.front());
        tasks_.pop_front();
        lock.unlock();
        task();
        lock.lock();
        continue;
      }
      if (!timers_.empty()) {
        const int64_t due = timers_.begin()->first;
        const int64_t now = NowMs();
        if (now >= due) {
          auto task = std::move(timers_.begin()->second);
          timers_.erase(timers_.begin());
          lock.unlock();
          task();
          lock.lock();
          continue;
        }
        worker_cv_.wait_for(lock, std::chrono::milliseconds(due - now));
        continue;
      }
      worker_cv_.wait(lock);
    }
  }

  // --- lobby (worker) ---
  void EnsureLobby() {
    if (!lobby_) {
      lobby_ = LobbyClient::Create();
    }
    if (lobby_started_) {
      return;
    }
    lobby_started_ = true;
    REXSYS_INFO("[Lobby] connecting {}", url_);
    LobbyClient::Callbacks callbacks;
    callbacks.on_state = [this](bool open, const std::string& why) {
      Post([this, open, why] { OnLobbyState(open, why); });
    };
    callbacks.on_message = [this](std::string text) {
      if (text == R"({"t":"pong"})") {
        lobby_->OnPong();
        lobby_rtt_.store(lobby_->rtt_ms());
        return;
      }
      Post([this, text = std::move(text)] { OnLobbyMessage(text); });
    };
    lobby_->Start(url_, std::move(callbacks));
  }

  void OnLobbyState(bool open, const std::string& why) {
    if (open) {
      lobby_failures_ = 0;
      REXSYS_INFO("[Lobby] connected, sending hello");
      lobby_->Send(json{{"t", "hello"},
                        {"game", "burstlimit"},
                        {"version", Version()},
                        {"name", PlayerName()}}
                       .dump());
      return;
    }
    const bool was_welcomed = welcomed_.exchange(false);
    host_.create_sent = false;
    host_.room.clear();
    {
      std::lock_guard<std::mutex> lock(shared_mutex_);
      own_room_.clear();
    }
    if (search_promise_) {
      search_promise_->set_value(json());
      search_promise_.reset();
    }
    if (guest_.active && !guest_.done) {
      guest_.join_sent = false;
      if (guest_.joined) {
        FailJoin("lost the lobby connection");
      }
    }
    if (!lobby_started_) {
      return;  // stopped on purpose
    }
    REXSYS_WARN("[Lobby] disconnected ({}), reconnecting", why);
    if (was_welcomed || lobby_failures_++ == 0) {
      PostNotice(peer_ && peer_connected_
                     ? "Online lobby connection lost (" + why + "), the match goes on"
                     : "Online lobby connection lost: " + why);
    }
  }

  void SendCreate() {
    if (!host_.active || host_.create_sent) {
      return;
    }
    if (peer_ && peer_connected_) {
      host_.create_deferred = true;
      REXSYS_WARN("[Lobby] create deferred: a match with {} is still connected",
                  peer_name_.empty() ? "a player" : peer_name_);
      return;
    }
    host_.create_deferred = false;
    host_.create_sent = true;
    std::string name = PlayerName();
    lobby_->Send(json{{"t", "create"},
                      {"name", name},
                      {"password", nullptr},
                      {"session", host_.session},
                      {"info", BuildInfo()},
                      {"listed", host_.listed}}
                     .dump());
    REXSYS_INFO("[Lobby] create sent name={} listed={}", name, host_.listed ? 1 : 0);
  }

  void SendJoin() {
    if (!guest_.active || guest_.join_sent || guest_.done) {
      return;
    }
    guest_.join_sent = true;
    lobby_->Send(json{{"t", "join"}, {"room", guest_.room}, {"password", nullptr}}.dump());
    REXSYS_WARN("[Lobby] join room={} host={} vip={}", guest_.room, guest_.host_name,
                IpString(guest_.host_vip));
  }

  json BuildInfo() {
    json ctx = json::object();
    json prop = json::object();
    {
      std::lock_guard<std::mutex> lock(shared_mutex_);
      for (uint32_t id : {kContextGameType, 3u, 4u, 5u, 6u}) {
        if (auto it = contexts_.find(id); it != contexts_.end()) {
          ctx[fmt::format("{:X}", id)] = it->second;
        }
      }
      for (const auto& [id, value] : properties_) {
        prop[fmt::format("{:X}", id)] = value;
      }
    }
    return json{{"v", 1},
                {"kid", Hex(host_.kid, 8)},
                {"vip", IpString(vip_)},
                {"ctx", ctx},
                {"prop", prop},
                {"sync", sync_.Current()},
                {"lrtt", lobby_rtt_.load()}};
  }

  IceServers ServersFrom(const json& welcome) {
    IceServers servers;
    if (welcome.contains("stun") && welcome["stun"].is_array()) {
      for (const auto& url : welcome["stun"]) {
        if (url.is_string() &&
            ParseHostPort(url.get<std::string>(), &servers.stun_host, &servers.stun_port, 3478)) {
          break;
        }
      }
    }
    const json turn = welcome.contains("turn") ? welcome["turn"] : json();
    if (turn.is_object() && turn.contains("urls") && turn["urls"].is_array()) {
      const std::string user = JsonString(turn, "username");
      const std::string credential = JsonString(turn, "credential");
      for (const auto& value : turn["urls"]) {
        if (!value.is_string()) {
          continue;
        }
        const std::string url = value.get<std::string>();
        // libjuice relays over UDP only.
        if (url.rfind("turn:", 0) != 0 ||
            (url.find("transport=") != std::string::npos &&
             url.find("transport=udp") == std::string::npos)) {
          continue;
        }
        IceServers::Turn entry;
        if (ParseHostPort(url, &entry.host, &entry.port, 3478) && servers.turn.size() < 2) {
          entry.username = user;
          entry.password = credential;
          servers.turn.push_back(entry);
        }
      }
    }
    if (const std::string stun = REXCVAR_GET(online_stun_override); !stun.empty()) {
      ParseHostPort(stun, &servers.stun_host, &servers.stun_port, 3478);
    }
    if (std::string turn_override = REXCVAR_GET(online_turn_override); !turn_override.empty()) {
      if (turn_override.rfind("turn:", 0) == 0) {
        turn_override = turn_override.substr(5);
      }
      const size_t at = turn_override.rfind('@');
      const size_t colon = turn_override.find(':');
      IceServers::Turn entry;
      if (at != std::string::npos && colon != std::string::npos && colon < at &&
          ParseHostPort(turn_override.substr(at + 1), &entry.host, &entry.port, 3478)) {
        entry.username = turn_override.substr(0, colon);
        entry.password = turn_override.substr(colon + 1, at - colon - 1);
        servers.turn = {entry};
      }
    }
    return servers;
  }

  // This match's ICE servers: STUN from welcome, TURN from joined /
  // peer_joined (per match since protocol rev 2; welcome.turn is null).
  IceServers MatchServers(const json& message) {
    IceServers servers = ServersFrom(message);
    if (servers.stun_host.empty()) {
      servers.stun_host = servers_.stun_host;
      servers.stun_port = servers_.stun_port;
    }
    if (servers.turn.empty()) {
      servers.turn = servers_.turn;  // an override, or an older lobby's welcome.turn
    }
    std::string list;
    for (const auto& turn : servers.turn) {
      // Not the secret: just enough to tell credentials apart in logs.
      list += fmt::format(" {}:{} (user {}..., {} B credential)", turn.host, turn.port,
                          turn.username.substr(0, 6), turn.password.size());
    }
    REXSYS_WARN("[ICE] TURN for this match:{}", list.empty() ? " none (STUN / direct only)" : list);
    return servers;
  }

  void OnLobbyMessage(const std::string& text) {
    const json message = json::parse(text, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
      REXSYS_WARN("[Lobby] unreadable message: {}", text.substr(0, 200));
      return;
    }
    const std::string type = JsonString(message, "t");
    if (type == "welcome") {
      client_id_ = JsonString(message, "id");
      servers_ = ServersFrom(message);
      welcomed_.store(true);
      REXSYS_WARN("[Lobby] welcome id={} stun={}:{} turn={}", client_id_,
                  servers_.stun_host.empty() ? "-" : servers_.stun_host, servers_.stun_port,
                  servers_.turn.size());
      lobby_->Send(R"({"t":"ping"})");  // measures the lobby round trip
      SendCreate();
      SendJoin();
    } else if (type == "rooms") {
      if (search_promise_) {
        search_promise_->set_value(message.contains("rooms") ? message["rooms"] : json());
        search_promise_.reset();
      }
    } else if (type == "created") {
      host_.room = JsonString(message, "room");
      {
        std::lock_guard<std::mutex> lock(shared_mutex_);
        own_room_ = host_.room;
      }
      REXSYS_WARN("[Lobby] room created id={} name={}", host_.room, PlayerName());
    } else if (type == "joined") {
      if (!guest_.active) {
        return;
      }
      guest_.joined = true;
      peer_client_id_ = JsonString(message, "host_id");
      REXSYS_WARN("[Lobby] joined room={} host={}", JsonString(message, "room"),
                  JsonString(message, "host_name"));
      match_servers_ = MatchServers(message);
      // Signals that came before `joined` (the host's description can beat it).
      auto early = std::move(early_signals_);
      early_signals_.clear();
      for (auto& [from, data] : early) {
        OnSignal(from, data);
      }
      const uint64_t attempt = guest_.attempt;
      PostAt(NowMs() + 5000, [this, attempt] {
        if (guest_.active && guest_.attempt == attempt && !peer_ && !guest_.done) {
          FailJoin("the host didn't answer");
        }
      });
    } else if (type == "peer_joined") {
      if (!host_.active) {
        return;
      }
      const std::string peer_name = JsonString(message, "peer_name");
      const std::string peer_id = JsonString(message, "peer_id");
      if (peer_ && peer_connected_ && !peer_left_ && peer_id != peer_client_id_) {
        // A match is on (its room was re-created after a lobby reconnect, say):
        // it isn't dropped for a newcomer, who times out ("the host didn't
        // answer") and leaves the seat.
        REXSYS_WARN("[Lobby] peer_joined {} ignored: in a match with {}", peer_name, peer_name_);
        return;
      }
      REXSYS_WARN("[Lobby] peer_joined {}", peer_name);
      ClosePeer("another guest joined");
      peer_client_id_ = JsonString(message, "peer_id");
      peer_name_ = peer_name;
      // The agent is created only now, with this match's TURN credentials,
      // and the host's description goes out after it (the guest answers it).
      match_servers_ = MatchServers(message);
      StartPeer(/*controlling=*/true);
      if (peer_) {
        SendSignal(json{{"kind", "description"},
                        {"sdp", peer_->LocalDescription()},
                        {"vip", IpString(vip_)},
                        {"net", kNetVersion}});
        peer_->Gather();
      }
    } else if (type == "peer_left") {
      if (JsonString(message, "peer_id") != peer_client_id_) {
        return;
      }
      REXSYS_WARN("[Lobby] peer_left {}", peer_name_);
      peer_left_ = true;
      auto closing = peer_;
      PostAt(NowMs() + 2000, [this, closing] {
        if (closing && peer_ == closing) {
          ClosePeer("peer left");
        }
      });
    } else if (type == "room_closed") {
      REXSYS_WARN("[Lobby] room_closed {}", JsonString(message, "room"));
      if (guest_.active && !guest_.done) {
        FailJoin("the host closed the room");
      }
    } else if (type == "signal") {
      OnSignal(JsonString(message, "from"), message.contains("data") ? message["data"] : json());
    } else if (type == "error") {
      const std::string code = JsonString(message, "code");
      const std::string re = JsonString(message, "re");
      REXSYS_WARN("[Lobby] error {} ({}) for {}", code, JsonString(message, "message"),
                  re.empty() ? "-" : re);
      if (re == "join") {
        if (code == "already_in_room") {
          lobby_->Send(R"({"t":"leave"})");
          guest_.join_sent = false;
          SendJoin();
        } else {
          FailJoin(code == "room_full"        ? "the room is full"
                   : code == "room_not_found" ? "the room is gone"
                   : code == "version_mismatch"
                       ? "the host runs another version"
                       : code);
        }
      } else if (re == "create") {
        PostNotice("The online lobby refused the room: " + code);
      } else if (re == "hello") {
        PostNotice("The online lobby refused this game: " + code);
      }
    }
  }

  void StartPeer(bool controlling) {
    PeerOptions options;
    options.controlling = controlling;
    options.servers = match_servers_;
    options.bind_ip = REXCVAR_GET(online_ice_bind_ip);
    options.relay_only = REXCVAR_GET(online_ice_relay_only);
    options.debug_connect_delay_ms = std::max(0, REXCVAR_GET(online_debug_connect_delay_ms));
    const std::string range = REXCVAR_GET(online_ice_port_range);
    unsigned begin = 0, end = 0;
    if (!range.empty() && std::sscanf(range.c_str(), "%u-%u", &begin, &end) == 2 && begin &&
        begin <= end && end < 65536) {
      options.port_begin = uint16_t(begin);
      options.port_end = uint16_t(end);
    }
    Transport::Options transport;
    transport.redundancy = std::clamp(REXCVAR_GET(online_redundancy), 0, 4);
    transport.copy_delays_ms.clear();
    for (const std::string& part : SplitComma(REXCVAR_GET(online_redundancy_delays_ms))) {
      const int value = std::atoi(part.c_str());
      if (value > 0 && value < 1000) {
        transport.copy_delays_ms.push_back(value);
      }
    }
    if (transport.copy_delays_ms.empty()) {
      transport.copy_delays_ms = {5, 12};
    }
    transport.sim_latency_ms = std::max(0, REXCVAR_GET(online_sim_latency_ms));
    transport.sim_jitter_ms = std::max(0, REXCVAR_GET(online_sim_jitter_ms));
    transport.sim_loss_pct = std::clamp(REXCVAR_GET(online_sim_loss_pct), 0, 100);
    Transport::Get().SetOptions(transport);
    REXSYS_INFO("[OnlineLink] redundancy={} delays={} sim latency={} jitter={} loss={}%",
                transport.redundancy, REXCVAR_GET(online_redundancy_delays_ms),
                transport.sim_latency_ms, transport.sim_jitter_ms, transport.sim_loss_pct);

    Peer::Events events;
    events.on_candidate = [this](Peer* peer, std::string sdp) {
      const uint64_t id = peer->id();
      Post([this, id, sdp = std::move(sdp)] {
        if (peer_ && peer_->id() == id) {
          SendSignal(json{{"kind", "candidate"}, {"sdp", sdp}});
        }
      });
    };
    events.on_gathering_done = [this](Peer* peer) {
      const uint64_t id = peer->id();
      Post([this, id] {
        if (peer_ && peer_->id() == id) {
          SendSignal(json{{"kind", "gathering_done"}});
        }
      });
    };
    events.on_state = [this](Peer* peer, int state) {
      const uint64_t id = peer->id();
      Post([this, id, state] { OnPeerState(id, state); });
    };
    events.on_hello = [this](Peer* peer, std::string body) {
      const uint64_t id = peer->id();
      Post([this, id, body = std::move(body)] { OnHello(id, body); });
    };
    events.on_bye = [this](Peer* peer) {
      const uint64_t id = peer->id();
      Post([this, id] {
        if (peer_ && peer_->id() == id && bye_peer_id_ != id) {
          bye_peer_id_ = id;
          REXSYS_WARN("[OnlineLink] bye from {}", IpString(peer_->vip()));
          auto closing = peer_;
          PostAt(NowMs() + 1000, [this, closing] {
            if (peer_ == closing) {
              ClosePeer("bye");
            }
          });
        }
      });
    };
    if (!options.servers.turn.empty() && !options.port_begin) {
      // Pick a local port the TURN server answers (see PickTurnFriendlyPort).
      const auto& turn = options.servers.turn.front();
      int answered = 0;
      const uint16_t port = PickTurnFriendlyPort(turn.host, turn.port, 400, 6, &answered);
      if (port) {
        options.port_begin = options.port_end = port;
        REXSYS_WARN("[ICE] TURN {}:{} answered {}/6 probe ports; using local port {}", turn.host,
                    turn.port, answered, port);
      } else {
        REXSYS_WARN("[ICE] TURN {}:{} answered none of 6 probe ports; relay may be unavailable",
                    turn.host, turn.port);
      }
    }
    auto peer = std::make_shared<Peer>(next_peer_id_++, options, std::move(events));
    if (!peer->Create()) {
      PostNotice("Online: couldn't start the connection (ICE)");
      return;
    }
    peer_ = std::move(peer);
    peer_started_ms_ = NowMs();
    peer_connected_ = false;
    hello_ok_ = false;
    remote_hello_ = false;
    hellos_sent_ = 0;
    host_notice_shown_ = false;
  }

  static std::vector<std::string> SplitComma(const std::string& text) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= text.size()) {
      const size_t comma = text.find(',', pos);
      parts.push_back(text.substr(pos, comma == std::string::npos ? std::string::npos
                                                                   : comma - pos));
      if (comma == std::string::npos) {
        break;
      }
      pos = comma + 1;
    }
    return parts;
  }

  void SendSignal(const json& data) {
    if (!lobby_ || peer_client_id_.empty()) {
      return;
    }
    lobby_->Send(json{{"t", "signal"}, {"to", peer_client_id_}, {"data", data}}.dump());
  }

  void OnSignal(const std::string& from, const json& data) {
    if (guest_.active && !guest_.joined && !guest_.done && !from.empty()) {
      if (early_signals_.size() < 64) {
        early_signals_.emplace_back(from, data);
      }
      return;
    }
    if (from.empty() || from != peer_client_id_ || !data.is_object()) {
      return;
    }
    const std::string kind = JsonString(data, "kind");
    if (kind == "description") {
      const std::string sdp = JsonString(data, "sdp");
      uint32_t remote_vip = 0;
      const bool has_vip = ParseIp(JsonString(data, "vip"), &remote_vip);
      if (JsonInt(data, "net", 0) != kNetVersion) {
        REXSYS_WARN("[OnlineLink] the peer speaks net {} (we speak {})", JsonInt(data, "net", 0),
                    kNetVersion);
      }
      if (host_.active) {
        if (!peer_) {
          return;
        }
        if (!has_vip || !IsVirtualIp(remote_vip) || remote_vip == vip_) {
          REXSYS_WARN("[OnlineLink] guest description without a usable virtual address ({})",
                      JsonString(data, "vip"));
          ClosePeer("bad guest address");
          return;
        }
        peer_->set_vip(remote_vip);
        Transport::Get().Register(remote_vip, peer_);
        if (!peer_->SetRemoteDescription(sdp)) {
          REXSYS_WARN("[ICE] the guest's description was rejected");
        }
        REXSYS_INFO("[OnlineLink] guest {} is {}", peer_name_, IpString(remote_vip));
      } else if (guest_.active && !guest_.done) {
        if (has_vip && remote_vip != guest_.host_vip) {
          REXSYS_WARN("[OnlineLink] host description vip {} differs from the room's {}",
                      IpString(remote_vip), IpString(guest_.host_vip));
        }
        if (!peer_) {
          StartPeer(/*controlling=*/false);
          if (!peer_) {
            FailJoin("couldn't start ICE");
            return;
          }
          peer_->set_vip(guest_.host_vip);
          Transport::Get().Register(guest_.host_vip, peer_);
        }
        if (!peer_->SetRemoteDescription(sdp)) {
          REXSYS_WARN("[ICE] the host's description was rejected");
        }
        SendSignal(json{{"kind", "description"},
                        {"sdp", peer_->LocalDescription()},
                        {"vip", IpString(vip_)},
                        {"net", kNetVersion}});
        peer_->Gather();
      }
    } else if (kind == "candidate") {
      if (peer_) {
        peer_->AddRemoteCandidate(JsonString(data, "sdp"));
      }
    } else if (kind == "gathering_done") {
      if (peer_) {
        peer_->SetRemoteGatheringDone();
      }
    }
  }

  void OnPeerState(uint64_t id, int state) {
    if (!peer_ || peer_->id() != id) {
      return;
    }
    static const char* kNames[] = {"disconnected", "gathering", "connecting",
                                   "connected",    "completed", "failed"};
    REXSYS_INFO("[ICE] state {}", state >= 0 && state < 6 ? kNames[state] : "?");
    if ((state == 3 || state == 4) && !peer_connected_) {
      peer_connected_ = true;
      REXSYS_WARN("[ICE] selected {} after {} ms", peer_->SelectedPath(),
                  NowMs() - peer_started_ms_);
      UpdateRelayPath();
      SendHello();
      // Datagrams the game sent while the link wasn't up go out now (host
      // replies, or the guest's join once the hello also checked out).
      if (!(guest_.active && !guest_.done)) {
        const size_t sent = Transport::Get().Release(peer_->vip(), true);
        if (sent) {
          REXSYS_WARN("[OnlineLink] sent {} datagram(s) held until the link was up", sent);
        }
      }
      TryCompleteJoin();
    } else if (state == 4) {
      UpdateRelayPath();  // the nominated pair can differ from the first one
    } else if (state == 5) {
      if (guest_.active && !guest_.done) {
        FailJoin("no network path to the host (ICE failed)");
      } else {
        PostNotice("Online: the connection to the other player failed");
        ClosePeer("ICE failed");
      }
    }
  }

  void UpdateRelayPath() {
    const std::string path = peer_->SelectedPath();
    const std::string types = path.substr(0, path.find(' '));  // "local/remote"
    const bool relay = types.find("relay") != std::string::npos;
    if (relay != peer_->relay_path() || !relay_logged_) {
      relay_logged_ = true;
      peer_->set_relay_path(relay);
      const int configured = std::clamp(REXCVAR_GET(online_redundancy), 0, 4);
      REXSYS_WARN("[OnlineLink] path {} ({}): {} duplicate(s) per datagram", types,
                  relay ? "TURN relay" : "direct", relay ? std::min(configured, 1) : configured);
    }
  }

  void SendHello() {
    if (!peer_ || !peer_connected_) {
      return;
    }
    const std::string body = json{{"net", kNetVersion},
                                  {"vip", IpString(vip_)},
                                  {"build", Version()},
                                  {"sync", sync_.Current()}}
                                 .dump();
    Transport::Get().SendSide(peer_, kSideHello, reinterpret_cast<const uint8_t*>(body.data()),
                              body.size());
    ++hellos_sent_;
    if (!remote_hello_ && hellos_sent_ < 40) {
      const uint64_t id = peer_->id();
      PostAt(NowMs() + 250, [this, id] {
        if (peer_ && peer_->id() == id && !remote_hello_) {
          SendHello();
        }
      });
    }
  }

  void OnHello(uint64_t id, const std::string& body) {
    if (!peer_ || peer_->id() != id) {
      return;
    }
    const json hello = json::parse(body, nullptr, false);
    if (host_.active && peer_->vip() == 0) {
      // The guest's hello can beat its description through the lobby.
      uint32_t remote_vip = 0;
      if (ParseIp(JsonString(hello, "vip"), &remote_vip) && IsVirtualIp(remote_vip) &&
          remote_vip != vip_) {
        peer_->set_vip(remote_vip);
        Transport::Get().Register(remote_vip, peer_);
      }
    }
    const bool first = !remote_hello_;
    remote_hello_ = true;
    if (first) {
      // Make sure the peer gets ours even if its earlier ones were lost.
      SendHello();
    }
    if (hello_ok_) {
      return;
    }
    std::string problem;
    if (!hello.is_object() || JsonInt(hello, "net", 0) != kNetVersion) {
      problem = "different online protocol";
    } else {
      const json mine = sync_.Current();
      const json theirs = hello.contains("sync") ? hello["sync"] : json::object();
      for (const auto& [name, value] : mine.items()) {
        if (!theirs.contains(name) || theirs[name] != value) {
          problem = fmt::format("{} differs (yours {}, theirs {})", name, value.dump(),
                                theirs.contains(name) ? theirs[name].dump() : "missing");
          break;
        }
      }
    }
    if (!problem.empty()) {
      REXSYS_WARN("[OnlineLink] hello mismatch: {}", problem);
      if (guest_.active && !guest_.done) {
        FailJoin("settings differ: " + problem);
      } else {
        PostNotice("Online: the other player's settings differ (" + problem + ")");
        ClosePeer("hello mismatch");
      }
      return;
    }
    hello_ok_ = true;
    REXSYS_WARN("[OnlineLink] hello ok peer={} build={} sync={}{}", IpString(peer_->vip()),
                JsonString(hello, "build"), hello.contains("sync") ? hello["sync"].dump() : "{}",
                peer_connected_ ? "" : " (local ICE not connected yet: waiting)");
    TryCompleteJoin();
  }

  // The guest's join is ready when BOTH the local ICE agent reports connected
  // (the peer's frames can arrive before that) and the hello checked out.
  void TryCompleteJoin() {
    if (!peer_ || !peer_connected_ || !hello_ok_) {
      return;
    }
    if (guest_.active && !guest_.done) {
      guest_.done = true;
      const size_t queued = Transport::Get().Release(guest_.host_vip, true);
      const std::string path = peer_->SelectedPath();
      REXSYS_WARN("[Session] join ready via {} after {} ms (queued={})",
                  path.empty() ? "?" : path, NowMs() - guest_.started_ms, queued);
      PostNotice(fmt::format("Connected to {} ({})", guest_.host_name,
                             path.rfind("relay", 0) == 0 || path.find("/relay") != std::string::npos
                                 ? "relay"
                                 : "direct"),
                 3.0);
    } else if (host_.active && !host_notice_shown_) {
      host_notice_shown_ = true;
      PostNotice(fmt::format("{} connected", peer_name_.empty() ? "A player" : peer_name_), 3.0);
    }
  }

  void FailJoin(const std::string& reason) {
    if (!guest_.active || guest_.done) {
      return;
    }
    guest_.done = true;
    guest_.failed = true;
    Transport::Get().Release(guest_.host_vip, false);
    if (guest_.join_sent && lobby_) {
      lobby_->Send(R"({"t":"leave"})");
    }
    REXSYS_WARN("[Session] join failed: {} after {} ms", reason, NowMs() - guest_.started_ms);
    PostNotice(fmt::format("Couldn't connect to {}: {}",
                           guest_.host_name.empty() ? "the host" : guest_.host_name, reason),
               8.0);
    ClosePeer("join failed");
  }

  void ClosePeer(const std::string& reason) {
    if (!peer_) {
      return;
    }
    REXSYS_WARN("[OnlineLink] closed peer={} ({})", IpString(peer_->vip()), reason);
    LogStats(true);
    const double minutes = std::max(1.0, double(NowMs() - peer_started_ms_)) / 60000.0;
    REXSYS_WARN(
        "[OnlineLink] totals peer={} path={}: sent {} B, received {} B in {:.1f} min "
        "({:.0f} KB/min out, {:.0f} KB/min in)",
        IpString(peer_->vip()), peer_->relay_path() ? "relay" : "direct",
        peer_->total_bytes_out(), peer_->total_bytes_in(), minutes,
        double(peer_->total_bytes_out()) / 1024.0 / minutes,
        double(peer_->total_bytes_in()) / 1024.0 / minutes);
    relay_logged_ = false;
    Transport::Get().Unregister(peer_.get());
    peer_.reset();
    peer_connected_ = false;
    hello_ok_ = false;
    remote_hello_ = false;
    peer_left_ = false;
    if (host_.active) {
      peer_client_id_.clear();
      if (host_.create_deferred) {
        // Posted: a new session (host_ reset) right after this has its own create.
        Post([this] {
          if (host_.active && host_.create_deferred && welcomed_.load()) {
            SendCreate();
          }
        });
      }
    }
  }

  void LogStats(bool final_line) {
    if (!peer_ || !peer_connected_) {
      return;
    }
    const Peer::Stats stats = peer_->TakeStats();
    const bool traffic = stats.out_frames || stats.in_unique || stats.dup;
    if (!traffic && !final_line && (++quiet_ticks_ % 10) != 0) {
      return;
    }
    REXSYS_WARN(
        "[OnlineLink] 1000ms peer={} out={} copies={} in={} dup={} old={} miss={} rtt={}ms "
        "timer_late={:.1f}ms simdrop={} senderr={} unknown={} bytes_out={} bytes_in={}{}",
        IpString(peer_->vip()), stats.out_frames, stats.copies, stats.in_unique, stats.dup,
        stats.old, stats.missing, stats.rtt_ms, stats.timer_late_ms, stats.sim_dropped,
        stats.send_errors, Transport::Get().dropped_unknown(), stats.bytes_out, stats.bytes_in,
        peer_->relay_path() ? " relay" : "");
  }

  void Tick() {
    PostAt(NowMs() + 1000, [this] { Tick(); });
    if (peer_ && peer_connected_) {
      uint8_t ping[8];
      const uint32_t now = static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count() /
          1000);
      const uint32_t id = ++ping_id_;
      for (int i = 0; i < 4; ++i) {
        ping[i] = uint8_t(id >> (24 - 8 * i));
        ping[4 + i] = uint8_t(now >> (24 - 8 * i));
      }
      Transport::Get().SendSide(peer_, kSidePing, ping, sizeof(ping));
      LogStats(false);
    }
  }

  // Shared with game threads.
  std::mutex shared_mutex_;
  std::string game_version_;
  std::map<uint32_t, uint32_t> contexts_;
  std::map<uint32_t, int64_t> properties_;
  std::map<std::string, KnownRoom> known_rooms_;
  std::string own_room_;
  std::string joining_kid_;
  std::string notice_;
  int64_t notice_ms_ = 0;
  double notice_seconds_ = 0;
  SyncLock sync_;

  std::once_flag mode_once_;
  std::atomic<bool> lobby_mode_{false};
  std::atomic<bool> welcomed_{false};
  std::atomic<uint32_t> lobby_rtt_{0};
  uint32_t vip_ = 0;
  std::string url_;

  // Worker.
  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  std::deque<std::function<void()>> tasks_;
  std::multimap<int64_t, std::function<void()>> timers_;

  // Worker-only state.
  std::unique_ptr<LobbyClient> lobby_;
  bool lobby_started_ = false;
  int lobby_failures_ = 0;
  std::string client_id_;
  IceServers servers_;
  IceServers match_servers_;
  std::vector<std::pair<std::string, json>> early_signals_;
  int sockets_open_ = 0;
  uint64_t idle_token_ = 0;
  std::shared_ptr<std::promise<json>> search_promise_;

  struct Hosting {
    bool active = false;
    bool listed = true;
    bool create_sent = false;
    // The lobby reconnected during a match: the room is re-created only once
    // that match's peer is gone, so nobody joins (and replaces it) meanwhile.
    bool create_deferred = false;
    uint8_t kid[8] = {};
    std::string session;
    std::string room;
  } host_;

  struct Joining {
    bool active = false;
    bool join_sent = false;
    bool joined = false;
    bool done = false;
    bool failed = false;
    std::string room;
    std::string host_name;
    std::string kid;
    uint32_t host_vip = 0;
    int64_t started_ms = 0;
    uint64_t attempt = 0;
  } guest_;
  uint64_t attempts_ = 0;

  std::shared_ptr<Peer> peer_;
  std::string peer_client_id_;
  std::string peer_name_;
  uint64_t next_peer_id_ = 1;
  uint64_t bye_peer_id_ = 0;
  int64_t peer_started_ms_ = 0;
  bool peer_connected_ = false;
  bool hello_ok_ = false;
  bool peer_left_ = false;  // the lobby said the match's guest left the room
  bool remote_hello_ = false;
  int hellos_sent_ = 0;
  bool relay_logged_ = false;
  bool host_notice_shown_ = false;
  uint32_t ping_id_ = 0;
  uint64_t quiet_ticks_ = 0;
};

}  // namespace

// ------------------------------------------------------------- public API

void SetGameVersion(std::string_view version) {
  Service::Get().SetGameVersion(std::string(version));
}

std::string LobbyVersion() {
  return Service::Get().Version();
}

void SetSyncedCvars(std::vector<std::string> names) {
  Service::Get().sync().SetNames(std::move(names));
}

std::string PlayerName() {
  std::string name = Printable(REXCVAR_GET(online_name), 15);
  if (name.empty()) {
    for (const char* variable : {"USERNAME", "USER"}) {
      if (const char* value = std::getenv(variable); value && *value) {
        name = Printable(value, 15);
        if (!name.empty()) {
          break;
        }
      }
    }
  }
  return name.empty() ? "Player" : name;
}

bool GetNotice(Notice* out) {
  return Service::Get().GetNotice(out);
}

bool IsLobbyMode() {
  return Service::Get().lobby_mode();
}

uint32_t VirtualIp() {
  return Service::Get().vip();
}

bool IsVirtualIp(uint32_t ip) {
  return (ip & 0xFFFF0000u) == kVirtualBase;
}

void OnGameSocketOpened() {
  Service::Get().SocketOpened();
}

void OnGameSocketClosed() {
  Service::Get().SocketClosed();
}

bool SendTo(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t* data,
            size_t size) {
  if (!IsLobbyMode()) {
    return false;
  }
  const uint32_t own = Service::Get().vip();
  if (dst_ip == 0x7F000001u || dst_ip == own) {
    // The host sends itself a "player left" notice at 127.0.0.1 (the native
    // socket isn't on port 1000 in lobby mode, so loop it back here).
    Transport::Get().LoopBack(dst_ip, src_port, dst_port, data, size);
    return true;
  }
  if (!IsVirtualIp(dst_ip)) {
    return false;
  }
  return Transport::Get().Send(src_port, dst_ip, dst_port, data, size);
}

bool RecvFrom(uint16_t dst_port, Datagram* out) {
  if (!IsLobbyMode()) {
    return false;
  }
  return Transport::Get().Recv(dst_port, out);
}

void OnUserSetContext(uint32_t user_index, uint32_t context_id, uint32_t value) {
  (void)user_index;
  Service::Get().OnContext(context_id, value);
}

void OnUserSetProperty(uint32_t user_index, uint32_t property_id, const uint8_t* value_be,
                       uint32_t size) {
  (void)user_index;
  if (!value_be) {
    return;
  }
  const uint32_t type = property_id >> 28;
  int64_t value = 0;
  if (type == 1 && size >= 4) {  // int32
    value = int32_t((uint32_t(value_be[0]) << 24) | (uint32_t(value_be[1]) << 16) |
                    (uint32_t(value_be[2]) << 8) | value_be[3]);
  } else if (type == 2 && size >= 8) {  // int64
    for (int i = 0; i < 8; ++i) {
      value = (value << 8) | value_be[i];
    }
  } else {
    return;
  }
  Service::Get().OnProperty(property_id, value);
}

void OnHostCreate(uint8_t* session_info, uint32_t flags, uint32_t public_slots,
                  uint32_t private_slots) {
  Service::Get().HostCreate(session_info, flags, public_slots, private_slots);
}

void OnHostModify() {
  Service::Get().HostModify();
}

void OnJoinCreate(const uint8_t* session_info) {
  Service::Get().JoinCreate(session_info);
}

void OnSessionDelete() {
  Service::Get().SessionDelete();
}

std::vector<SearchResult> Search(uint32_t max_results,
                                 const std::vector<std::pair<uint32_t, uint32_t>>& contexts) {
  if (!IsLobbyMode()) {
    return {};
  }
  return Service::Get().Search(max_results, contexts);
}

uint32_t EstimateRttMs(const uint8_t* xnkid) {
  return Service::Get().EstimateRtt(xnkid);
}

bool SendGameSide(uint8_t kind, const uint8_t* body, size_t size, int copies) {
  if (!IsLobbyMode()) {
    return false;
  }
  return Transport::Get().SendGameSide(kind, body, size, copies);
}

void SetGameSideHandler(std::function<void(uint8_t kind, const uint8_t* body, size_t size)> handler) {
  Transport::Get().SetGameSideHandler(std::move(handler));
}

uint32_t PeerRttMs() {
  if (!IsLobbyMode()) {
    return 0;
  }
  auto peer = Transport::Get().FirstConnected();
  return peer ? peer->rtt_ms() : 0;
}

}  // namespace rex::net::online
