/**
 * @file        system/net/peer_transport.cpp
 * @brief       ICE link (libjuice) + datagram transport. See peer_transport.h.
 *
 * Locking: libjuice runs its callbacks with its own registry lock held, and a
 * relayed juice_send takes that lock too. So nothing here calls into libjuice
 * while holding one of our locks, and callbacks never call into libjuice:
 * replies from a callback (pong) go through the timed-send thread.
 */

#include "peer_transport.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <juice/juice.h>

#include <rex/logging.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include <string>
#include <vector>

namespace rex::net::online {
namespace {

constexpr uint32_t kWindowBits = 2048;
constexpr size_t kMaxPendingFrames = 64;
constexpr int64_t kMaxPendingAgeMs = 10000;
constexpr size_t kMaxInboundPerPort = 512;

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint16_t LoadBe16(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void StoreBe16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

std::string CandidateType(const std::string& sdp) {
  const size_t typ = sdp.find(" typ ");
  if (typ == std::string::npos) {
    return "?";
  }
  const size_t begin = typ + 5;
  const size_t end = sdp.find(' ', begin);
  return sdp.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

void JuiceLog(juice_log_level_t level, const char* message) {
  switch (level) {
    case JUICE_LOG_LEVEL_VERBOSE:
    case JUICE_LOG_LEVEL_DEBUG:
      // online_ice_log already filters; show what it lets through.
      REXSYS_INFO("[juice] {}", message);
      break;
    case JUICE_LOG_LEVEL_INFO:
      REXSYS_INFO("[juice] {}", message);
      break;
    case JUICE_LOG_LEVEL_WARN:
      REXSYS_WARN("[juice] {}", message);
      break;
    default:
      REXSYS_ERROR("[juice] {}", message);
      break;
  }
}

void OnStateChanged(juice_agent_t*, juice_state_t state, void* user) {
  static_cast<Peer*>(user)->OnJuiceState(state);
}
void OnCandidate(juice_agent_t*, const char* sdp, void* user) {
  static_cast<Peer*>(user)->OnJuiceCandidate(sdp);
}
void OnGatheringDone(juice_agent_t*, void* user) {
  static_cast<Peer*>(user)->OnJuiceGatheringDone();
}
void OnRecv(juice_agent_t*, const char* data, size_t size, void* user) {
  static_cast<Peer*>(user)->OnWire(reinterpret_cast<const uint8_t*>(data), size);
}

}  // namespace

void SetIceLogLevel(const std::string& level) {
  juice_log_level_t value = JUICE_LOG_LEVEL_WARN;
  if (level == "none") {
    value = JUICE_LOG_LEVEL_NONE;
  } else if (level == "error") {
    value = JUICE_LOG_LEVEL_ERROR;
  } else if (level == "info") {
    value = JUICE_LOG_LEVEL_INFO;
  } else if (level == "debug") {
    value = JUICE_LOG_LEVEL_DEBUG;
  } else if (level == "verbose") {
    value = JUICE_LOG_LEVEL_VERBOSE;
  }
  juice_set_log_handler(JuiceLog);
  juice_set_log_level(value);
}

uint16_t PickTurnFriendlyPort(const std::string& host, uint16_t port, int timeout_ms, int count,
                              int* answered) {
  *answered = 0;
#if defined(_WIN32)
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) || !found) {
    WSACleanup();
    return 0;
  }
  sockaddr_in target = *reinterpret_cast<sockaddr_in*>(found->ai_addr);
  freeaddrinfo(found);
  std::vector<SOCKET> sockets;
  std::vector<uint16_t> ports;
  for (int i = 0; i < count; ++i) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
      continue;
    }
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    int length = sizeof(local);
    u_long nonblocking = 1;
    if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&local), &length) != 0 ||
        ioctlsocket(s, FIONBIO, &nonblocking) != 0) {
      closesocket(s);
      continue;
    }
    uint8_t request[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    for (int b = 8; b < 20; ++b) {
      request[b] = uint8_t(std::rand());
    }
    sendto(s, reinterpret_cast<const char*>(request), sizeof(request), 0,
           reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    sockets.push_back(s);
    ports.push_back(ntohs(local.sin_port));
  }
  uint16_t chosen = 0;
  std::vector<bool> got(sockets.size(), false);
  const int64_t deadline = NowUs() + int64_t(timeout_ms) * 1000;
  while (!sockets.empty()) {
    const int64_t left = deadline - NowUs();
    if (left <= 0) {
      break;
    }
    fd_set readable;
    FD_ZERO(&readable);
    for (size_t i = 0; i < sockets.size(); ++i) {
      if (!got[i]) {
        FD_SET(sockets[i], &readable);
      }
    }
    timeval wait = {long(left / 1000000), long(left % 1000000)};
    if (select(0, &readable, nullptr, nullptr, &wait) <= 0) {
      break;
    }
    for (size_t i = 0; i < sockets.size(); ++i) {
      if (got[i] || !FD_ISSET(sockets[i], &readable)) {
        continue;
      }
      uint8_t reply[512];
      const int n = recv(sockets[i], reinterpret_cast<char*>(reply), sizeof(reply), 0);
      if (n >= 20 && reply[0] == 0x01 && reply[1] == 0x01) {
        got[i] = true;
        ++*answered;
        if (!chosen) {
          chosen = ports[i];
        }
      }
    }
    // A short extra wait only to count the other answers for the log.
    if (chosen && NowUs() > deadline - int64_t(timeout_ms) * 500) {
      break;
    }
  }
  for (SOCKET s : sockets) {
    closesocket(s);
  }
  WSACleanup();
  return chosen;
#else
  (void)host;
  (void)port;
  (void)timeout_ms;
  (void)count;
  return 0;
#endif
}

bool StartDebugTurnServer(uint16_t port) {
  static std::mutex mutex;
  static juice_server_t* server = nullptr;
  std::lock_guard<std::mutex> lock(mutex);
  if (server) {
    return true;
  }
  static juice_server_credentials_t credentials = {"test", "test", 0};
  juice_server_config_t config = {};
  config.credentials = &credentials;
  config.credentials_count = 1;
  config.max_allocations = 16;
  config.max_peers = 16;
  config.bind_address = "127.0.0.1";
  config.external_address = "127.0.0.1";
  config.port = port;
  config.realm = "rexglue";
  server = juice_server_create(&config);
  if (!server) {
    REXSYS_WARN("[ICE] debug TURN server on 127.0.0.1:{} failed to start", port);
    return false;
  }
  REXSYS_WARN("[ICE] debug TURN server on 127.0.0.1:{} (user test, password test)",
              juice_server_get_port(server));
  return true;
}

// ---------------------------------------------------------------------- Peer

Peer::Peer(uint64_t id, PeerOptions options, Events events)
    : id_(id), options_(std::move(options)), events_(std::move(events)) {
  window_.assign(kWindowBits / 64, 0);
}

Peer::~Peer() {
  if (agent_) {
    juice_destroy(agent_);
    agent_ = nullptr;
  }
}

bool Peer::Create() {
  juice_config_t config = {};
  config.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
  if (!options_.servers.stun_host.empty() && options_.servers.stun_port) {
    config.stun_server_host = options_.servers.stun_host.c_str();
    config.stun_server_port = options_.servers.stun_port;
  }
  std::vector<juice_turn_server_t> turn;
  for (const auto& server : options_.servers.turn) {
    if (turn.size() >= 2) {
      break;  // libjuice uses at most 2 relays
    }
    juice_turn_server_t entry = {};
    entry.host = server.host.c_str();
    entry.username = server.username.c_str();
    entry.password = server.password.c_str();
    entry.port = server.port;
    turn.push_back(entry);
  }
  config.turn_servers = turn.empty() ? nullptr : turn.data();
  config.turn_servers_count = static_cast<int>(turn.size());
  config.bind_address = options_.bind_ip.empty() ? nullptr : options_.bind_ip.c_str();
  config.local_port_range_begin = options_.port_begin;
  config.local_port_range_end = options_.port_end;
  config.cb_state_changed = OnStateChanged;
  config.cb_candidate = OnCandidate;
  config.cb_gathering_done = OnGatheringDone;
  config.cb_recv = OnRecv;
  config.user_ptr = this;
  agent_ = juice_create(&config);
  if (!agent_) {
    REXSYS_WARN("[ICE] juice_create failed");
    return false;
  }
  REXSYS_INFO("[ICE] agent created role={} stun={}:{} turn={} relay_only={} bind={}",
              options_.controlling ? "controlling" : "controlled",
              options_.servers.stun_host.empty() ? "-" : options_.servers.stun_host,
              options_.servers.stun_port, turn.size(), options_.relay_only ? 1 : 0,
              options_.bind_ip.empty() ? "any" : options_.bind_ip);
  return true;
}

std::string Peer::LocalDescription() {
  char buffer[JUICE_MAX_SDP_STRING_LEN] = {};
  if (!agent_ || juice_get_local_description(agent_, buffer, sizeof(buffer)) < 0) {
    return {};
  }
  return buffer;
}

bool Peer::SetRemoteDescription(const std::string& sdp) {
  if (!agent_) {
    return false;
  }
  std::string filtered = sdp;
  if (options_.relay_only) {
    // Drop non-relay candidates a description might carry.
    std::string out;
    size_t pos = 0;
    while (pos < filtered.size()) {
      size_t end = filtered.find('\n', pos);
      std::string line =
          filtered.substr(pos, end == std::string::npos ? std::string::npos : end - pos + 1);
      if (line.rfind("a=candidate:", 0) != 0 || line.find(" typ relay") != std::string::npos) {
        out += line;
      }
      if (end == std::string::npos) {
        break;
      }
      pos = end + 1;
    }
    filtered = out;
  }
  return juice_set_remote_description(agent_, filtered.c_str()) == JUICE_ERR_SUCCESS;
}

void Peer::AddRemoteCandidate(const std::string& sdp) {
  if (!agent_) {
    return;
  }
  if (options_.relay_only && sdp.find(" typ relay") == std::string::npos) {
    REXSYS_INFO("[ICE] remote candidate skipped (relay only): {}", sdp);
    return;
  }
  REXSYS_INFO("[ICE] remote candidate {}", sdp);
  juice_add_remote_candidate(agent_, sdp.c_str());
}

void Peer::SetRemoteGatheringDone() {
  if (agent_) {
    juice_set_remote_gathering_done(agent_);
  }
}

void Peer::Gather() {
  if (agent_) {
    juice_gather_candidates(agent_);
  }
}

std::string Peer::SelectedPath() {
  char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {};
  char remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {};
  if (!agent_ || juice_get_selected_candidates(agent_, local, sizeof(local), remote,
                                               sizeof(remote)) < 0) {
    return {};
  }
  char local_address[JUICE_MAX_ADDRESS_STRING_LEN] = {};
  char remote_address[JUICE_MAX_ADDRESS_STRING_LEN] = {};
  juice_get_selected_addresses(agent_, local_address, sizeof(local_address), remote_address,
                               sizeof(remote_address));
  return CandidateType(local) + "/" + CandidateType(remote) + " (" + local_address + " -> " +
         remote_address + ")";
}

void Peer::OnJuiceState(int state) {
  const bool up = state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED;
  if (up && options_.debug_connect_delay_ms > 0 && !connected_.load()) {
    // Test switch: tell the owner (and the send path) only later, so the
    // peer's hello and frames arrive first, as on a real network.
    std::weak_ptr<Peer> weak = weak_from_this();
    const int delay = options_.debug_connect_delay_ms;
    std::thread([weak, state, delay] {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
      if (auto self = weak.lock()) {
        REXSYS_WARN("[ICE] (debug) reporting state {} {} ms late", state, delay);
        self->connected_.store(true);
        if (self->events_.on_state) {
          self->events_.on_state(self.get(), state);
        }
      }
    }).detach();
    return;
  }
  connected_.store(up);
  if (events_.on_state) {
    events_.on_state(this, state);
  }
}

void Peer::OnJuiceCandidate(const char* sdp) {
  std::string line = sdp ? sdp : "";
  if (options_.relay_only && line.find(" typ relay") == std::string::npos) {
    REXSYS_INFO("[ICE] local candidate not sent (relay only): {}", line);
    return;
  }
  REXSYS_INFO("[ICE] local candidate {}", line);
  if (events_.on_candidate) {
    events_.on_candidate(this, std::move(line));
  }
}

void Peer::OnJuiceGatheringDone() {
  REXSYS_INFO("[ICE] gathering done");
  if (events_.on_gathering_done) {
    events_.on_gathering_done(this);
  }
}

bool Peer::AcceptSeq(uint32_t seq) {
  if (seq == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(window_mutex_);
  auto bit = [](uint32_t s) {
    const uint32_t index = s % kWindowBits;
    return std::make_pair(size_t(index / 64), uint64_t(1) << (index % 64));
  };
  if (seq > highest_) {
    if (seq - highest_ >= kWindowBits) {
      std::fill(window_.begin(), window_.end(), 0);
    } else {
      for (uint32_t s = highest_ + 1; s < seq; ++s) {
        const auto [word, mask] = bit(s);
        window_[word] &= ~mask;
      }
    }
    highest_ = seq;
    const auto [word, mask] = bit(seq);
    window_[word] |= mask;
    ++unique_total_;
    in_unique_.fetch_add(1);
    return true;
  }
  if (highest_ - seq >= kWindowBits) {
    old_.fetch_add(1);
    return false;
  }
  const auto [word, mask] = bit(seq);
  if (window_[word] & mask) {
    dup_.fetch_add(1);
    return false;
  }
  window_[word] |= mask;
  ++unique_total_;
  in_unique_.fetch_add(1);
  return true;
}

void Peer::AddTimerLateness(double ms) {
  std::lock_guard<std::mutex> lock(late_mutex_);
  late_sum_ms_ += ms;
  ++late_count_;
}

Peer::Stats Peer::TakeStats() {
  Stats stats;
  stats.out_frames = out_frames_.exchange(0);
  stats.copies = copies_.exchange(0);
  stats.in_unique = in_unique_.exchange(0);
  stats.dup = dup_.exchange(0);
  stats.old = old_.exchange(0);
  stats.sim_dropped = sim_dropped_.exchange(0);
  stats.send_errors = send_errors_.exchange(0);
  stats.rtt_ms = rtt_ms_.load();
  stats.bytes_out = bytes_out_.exchange(0);
  stats.not_up = not_up_.exchange(0);
  stats.bytes_in = bytes_in_.exchange(0);
  {
    std::lock_guard<std::mutex> lock(window_mutex_);
    stats.missing = highest_ >= unique_total_ ? highest_ - unique_total_ : 0;
  }
  {
    std::lock_guard<std::mutex> lock(late_mutex_);
    stats.timer_late_ms = late_count_ ? late_sum_ms_ / double(late_count_) : 0.0;
    late_sum_ms_ = 0;
    late_count_ = 0;
  }
  return stats;
}

void Peer::OnWire(const uint8_t* data, size_t size) {
  if (size < 1) {
    return;
  }
  bytes_in_.fetch_add(size);
  total_bytes_in_.fetch_add(size);
  if (data[0] == 1) {
    if (size < 9) {
      return;
    }
    const uint16_t src_port = LoadBe16(data + 1);
    const uint16_t dst_port = LoadBe16(data + 3);
    const uint32_t seq = LoadBe32(data + 5);
    if (!AcceptSeq(seq)) {
      return;
    }
    Datagram datagram;
    datagram.from_ip = vip_.load();
    datagram.from_port = src_port;
    datagram.data.assign(data + 9, data + size);
    Transport::Get().Deliver(dst_port, std::move(datagram));
    return;
  }
  if (data[0] == 2) {
    if (size < 4) {
      return;
    }
    const uint8_t kind = data[1];
    const uint16_t length = LoadBe16(data + 2);
    if (size_t(4) + length > size) {
      return;
    }
    const uint8_t* body = data + 4;
    switch (kind) {
      case kSideHello:
        if (events_.on_hello) {
          events_.on_hello(this, std::string(reinterpret_cast<const char*>(body), length));
        }
        break;
      case kSidePing: {
        // Answer through the timed-send thread (no libjuice call from here).
        std::vector<uint8_t> frame(4 + length);
        frame[0] = 2;
        frame[1] = kSidePong;
        StoreBe16(frame.data() + 2, length);
        std::memcpy(frame.data() + 4, body, length);
        Transport::Get().SendWireDeferred(weak_from_this(), this, std::move(frame));
        break;
      }
      case kSidePong:
        if (length >= 8) {
          const uint32_t sent = LoadBe32(body + 4);
          const uint32_t now = static_cast<uint32_t>(NowUs() / 1000);
          const uint32_t rtt = now - sent;
          if (rtt < 60000) {
            const uint32_t old = rtt_ms_.load();
            rtt_ms_.store(old ? (old * 7 + rtt) / 8 : std::max<uint32_t>(rtt, 1));
          }
        }
        break;
      case kSideBye:
        if (events_.on_bye) {
          events_.on_bye(this);
        }
        break;
      default:
        if (kind >= kGameSideKindFirst && kind <= kGameSideKindLast) {
          Transport::Get().DispatchGameSide(kind, body, length);
        }
        break;
    }
    return;
  }
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true)) {
    REXSYS_WARN("[OnlineLink] unknown frame type {} ignored", data[0]);
  }
}

// ----------------------------------------------------------------- Transport

Transport& Transport::Get() {
  static Transport* transport = new Transport();  // lives as long as the process
  return *transport;
}

Transport::Transport() {
#if defined(_WIN32)
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  high_resolution_ = timer != nullptr;
  if (!timer) {
    timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }
  timer_ = timer;
  wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
#endif
  std::thread([this] { TimerLoop(); }).detach();
}

void Transport::SetOptions(const Options& options) {
  std::lock_guard<std::mutex> lock(mutex_);
  options_ = options;
}

void Transport::Register(uint32_t vip, std::shared_ptr<Peer> peer) {
  std::shared_ptr<Peer> replaced;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : peers_) {
      if (entry.first == vip) {
        replaced = std::move(entry.second);
        entry.second = std::move(peer);
        break;
      }
    }
    if (!replaced && peer) {
      peers_.emplace_back(vip, std::move(peer));
    }
  }
  // `replaced` is released here, outside the lock.
}

void Transport::Unregister(Peer* peer) {
  std::vector<std::shared_ptr<Peer>> removed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = peers_.begin(); it != peers_.end();) {
      if (it->second.get() == peer) {
        removed.push_back(std::move(it->second));
        it = peers_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

std::shared_ptr<Peer> Transport::Find(uint32_t vip) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : peers_) {
    if (entry.first == vip) {
      return entry.second;
    }
  }
  return nullptr;
}

void Transport::Expect(uint32_t vip) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& pending : pending_) {
    if (pending.vip == vip) {
      return;
    }
  }
  pending_.push_back({vip, {}});
}

size_t Transport::Release(uint32_t vip, bool flush) {
  std::vector<std::pair<int64_t, std::vector<uint8_t>>> frames;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
      if (it->vip == vip) {
        frames = std::move(it->frames);
        pending_.erase(it);
        break;
      }
    }
  }
  if (!flush || frames.empty()) {
    return 0;
  }
  auto peer = Find(vip);
  if (!peer) {
    return 0;
  }
  const int64_t now = NowUs();
  size_t sent = 0;
  for (auto& [time, frame] : frames) {
    if (now - time > kMaxPendingAgeMs * 1000 || frame.size() < 4) {
      continue;
    }
    SendGame(peer, LoadBe16(frame.data()), LoadBe16(frame.data() + 2), frame.data() + 4,
             frame.size() - 4);
    ++sent;
  }
  return sent;
}

bool Transport::Send(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t* data,
                     size_t size) {
  std::shared_ptr<Peer> peer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pending : pending_) {
      if (pending.vip == dst_ip) {
        if (pending.frames.size() < kMaxPendingFrames) {
          std::vector<uint8_t> frame(4 + size);
          StoreBe16(frame.data(), src_port);
          StoreBe16(frame.data() + 2, dst_port);
          std::memcpy(frame.data() + 4, data, size);
          pending.frames.emplace_back(NowUs(), std::move(frame));
        }
        return true;
      }
    }
    for (auto& entry : peers_) {
      if (entry.first == dst_ip) {
        peer = entry.second;
        break;
      }
    }
    if (peer && !peer->connected()) {
      // The link isn't up yet (the peer's frames can arrive before our agent
      // reports connected): hold the datagram; the owner flushes on connect.
      std::vector<uint8_t> frame(4 + size);
      StoreBe16(frame.data(), src_port);
      StoreBe16(frame.data() + 2, dst_port);
      std::memcpy(frame.data() + 4, data, size);
      pending_.push_back({dst_ip, {}});
      pending_.back().frames.emplace_back(NowUs(), std::move(frame));
      return true;
    }
  }
  if (!peer) {
    dropped_unknown_.fetch_add(1);
    return true;
  }
  SendGame(peer, src_port, dst_port, data, size);
  return true;
}

void Transport::SendGame(const std::shared_ptr<Peer>& peer, uint16_t src_port, uint16_t dst_port,
                         const uint8_t* data, size_t size) {
  std::vector<uint8_t> frame(9 + size);
  frame[0] = 1;
  StoreBe16(frame.data() + 1, src_port);
  StoreBe16(frame.data() + 3, dst_port);
  StoreBe32(frame.data() + 5, peer->NextSeq());
  std::memcpy(frame.data() + 9, data, size);
  int redundancy;
  std::vector<int> delays;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    redundancy = options_.redundancy;
    delays = options_.copy_delays_ms;
  }
  if (peer->relay_path()) {
    redundancy = std::min(redundancy, 1);  // relayed bandwidth is paid
  }
  for (int i = 0; i < redundancy; ++i) {
    const int delay_ms = i < int(delays.size()) ? delays[i] : (delays.empty() ? 5 : delays.back() + 7 * (i - int(delays.size()) + 1));
    SendWire(peer, frame, int64_t(std::max(1, delay_ms)) * 1000, true);
  }
  SendWire(peer, std::move(frame), 0, false);
}

void Transport::SendSide(const std::shared_ptr<Peer>& peer, uint8_t kind, const uint8_t* body,
                         size_t size) {
  if (!peer || size > 0xFFFF) {
    return;
  }
  std::vector<uint8_t> frame(4 + size);
  frame[0] = 2;
  frame[1] = kind;
  StoreBe16(frame.data() + 2, static_cast<uint16_t>(size));
  if (size) {
    std::memcpy(frame.data() + 4, body, size);
  }
  SendWire(peer, std::move(frame), 0, false);
}

std::shared_ptr<Peer> Transport::FirstConnected() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : peers_) {
    if (entry.second && entry.second->connected()) {
      return entry.second;
    }
  }
  return nullptr;
}

bool Transport::SendGameSide(uint8_t kind, const uint8_t* body, size_t size, int copies) {
  if (kind < kGameSideKindFirst || kind > kGameSideKindLast || size > 0xFFFF) {
    return false;
  }
  auto peer = FirstConnected();
  if (!peer) {
    return false;
  }
  std::vector<uint8_t> frame(4 + size);
  frame[0] = 2;
  frame[1] = kind;
  StoreBe16(frame.data() + 2, static_cast<uint16_t>(size));
  if (size) {
    std::memcpy(frame.data() + 4, body, size);
  }
  if (peer->relay_path()) {
    copies = 0;  // relayed bandwidth is paid
  }
  std::vector<int> delays;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    delays = options_.copy_delays_ms;
  }
  for (int i = 0; i < std::clamp(copies, 0, 3); ++i) {
    const int delay_ms = i < int(delays.size()) ? delays[i] : 5 + 7 * i;
    SendWire(peer, frame, int64_t(std::max(1, delay_ms)) * 1000, true);
  }
  SendWire(peer, std::move(frame), 0, false);
  return true;
}

void Transport::SetGameSideHandler(std::function<void(uint8_t, const uint8_t*, size_t)> handler) {
  std::lock_guard<std::mutex> lock(game_side_mutex_);
  if (handler) {
    game_side_handler_ =
        std::make_shared<std::function<void(uint8_t, const uint8_t*, size_t)>>(std::move(handler));
  } else {
    game_side_handler_.reset();
  }
}

void Transport::DispatchGameSide(uint8_t kind, const uint8_t* body, size_t size) {
  std::shared_ptr<std::function<void(uint8_t, const uint8_t*, size_t)>> handler;
  {
    std::lock_guard<std::mutex> lock(game_side_mutex_);
    handler = game_side_handler_;
  }
  if (handler && *handler) {
    (*handler)(kind, body, size);
  }
}

void Transport::SendWire(const std::shared_ptr<Peer>& peer, std::vector<uint8_t> frame,
                         int64_t delay_us, bool copy) {
  if (!peer) {
    return;
  }
  int latency, jitter, loss;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    latency = options_.sim_latency_ms;
    jitter = options_.sim_jitter_ms;
    loss = options_.sim_loss_pct;
  }
  if (frame.size() && frame[0] == 1) {
    peer->CountOut(copy);
  }
  if (loss > 0 || jitter > 0) {
    std::lock_guard<std::mutex> lock(rng_mutex_);
    auto next = [this] {
      rng_ ^= rng_ << 13;
      rng_ ^= rng_ >> 7;
      rng_ ^= rng_ << 17;
      return rng_;
    };
    if (loss > 0 && int(next() % 100) < loss) {
      peer->CountSimDrop();
      return;
    }
    if (jitter > 0) {
      delay_us += int64_t(next() % uint64_t(jitter + 1)) * 1000;
    }
  }
  delay_us += int64_t(latency) * 1000;
  if (delay_us <= 0) {
    if (!peer->connected()) {
      peer->CountNotUp();
    } else if (juice_send(peer->agent(), reinterpret_cast<const char*>(frame.data()),
                          frame.size()) < 0) {
      peer->CountSendError();
    } else {
      peer->CountBytesOut(frame.size());
    }
    return;
  }
  {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    jobs_.push_back({NowUs() + delay_us, job_order_++, peer, std::move(frame)});
    std::push_heap(jobs_.begin(), jobs_.end(), std::greater<Job>());
  }
#if defined(_WIN32)
  SetEvent(static_cast<HANDLE>(wake_));
#endif
}

void Transport::SendWireDeferred(std::weak_ptr<Peer> peer, Peer* raw,
                                 std::vector<uint8_t> frame) {
  int latency, jitter, loss;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    latency = options_.sim_latency_ms;
    jitter = options_.sim_jitter_ms;
    loss = options_.sim_loss_pct;
  }
  int64_t delay_us = int64_t(latency) * 1000;
  if (loss > 0 || jitter > 0) {
    std::lock_guard<std::mutex> lock(rng_mutex_);
    auto next = [this] {
      rng_ ^= rng_ << 13;
      rng_ ^= rng_ >> 7;
      rng_ ^= rng_ << 17;
      return rng_;
    };
    if (loss > 0 && int(next() % 100) < loss) {
      raw->CountSimDrop();
      return;
    }
    if (jitter > 0) {
      delay_us += int64_t(next() % uint64_t(jitter + 1)) * 1000;
    }
  }
  {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    jobs_.push_back({NowUs() + delay_us, job_order_++, std::move(peer), std::move(frame)});
    std::push_heap(jobs_.begin(), jobs_.end(), std::greater<Job>());
  }
#if defined(_WIN32)
  SetEvent(static_cast<HANDLE>(wake_));
#endif
}

void Transport::TimerLoop() {
  for (;;) {
    std::vector<Job> ready;
    int64_t wait_us = -1;
    const int64_t now = NowUs();
    {
      std::lock_guard<std::mutex> lock(timer_mutex_);
      while (!jobs_.empty() && jobs_.front().due_us <= now) {
        std::pop_heap(jobs_.begin(), jobs_.end(), std::greater<Job>());
        ready.push_back(std::move(jobs_.back()));
        jobs_.pop_back();
      }
      if (!jobs_.empty()) {
        wait_us = jobs_.front().due_us - now;
      }
    }
    for (auto& job : ready) {
      auto peer = job.peer.lock();
      if (!peer) {
        continue;
      }
      peer->AddTimerLateness(double(NowUs() - job.due_us) / 1000.0);
      if (!peer->connected()) {
        peer->CountNotUp();  // a copy or pong before the link is up: skip it
      } else if (juice_send(peer->agent(), reinterpret_cast<const char*>(job.frame.data()),
                            job.frame.size()) < 0) {
        peer->CountSendError();
      } else {
        peer->CountBytesOut(job.frame.size());
      }
    }
    if (!ready.empty()) {
      continue;
    }
#if defined(_WIN32)
    if (wait_us < 0) {
      WaitForSingleObject(static_cast<HANDLE>(wake_), INFINITE);
    } else {
      LARGE_INTEGER due;
      due.QuadPart = -std::max<int64_t>(1, wait_us * 10);  // 100 ns units, relative
      SetWaitableTimer(static_cast<HANDLE>(timer_), &due, 0, nullptr, nullptr, FALSE);
      HANDLE handles[2] = {static_cast<HANDLE>(wake_), static_cast<HANDLE>(timer_)};
      WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    }
#else
    std::this_thread::sleep_for(std::chrono::microseconds(
        wait_us < 0 ? 1000 : std::min<int64_t>(wait_us, 1000)));
#endif
  }
}

void Transport::LoopBack(uint32_t from_ip, uint16_t from_port, uint16_t dst_port,
                         const uint8_t* data, size_t size) {
  Datagram datagram;
  datagram.from_ip = from_ip;
  datagram.from_port = from_port;
  datagram.data.assign(data, data + size);
  Deliver(dst_port, std::move(datagram));
}

void Transport::Deliver(uint16_t dst_port, Datagram datagram) {
  std::lock_guard<std::mutex> lock(inbound_mutex_);
  for (auto& entry : inbound_) {
    if (entry.first == dst_port) {
      if (entry.second.size() >= kMaxInboundPerPort) {
        entry.second.erase(entry.second.begin());
      }
      entry.second.push_back(std::move(datagram));
      return;
    }
  }
  inbound_.emplace_back(dst_port, std::vector<Datagram>{});
  inbound_.back().second.push_back(std::move(datagram));
}

bool Transport::Recv(uint16_t dst_port, Datagram* out) {
  std::lock_guard<std::mutex> lock(inbound_mutex_);
  for (auto& entry : inbound_) {
    if (entry.first == dst_port && !entry.second.empty()) {
      *out = std::move(entry.second.front());
      entry.second.erase(entry.second.begin());
      return true;
    }
  }
  return false;
}

void Transport::ClearInbound() {
  std::lock_guard<std::mutex> lock(inbound_mutex_);
  inbound_.clear();
}

}  // namespace rex::net::online
