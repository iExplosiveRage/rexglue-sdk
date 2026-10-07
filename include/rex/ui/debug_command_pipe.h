/**
 * @file        rex/ui/debug_command_pipe.h
 * @brief       Test-automation control channel: console lines over a local
 *              named pipe, no keyboard or window focus needed.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace rex::ui {

/// Lets a console command finish its work later, on another thread. The
/// command calls this while it runs and calls the returned function exactly
/// once when the work is done. A line that came through the debug command
/// pipe is acknowledged only then ("ok[ <message>]" or "error[ <message>]");
/// for a line typed into the console the call does nothing, so the command
/// logs its own result either way.
using CommandCompletion = std::function<void(bool ok, std::string message)>;
CommandCompletion DeferCommandCompletion();

/// Serves the local named pipe \\.\pipe\<name> (Windows only; remote clients
/// are rejected and only the current user may open it). Every line a client
/// writes ('\n'-terminated, UTF-8) runs exactly like a line typed into the
/// console, on the UI thread, and is answered with one line:
///   "ok[ <detail>]"   - done (a cvar read answers "ok <name> = <value>")
///   "unknown command" - no such command or cvar
///   "error[ <detail>]"- invalid value, the command failed, or timed out
/// Lines are run one at a time in order. One client at a time; a client may
/// disconnect and the next one connect.
class DebugCommandPipe {
 public:
  /// Queues `function` to run on the UI thread; false if it can't anymore.
  using UIThreadPoster = std::function<bool(std::function<void()>)>;

  DebugCommandPipe() = default;
  ~DebugCommandPipe();
  DebugCommandPipe(const DebugCommandPipe&) = delete;
  DebugCommandPipe& operator=(const DebugCommandPipe&) = delete;

  /// Starts the pipe thread. False (and logged) if the platform has no named
  /// pipes or the name is empty.
  bool Start(const std::string& name, UIThreadPoster post_to_ui_thread);

  /// Stops serving. Call on the UI thread: no line runs after it returns.
  void Stop();

  struct State;

 private:
  std::shared_ptr<State> state_;
  std::thread thread_;
};

}  // namespace rex::ui
