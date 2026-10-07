/**
 * @file        ui/debug_command_pipe.cpp
 * @brief       Console lines over a local named pipe. See debug_command_pipe.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/debug_command_pipe.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/overlay/console_overlay.h>

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#endif

namespace rex::ui {

namespace {

// One line from the pipe, run on the UI thread and answered on the pipe
// thread.
struct PendingLine {
#if REX_PLATFORM_WIN32
  PendingLine() : done_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~PendingLine() {
    if (done_event) {
      CloseHandle(done_event);
    }
  }
  HANDLE done_event;
#endif

  void Complete(ConsoleLineResult new_result) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (done) {
        return;
      }
      done = true;
      result = std::move(new_result);
    }
#if REX_PLATFORM_WIN32
    SetEvent(done_event);
#endif
  }

  std::mutex mutex;
  bool done = false;
  // A command took the completion over (DeferCommandCompletion).
  bool deferred = false;
  ConsoleLineResult result;
};

// The pipe line the current thread is running, for DeferCommandCompletion.
thread_local std::shared_ptr<PendingLine> g_running_line;

}  // namespace

CommandCompletion DeferCommandCompletion() {
  std::shared_ptr<PendingLine> line = g_running_line;
  if (!line) {
    return [](bool, std::string) {};
  }
  {
    std::lock_guard<std::mutex> lock(line->mutex);
    line->deferred = true;
  }
  return [line](bool ok, std::string message) {
    line->Complete(
        {ok ? ConsoleLineStatus::kOk : ConsoleLineStatus::kError, std::move(message)});
  };
}

struct DebugCommandPipe::State {
  std::string name;
  UIThreadPoster post;
  // Cleared on the UI thread by Stop(); UI-thread work checks it first.
  std::atomic<bool> accepting{true};
#if REX_PLATFORM_WIN32
  HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ~State() {
    if (stop_event) {
      CloseHandle(stop_event);
    }
  }
#endif
};

#if REX_PLATFORM_WIN32

namespace {

// Generous for any command, but a command that never completes its deferred
// work must not wedge the pipe.
constexpr DWORD kLineTimeoutMs = 120000;
// A client that sends this much without a newline is dropped.
constexpr size_t kMaxLineBytes = 64 * 1024;

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring wide(size_t(std::max(length, 0)), L'\0');
  if (length > 0) {
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), length);
  }
  return wide;
}

// Security descriptor that lets only this user (and SYSTEM) open the pipe -
// the default one would also let everyone connect for reading.
PSECURITY_DESCRIPTOR CreateCurrentUserOnlyDescriptor() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return nullptr;
  }
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<uint8_t> buffer(size);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  LPWSTR sid_text = nullptr;
  if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) &&
      ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid_text)) {
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid_text) + L")";
    LocalFree(sid_text);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr)) {
      descriptor = nullptr;
    }
  }
  CloseHandle(token);
  return descriptor;
}

enum class IoWait { kDone, kFailed, kStopped };

// Waits for an overlapped operation on `pipe`, or for the stop event (then
// cancels it).
IoWait WaitIo(HANDLE pipe, OVERLAPPED& overlapped, HANDLE stop_event, DWORD* transferred) {
  DWORD bytes = 0;
  const HANDLE handles[2] = {overlapped.hEvent, stop_event};
  const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
  if (wait != WAIT_OBJECT_0) {
    CancelIoEx(pipe, &overlapped);
    GetOverlappedResult(pipe, &overlapped, &bytes, TRUE);
    return IoWait::kStopped;
  }
  if (!GetOverlappedResult(pipe, &overlapped, &bytes, FALSE)) {
    return IoWait::kFailed;
  }
  if (transferred) {
    *transferred = bytes;
  }
  return IoWait::kDone;
}

// Starts an overlapped read or write and waits for it.
IoWait TransferIo(HANDLE pipe, HANDLE io_event, HANDLE stop_event, bool write, void* data,
                  DWORD size, DWORD* transferred) {
  OVERLAPPED overlapped = {};
  overlapped.hEvent = io_event;
  ResetEvent(io_event);
  const BOOL started = write ? WriteFile(pipe, data, size, nullptr, &overlapped)
                             : ReadFile(pipe, data, size, nullptr, &overlapped);
  if (!started && GetLastError() != ERROR_IO_PENDING) {
    return IoWait::kFailed;
  }
  return WaitIo(pipe, overlapped, stop_event, transferred);
}

std::string AckText(const ConsoleLineResult& result) {
  std::string message = result.message;
  for (char& c : message) {
    if (c == '\r' || c == '\n') {
      c = ' ';
    }
  }
  switch (result.status) {
    case ConsoleLineStatus::kOk:
      return message.empty() ? "ok" : "ok " + message;
    case ConsoleLineStatus::kUnknownCommand:
      return "unknown command";
    case ConsoleLineStatus::kError:
    default:
      return message.empty() ? "error" : "error " + message;
  }
}

// Runs one line on the UI thread and waits for its result. False when the pipe
// is stopping (no ack then).
bool RunLine(const std::shared_ptr<DebugCommandPipe::State>& state, std::string line,
             std::string& ack) {
  auto pending = std::make_shared<PendingLine>();
  if (!pending->done_event) {
    ack = "error out of resources";
    return true;
  }
  const bool posted = state->post([state, pending, line = std::move(line)]() {
    if (!state->accepting.load(std::memory_order_acquire)) {
      pending->Complete({ConsoleLineStatus::kError, "shutting down"});
      return;
    }
    ConsoleLineResult result;
    g_running_line = pending;
    try {
      result = ExecuteConsoleLine(line, "pipe", [](spdlog::level::level_enum level,
                                                   std::string text) {
        if (level >= spdlog::level::warn) {
          REXLOG_WARN("{}", text);
        } else {
          REXLOG_INFO("{}", text);
        }
      });
    } catch (const std::exception& e) {
      result = {ConsoleLineStatus::kError, e.what()};
    } catch (...) {
      result = {ConsoleLineStatus::kError, "exception"};
    }
    g_running_line.reset();
    bool deferred;
    {
      std::lock_guard<std::mutex> lock(pending->mutex);
      deferred = pending->deferred;
    }
    if (!deferred || result.status != ConsoleLineStatus::kOk) {
      pending->Complete(std::move(result));
    }
  });
  if (!posted) {
    ack = "error shutting down";
    return true;
  }
  const HANDLE handles[2] = {pending->done_event, state->stop_event};
  const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, kLineTimeoutMs);
  if (wait == WAIT_OBJECT_0) {
    std::lock_guard<std::mutex> lock(pending->mutex);
    ack = AckText(pending->result);
    return true;
  }
  if (wait == WAIT_TIMEOUT) {
    ack = "error timed out";
    return true;
  }
  return false;
}

void Serve(std::shared_ptr<DebugCommandPipe::State> state) {
  const std::wstring path = L"\\\\.\\pipe\\" + Utf8ToWide(state->name);
  PSECURITY_DESCRIPTOR descriptor = CreateCurrentUserOnlyDescriptor();
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
  // FIRST_PIPE_INSTANCE: never share the name with a pipe someone else made.
  const HANDLE pipe = CreateNamedPipeW(
      path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096,
      0, descriptor ? &attributes : nullptr);
  const DWORD create_error = GetLastError();
  const bool user_only = descriptor != nullptr;
  if (descriptor) {
    LocalFree(descriptor);
  }
  if (pipe == INVALID_HANDLE_VALUE) {
    REXLOG_ERROR("Debug command pipe: can't create \\\\.\\pipe\\{} (error {}){}", state->name,
                 create_error,
                 create_error == ERROR_ACCESS_DENIED ? " - is the name already in use?" : "");
    return;
  }
  const HANDLE io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!io_event) {
    CloseHandle(pipe);
    return;
  }
  REXLOG_INFO("Debug command pipe: listening on \\\\.\\pipe\\{}{}", state->name,
              user_only ? "" : " (default security)");

  bool stopping = false;
  while (!stopping) {
    OVERLAPPED overlapped = {};
    overlapped.hEvent = io_event;
    ResetEvent(io_event);
    if (!ConnectNamedPipe(pipe, &overlapped)) {
      const DWORD error = GetLastError();
      if (error == ERROR_IO_PENDING) {
        const IoWait wait = WaitIo(pipe, overlapped, state->stop_event, nullptr);
        if (wait == IoWait::kStopped) {
          break;
        }
        if (wait == IoWait::kFailed) {
          DisconnectNamedPipe(pipe);
          continue;
        }
      } else if (error != ERROR_PIPE_CONNECTED) {
        REXLOG_WARN("Debug command pipe: ConnectNamedPipe failed (error {})", error);
        DisconnectNamedPipe(pipe);
        if (WaitForSingleObject(state->stop_event, 250) == WAIT_OBJECT_0) {
          break;
        }
        continue;
      }
    }
    REXLOG_INFO("Debug command pipe: client connected");

    std::string buffer;
    char chunk[1024];
    bool connected = true;
    while (connected && !stopping) {
      DWORD read = 0;
      const IoWait wait = TransferIo(pipe, io_event, state->stop_event, false, chunk,
                                     DWORD(sizeof(chunk)), &read);
      if (wait == IoWait::kStopped) {
        stopping = true;
        break;
      }
      if (wait == IoWait::kFailed) {
        // ERROR_BROKEN_PIPE: the client went away.
        break;
      }
      buffer.append(chunk, read);
      size_t newline;
      while (connected && (newline = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, newline);
        buffer.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        std::string ack;
        if (!RunLine(state, std::move(line), ack)) {
          stopping = true;
          break;
        }
        ack += '\n';
        DWORD written = 0;
        const IoWait write_wait = TransferIo(pipe, io_event, state->stop_event, true, ack.data(),
                                             DWORD(ack.size()), &written);
        if (write_wait == IoWait::kStopped) {
          stopping = true;
          break;
        }
        if (write_wait == IoWait::kFailed) {
          connected = false;
        }
      }
      if (buffer.size() > kMaxLineBytes) {
        REXLOG_WARN("Debug command pipe: line too long, dropping the client");
        break;
      }
    }
    DisconnectNamedPipe(pipe);
    if (!stopping) {
      REXLOG_INFO("Debug command pipe: client disconnected");
    }
  }
  CloseHandle(io_event);
  CloseHandle(pipe);
}

}  // namespace

bool DebugCommandPipe::Start(const std::string& name, UIThreadPoster post_to_ui_thread) {
  if (state_ || name.empty() || !post_to_ui_thread) {
    return false;
  }
  if (name.find_first_of("\\/") != std::string::npos) {
    REXLOG_ERROR("Debug command pipe: the name can't contain slashes: {}", name);
    return false;
  }
  auto state = std::make_shared<State>();
  if (!state->stop_event) {
    return false;
  }
  state->name = name;
  state->post = std::move(post_to_ui_thread);
  state_ = state;
  thread_ = std::thread([state]() { Serve(state); });
  return true;
}

void DebugCommandPipe::Stop() {
  if (!state_) {
    return;
  }
  state_->accepting.store(false, std::memory_order_release);
  SetEvent(state_->stop_event);
  if (thread_.joinable()) {
    thread_.join();
  }
  state_.reset();
}

#else  // REX_PLATFORM_WIN32

bool DebugCommandPipe::Start(const std::string& name, UIThreadPoster post_to_ui_thread) {
  (void)post_to_ui_thread;
  REXLOG_ERROR("Debug command pipe: named pipes are only implemented on Windows ({})", name);
  return false;
}

void DebugCommandPipe::Stop() {}

#endif  // REX_PLATFORM_WIN32

DebugCommandPipe::~DebugCommandPipe() {
  Stop();
}

}  // namespace rex::ui
