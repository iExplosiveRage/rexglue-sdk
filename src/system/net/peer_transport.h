/**
 * @file        system/net/peer_transport.h
 * @brief       ICE link to one peer (libjuice) and the datagram transport on
 *              top of it: framing, sequence numbers, timed duplicate copies,
 *              de-duplication, the side channel, simulated latency / loss and
 *              stats.
 *
 * Wire frames (first byte = type, integers big-endian):
 *   1  [u8 1][u16 src_port][u16 dst_port][u32 seq][payload]   one game datagram
 *      (ports as the game sees them; duplicate copies reuse the seq)
 *   2  [u8 2][u8 kind][u16 length][body]                       side channel
 *      kinds: 1 hello (JSON), 2 ping [u32 id][u32 ms], 3 pong (echo), 4 bye,
 *      16..31 the game's own (rex::net::online::SendGameSide)
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/net/online.h>

typedef struct juice_agent juice_agent_t;

namespace rex::net::online {

struct IceServers {
  std::string stun_host;
  uint16_t stun_port = 0;
  struct Turn {
    std::string host;
    uint16_t port = 3478;
    std::string username;
    std::string password;
  };
  std::vector<Turn> turn;
};

struct PeerOptions {
  bool controlling = false;
  IceServers servers;
  std::string bind_ip;
  uint16_t port_begin = 0;
  uint16_t port_end = 0;
  bool relay_only = false;
  int debug_connect_delay_ms = 0;  // test: report "connected" late
};

enum SideKind : uint8_t {
  kSideHello = 1,
  kSidePing = 2,
  kSidePong = 3,
  kSideBye = 4,
};

class Peer : public std::enable_shared_from_this<Peer> {
 public:
  // Run on libjuice's thread: keep them short (post to the owner's thread).
  struct Events {
    std::function<void(Peer*, std::string sdp)> on_candidate;
    std::function<void(Peer*)> on_gathering_done;
    std::function<void(Peer*, int juice_state)> on_state;
    std::function<void(Peer*, std::string body)> on_hello;
    std::function<void(Peer*)> on_bye;
  };

  Peer(uint64_t id, PeerOptions options, Events events);
  ~Peer();
  Peer(const Peer&) = delete;
  Peer& operator=(const Peer&) = delete;

  // Owner thread only (never from a libjuice callback).
  bool Create();
  std::string LocalDescription();
  bool SetRemoteDescription(const std::string& sdp);
  void AddRemoteCandidate(const std::string& sdp);
  void SetRemoteGatheringDone();
  void Gather();
  std::string SelectedPath();  // "srflx/host" etc., empty if none

  uint64_t id() const { return id_; }
  bool controlling() const { return options_.controlling; }
  bool relay_only() const { return options_.relay_only; }

  uint32_t vip() const { return vip_.load(); }
  void set_vip(uint32_t vip) { vip_.store(vip); }
  bool connected() const { return connected_.load(); }
  uint32_t rtt_ms() const { return rtt_ms_.load(); }

  // Stats (read by the owner once a second).
  struct Stats {
    uint64_t out_frames = 0, copies = 0, in_unique = 0, dup = 0, old = 0;
    uint64_t sim_dropped = 0, send_errors = 0;
    uint64_t bytes_out = 0, bytes_in = 0;  // wire frames (before TURN / UDP headers)
    uint64_t not_up = 0;  // frames skipped because the link wasn't up (copies, pongs)
    uint64_t missing = 0;  // seqs up to the highest seen that never arrived
    uint32_t rtt_ms = 0;
    double timer_late_ms = 0;  // average lateness of timed sends
  };
  Stats TakeStats();

  // Transport internals.
  juice_agent_t* agent() const { return agent_; }
  uint32_t NextSeq() { return seq_.fetch_add(1) + 1; }
  void CountOut(bool copy) { (copy ? copies_ : out_frames_).fetch_add(1); }
  void CountSendError() { send_errors_.fetch_add(1); }
  void CountNotUp() { not_up_.fetch_add(1); }
  void CountBytesOut(size_t bytes) {
    bytes_out_.fetch_add(bytes);
    total_bytes_out_.fetch_add(bytes);
  }
  uint64_t total_bytes_out() const { return total_bytes_out_.load(); }
  uint64_t total_bytes_in() const { return total_bytes_in_.load(); }
  /// True when the selected pair uses a TURN relay on either side: fewer
  /// duplicate copies there (relay bandwidth is paid).
  bool relay_path() const { return relay_path_.load(); }
  void set_relay_path(bool relay) { relay_path_.store(relay); }
  void CountSimDrop() { sim_dropped_.fetch_add(1); }
  void AddTimerLateness(double ms);
  void OnWire(const uint8_t* data, size_t size);  // libjuice thread
  void OnJuiceState(int state);
  void OnJuiceCandidate(const char* sdp);
  void OnJuiceGatheringDone();

 private:
  bool AcceptSeq(uint32_t seq);

  const uint64_t id_;
  const PeerOptions options_;
  const Events events_;
  juice_agent_t* agent_ = nullptr;
  std::atomic<uint32_t> vip_{0};
  std::atomic<bool> connected_{false};
  std::atomic<uint32_t> seq_{0};

  std::mutex window_mutex_;
  uint32_t highest_ = 0;
  uint64_t unique_total_ = 0;
  std::vector<uint64_t> window_;  // bitmap of the last kWindow seqs

  std::atomic<uint64_t> out_frames_{0}, copies_{0}, in_unique_{0}, dup_{0}, old_{0};
  std::atomic<uint64_t> sim_dropped_{0}, send_errors_{0};
  std::atomic<uint64_t> bytes_out_{0}, bytes_in_{0}, total_bytes_out_{0}, total_bytes_in_{0};
  std::atomic<bool> relay_path_{false};
  std::atomic<uint64_t> not_up_{0};
  std::atomic<uint32_t> rtt_ms_{0};
  std::mutex late_mutex_;
  double late_sum_ms_ = 0;
  uint64_t late_count_ = 0;
};

/// The datagram layer shared by all peers (one per process).
class Transport {
 public:
  static Transport& Get();

  struct Options {
    int redundancy = 2;
    std::vector<int> copy_delays_ms = {5, 12};
    int sim_latency_ms = 0;
    int sim_jitter_ms = 0;
    int sim_loss_pct = 0;
  };
  void SetOptions(const Options& options);

  void Register(uint32_t vip, std::shared_ptr<Peer> peer);
  void Unregister(Peer* peer);
  std::shared_ptr<Peer> Find(uint32_t vip);

  /// Guest join: datagrams to `vip` wait (max 64, 10 s) until Release.
  /// (Datagrams to a registered peer whose link isn't up yet wait the same way.)
  void Expect(uint32_t vip);
  /// Ends the wait: flushes the queue to the peer (flush) or drops it.
  size_t Release(uint32_t vip, bool flush);

  // Game side.
  bool Send(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t* data,
            size_t size);
  void LoopBack(uint32_t from_ip, uint16_t from_port, uint16_t dst_port, const uint8_t* data,
                size_t size);
  bool Recv(uint16_t dst_port, Datagram* out);
  void ClearInbound();

  // Peer side.
  void Deliver(uint16_t dst_port, Datagram datagram);
  void SendSide(const std::shared_ptr<Peer>& peer, uint8_t kind, const uint8_t* body,
                size_t size);
  /// Sends a wire frame now or after `delay_us` (plus the simulated network).
  void SendWire(const std::shared_ptr<Peer>& peer, std::vector<uint8_t> frame, int64_t delay_us,
                bool copy);
  /// From a libjuice callback: always through the timed-send thread, holding
  /// only a weak reference (a peer must never be destroyed inside a callback).
  void SendWireDeferred(std::weak_ptr<Peer> peer, Peer* raw, std::vector<uint8_t> frame);

  uint64_t dropped_unknown() const { return dropped_unknown_.load(); }

  // Game side channel (kinds kGameSideKindFirst..Last).
  /// The first registered peer whose link is up (a session has one peer).
  std::shared_ptr<Peer> FirstConnected();
  /// Sends a side frame plus `copies` timed copies (direct paths only).
  bool SendGameSide(uint8_t kind, const uint8_t* body, size_t size, int copies);
  void SetGameSideHandler(std::function<void(uint8_t, const uint8_t*, size_t)> handler);
  /// From Peer::OnWire (libjuice thread).
  void DispatchGameSide(uint8_t kind, const uint8_t* body, size_t size);

 private:
  Transport();
  void SendGame(const std::shared_ptr<Peer>& peer, uint16_t src_port, uint16_t dst_port,
                const uint8_t* data, size_t size);
  void TimerLoop();

  struct Job {
    int64_t due_us;
    uint64_t order;
    std::weak_ptr<Peer> peer;
    std::vector<uint8_t> frame;
    bool operator>(const Job& other) const {
      return due_us != other.due_us ? due_us > other.due_us : order > other.order;
    }
  };

  std::mutex mutex_;
  Options options_;
  std::vector<std::pair<uint32_t, std::shared_ptr<Peer>>> peers_;
  struct Pending {
    uint32_t vip;
    std::vector<std::pair<int64_t, std::vector<uint8_t>>> frames;  // time, [src][dst][payload]
  };
  std::vector<Pending> pending_;

  std::mutex inbound_mutex_;
  std::vector<std::pair<uint16_t, std::vector<Datagram>>> inbound_;  // per port, FIFO

  std::mutex timer_mutex_;
  std::vector<Job> jobs_;  // min-heap
  uint64_t job_order_ = 0;
  void* timer_ = nullptr;
  void* wake_ = nullptr;
  bool high_resolution_ = false;

  std::mutex rng_mutex_;
  uint64_t rng_ = 0x9E3779B97F4A7C15ull;

  std::atomic<uint64_t> dropped_unknown_{0};

  std::mutex game_side_mutex_;
  std::shared_ptr<std::function<void(uint8_t, const uint8_t*, size_t)>> game_side_handler_;
};

/// Debug switch: a local STUN/TURN server (libjuice) for relay-only tests
/// without a TURN account. Credentials test/test.
bool StartDebugTurnServer(uint16_t port);

/// Some networks reach Cloudflare's TURN anycast address only from some local
/// UDP ports (a port either always gets answers or never does). Sends a STUN
/// Binding to host:port from `count` fresh sockets and returns a local port that
/// got an answer within `timeout_ms` (0 if none); the ICE agent then binds it.
/// `answered` receives how many of the probes were answered.
uint16_t PickTurnFriendlyPort(const std::string& host, uint16_t port, int timeout_ms, int count,
                              int* answered);

/// Routes libjuice's log to ours at the given level name.
void SetIceLogLevel(const std::string& level);

}  // namespace rex::net::online
