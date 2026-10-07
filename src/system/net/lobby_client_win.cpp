/**
 * @file        system/net/lobby_client_win.cpp
 * @brief       Lobby WebSocket over WinHTTP (Windows, and Wine 6.0+).
 *
 * One receive thread (connect, blocking receive, reconnect with backoff) and
 * one send thread (queue, pacing, keep-alive ping). Sends never run on the
 * receive thread and only one send runs at a time: Wine's WinHTTP WebSocket
 * isn't safe for a send racing a receive that answers a control frame (Wine
 * bug 58556), and the lobby never sends WebSocket-level pings, so the only
 * such frame is the final close.
 */

#include "lobby_client.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>

namespace rex::net::online {
namespace {

constexpr char kPing[] = "{\"t\":\"ping\"}";
constexpr int64_t kKeepAliveMs = 20000;
constexpr size_t kMaxSendsPerSecond = 15;
constexpr size_t kMaxMessageBytes = 1 << 20;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::wstring Widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int length =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
  return out;
}

struct ParsedUrl {
  bool secure = false;
  std::wstring host;
  INTERNET_PORT port = 0;
  std::wstring path;
};

// ws://host[:port][/path] or wss://... (WinHttpCrackUrl doesn't know ws).
bool ParseUrl(const std::string& url, ParsedUrl* out) {
  std::string rest;
  if (url.rfind("wss://", 0) == 0) {
    out->secure = true;
    rest = url.substr(6);
  } else if (url.rfind("ws://", 0) == 0) {
    out->secure = false;
    rest = url.substr(5);
  } else {
    return false;
  }
  const size_t slash = rest.find('/');
  std::string host_port = rest.substr(0, slash);
  std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
  int port = out->secure ? 443 : 80;
  std::string host = host_port;
  if (!host_port.empty() && host_port[0] == '[') {
    const size_t close = host_port.find(']');
    if (close == std::string::npos) {
      return false;
    }
    host = host_port.substr(1, close - 1);
    if (close + 1 < host_port.size() && host_port[close + 1] == ':') {
      port = std::atoi(host_port.c_str() + close + 2);
    }
  } else {
    const size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) {
      host = host_port.substr(0, colon);
      port = std::atoi(host_port.c_str() + colon + 1);
    }
  }
  if (host.empty() || port <= 0 || port > 65535) {
    return false;
  }
  out->host = Widen(host);
  out->port = static_cast<INTERNET_PORT>(port);
  out->path = Widen(path);
  return true;
}

class WinHttpLobbyClient final : public LobbyClient {
 public:
  bool supported() const override { return true; }

  void Start(const std::string& url, Callbacks callbacks) override {
    std::lock_guard<std::mutex> lock(mutex_);
    url_ = url;
    callbacks_ = std::move(callbacks);
    enabled_ = true;
    ++generation_;
    if (!threads_started_) {
      threads_started_ = true;
      // The client lives as long as the process (never destroyed), so the
      // threads can stay detached.
      std::thread([this] { RxLoop(); }).detach();
      std::thread([this] { TxLoop(); }).detach();
    }
    cv_.notify_all();
  }

  void Stop() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      enabled_ = false;
      ++generation_;
      queue_.clear();
      cv_.notify_all();
    }
    CloseSocket();
  }

  void Send(std::string text) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) {
      return;
    }
    queue_.push_back(std::move(text));
    cv_.notify_all();
  }

  uint32_t rtt_ms() const override { return rtt_ms_.load(std::memory_order_relaxed); }

  void OnPong() override {
    const int64_t sent = ping_sent_ms_.exchange(0);
    if (sent > 0) {
      rtt_ms_.store(static_cast<uint32_t>(std::max<int64_t>(1, NowMs() - sent)),
                    std::memory_order_relaxed);
    }
  }

 private:
  bool Connect(const std::string& url, std::string* why) {
    ParsedUrl parsed;
    if (!ParseUrl(url, &parsed)) {
      *why = "bad URL (expected ws:// or wss://)";
      return false;
    }
    HINTERNET session = WinHttpOpen(L"ReXGlue-online/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
      *why = fmt::format("WinHttpOpen failed ({})", GetLastError());
      return false;
    }
    // resolve, connect, send, receive. The lobby answers the keep-alive ping
    // every 20 s, so a minute without data means the connection is gone.
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 60000);
    HINTERNET connect = WinHttpConnect(session, parsed.host.c_str(), parsed.port, 0);
    HINTERNET request = nullptr;
    HINTERNET socket = nullptr;
    DWORD status = 0;
    if (!connect) {
      *why = fmt::format("WinHttpConnect failed ({})", GetLastError());
    } else if (!(request = WinHttpOpenRequest(connect, L"GET", parsed.path.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              parsed.secure ? WINHTTP_FLAG_SECURE : 0))) {
      *why = fmt::format("WinHttpOpenRequest failed ({})", GetLastError());
    } else if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
      *why = fmt::format("WebSocket upgrade option failed ({})", GetLastError());
    } else if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
               !WinHttpReceiveResponse(request, nullptr)) {
      *why = fmt::format("connection failed ({})", GetLastError());
    } else {
      DWORD size = sizeof(status);
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
      if (status != 101) {
        *why = fmt::format("HTTP {} instead of a WebSocket upgrade", status);
      } else if (!(socket = WinHttpWebSocketCompleteUpgrade(request, 0))) {
        *why = fmt::format("WebSocket upgrade failed ({})", GetLastError());
      }
    }
    if (request) {
      WinHttpCloseHandle(request);
    }
    if (!socket) {
      if (connect) {
        WinHttpCloseHandle(connect);
      }
      WinHttpCloseHandle(session);
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) {
      WinHttpCloseHandle(socket);
      WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      *why = "stopped";
      return false;
    }
    session_ = session;
    connect_ = connect;
    socket_ = socket;
    open_ = true;
    queue_.clear();
    last_send_ms_ = NowMs();
    cv_.notify_all();
    return true;
  }

  void CloseSocket() {
    // Never while a send is running on the handle.
    std::lock_guard<std::mutex> send_lock(send_mutex_);
    HINTERNET socket, connect, session;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      socket = socket_;
      connect = connect_;
      session = session_;
      socket_ = connect_ = session_ = nullptr;
      open_ = false;
      queue_.clear();
    }
    // Closing the handle also ends a receive blocked on it.
    if (socket) {
      WinHttpCloseHandle(socket);
    }
    if (connect) {
      WinHttpCloseHandle(connect);
    }
    if (session) {
      WinHttpCloseHandle(session);
    }
  }

  std::string ReceiveLoop(const Callbacks& callbacks) {
    HINTERNET socket;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      socket = socket_;
    }
    if (!socket) {
      return "closed";
    }
    std::vector<BYTE> buffer(16384);
    std::string message;
    bool binary = false;
    for (;;) {
      DWORD read = 0;
      WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
      const DWORD error = WinHttpWebSocketReceive(socket, buffer.data(),
                                                  static_cast<DWORD>(buffer.size()), &read, &type);
      if (error != ERROR_SUCCESS) {
        return error == ERROR_WINHTTP_TIMEOUT ? "no data for 60 s"
                                              : fmt::format("receive failed ({})", error);
      }
      switch (type) {
        case WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE:
          if (!binary) {
            message.append(reinterpret_cast<const char*>(buffer.data()), read);
            if (message.size() > kMaxMessageBytes) {
              return "message too large";
            }
          }
          break;
        case WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE:
          message.append(reinterpret_cast<const char*>(buffer.data()), read);
          if (callbacks.on_message) {
            callbacks.on_message(std::move(message));
          }
          message.clear();
          binary = false;
          break;
        case WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE:
          binary = true;
          break;
        case WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE:
          message.clear();
          binary = false;
          break;
        case WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE: {
          USHORT status = 0;
          BYTE reason[WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH] = {};
          DWORD reason_length = 0;
          WinHttpWebSocketQueryCloseStatus(socket, &status, reason, sizeof(reason),
                                           &reason_length);
          return fmt::format("closed by the lobby ({} {})", status,
                             std::string(reinterpret_cast<const char*>(reason), reason_length));
        }
        default:
          break;
      }
    }
  }

  void RxLoop() {
    static constexpr int kBackoffSeconds[] = {1, 2, 5, 10, 30};
    size_t failures = 0;
    for (;;) {
      std::string url;
      Callbacks callbacks;
      uint64_t generation;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return enabled_; });
        url = url_;
        callbacks = callbacks_;
        generation = generation_;
      }
      std::string why;
      if (Connect(url, &why)) {
        failures = 0;
        if (callbacks.on_state) {
          callbacks.on_state(true, {});
        }
        why = ReceiveLoop(callbacks);
        CloseSocket();
      } else {
        ++failures;
      }
      if (callbacks.on_state) {
        callbacks.on_state(false, why);
      }
      const int delay = kBackoffSeconds[std::min<size_t>(
          failures ? failures - 1 : 0, std::size(kBackoffSeconds) - 1)];
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, std::chrono::seconds(delay),
                   [&] { return !enabled_ || generation_ != generation; });
    }
  }

  void TxLoop() {
    std::deque<int64_t> recent;
    for (;;) {
      std::string message;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(1), [this] { return open_ && !queue_.empty(); });
        if (!open_) {
          continue;
        }
        if (!queue_.empty()) {
          message = std::move(queue_.front());
          queue_.pop_front();
        } else if (NowMs() - last_send_ms_ >= kKeepAliveMs) {
          message = kPing;
        } else {
          continue;
        }
      }
      // The lobby drops clients above 20 messages per second.
      while (!recent.empty() && NowMs() - recent.front() >= 1000) {
        recent.pop_front();
      }
      if (recent.size() >= kMaxSendsPerSecond) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            std::max<int64_t>(1, 1000 - (NowMs() - recent.front()))));
        recent.pop_front();
      }
      {
        std::lock_guard<std::mutex> send_lock(send_mutex_);
        HINTERNET socket;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          socket = socket_;
        }
        if (!socket) {
          continue;
        }
        if (message == kPing) {
          ping_sent_ms_.store(NowMs());
        }
        const DWORD error = WinHttpWebSocketSend(
            socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            const_cast<char*>(message.data()), static_cast<DWORD>(message.size()));
        if (error != ERROR_SUCCESS) {
          REXSYS_WARN("[Lobby] send failed ({})", error);
        }
      }
      const int64_t now = NowMs();
      recent.push_back(now);
      std::lock_guard<std::mutex> lock(mutex_);
      last_send_ms_ = now;
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::mutex send_mutex_;
  bool threads_started_ = false;
  bool enabled_ = false;
  bool open_ = false;
  uint64_t generation_ = 0;
  std::string url_;
  Callbacks callbacks_;
  std::deque<std::string> queue_;
  int64_t last_send_ms_ = 0;
  HINTERNET session_ = nullptr;
  HINTERNET connect_ = nullptr;
  HINTERNET socket_ = nullptr;
  std::atomic<int64_t> ping_sent_ms_{0};
  std::atomic<uint32_t> rtt_ms_{0};
};

}  // namespace

std::unique_ptr<LobbyClient> LobbyClient::Create() {
  return std::make_unique<WinHttpLobbyClient>();
}

}  // namespace rex::net::online
