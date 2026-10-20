/**
 * @file        system/interfaces/native_gpu.h
 * @brief       Native renderer interface: a title's hooked Xbox D3D calls drive
 *              the GPU backend directly on the calling guest thread
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Only exists while gpu_native_renderer was on at startup (and the
 *              backend supports it). Every call takes the locks it needs
 *              itself. Addresses are guest physical addresses (as PM4 carries
 *              them), never guest virtual ones.
 */

#pragma once

#include <cstdint>

namespace rex::system {

// Where a ring drain stopped.
enum class NativeDrainState : uint32_t {
  kIdle,             // everything kicked so far has run
  kBlockedOnWait,    // stopped before a WAIT_REG_MEM that isn't met yet
  kBlockedOnOwner,   // stopped before XE_SWAP, which the swap thread runs
  kBusy,             // another thread (or this one, further up) is draining
  kNotReady,         // the backend isn't set up yet
};

// What ExecuteScope did with a range of command buffer.
struct NativeScopeResult {
  uint32_t executed_packets = 0;  // rendering packets run now (and turned into NOPs)
  uint32_t executed_dwords = 0;
  uint32_t left_packets = 0;      // sync / present packets left for the ring
  bool ok = true;                 // false: a packet ran past the end of the range
  uint32_t left_headers[8] = {};  // the first packets left (header, then its first dword)
};

// A register write a title's native draw path makes instead of writing a
// type-0 packet into the command buffer: count registers from first_register,
// with big-endian values (as a packet would carry them). It takes effect in
// command buffer order, right before the packet at dword at_dword of the
// ExecuteScope range (at or past the end: after the range's last packet).
struct NativeRegisterWrite {
  uint32_t at_dword = 0;
  uint32_t first_register = 0;
  uint32_t count = 0;
  const uint32_t* values = nullptr;
};

// Counters since the last GetStats(reset = true).
struct NativeGpuStats {
  uint64_t drains = 0;            // TryDrain calls that ran
  uint64_t drains_idle = 0;
  uint64_t drains_blocked_wait = 0;
  uint64_t drains_blocked_owner = 0;
  uint64_t drains_busy = 0;
  uint64_t drain_packets = 0;     // packets the drains executed
  uint64_t drain_render_packets = 0;  // of them, rendering packets (expected 0)
  uint64_t drain_ns = 0;          // drain time without XE_SWAP and interrupt callbacks
  uint64_t drain_max_ns = 0;
  // Drain time histogram (without XE_SWAP and interrupts): 20 us buckets, last = longer.
  static constexpr uint32_t kDrainBuckets = 64;
  static constexpr uint32_t kDrainBucketNs = 20000;
  uint32_t drain_histogram[kDrainBuckets] = {};
  uint64_t swaps_owner = 0;       // XE_SWAP run by the swap thread
  uint64_t swaps_foreign = 0;     // by another thread after the owner timeout
  uint64_t swaps_worker = 0;      // by the worker's watchdog
  uint64_t watchdog_drains = 0;   // watchdog drains that ran something
  uint64_t interrupts = 0;        // INTERRUPT packets dispatched on guest threads
  uint64_t interrupts_worker = 0;  // on the worker (watchdog)
  // INTERRUPTs whose callback was already the XDK's 0x0BADF00D (its handler
  // would trap and call it): dispatched without the callback (expected 0).
  uint64_t interrupt_badf00d_skips = 0;
  // Guest-thread dispatches after which the interrupted thread's state wasn't
  // exactly as before (registers, host FP control, TLS pointer, CPU number;
  // expected 0).
  uint64_t interrupt_restore_mismatches = 0;
  uint64_t scopes = 0;
  uint64_t scope_packets = 0;
  uint64_t scope_dwords = 0;
  uint64_t scope_left = 0;
  uint64_t scope_failures = 0;
  uint64_t scope_ns = 0;
  uint64_t register_writes = 0;   // NativeRegisterWrite runs applied
  uint64_t register_dwords = 0;
  uint32_t first_render_packet[4] = {};  // the first rendering packet a drain met (header, address)
};

class INativeGpu {
 public:
  virtual ~INativeGpu() = default;

  // Blocks until the backend is set up. Returns the latched mode: false = this
  // session renders through the emulated command processor.
  virtual bool WaitUntilReady() = 0;

  // Runs the rendering packets in [physical, physical + dwords * 4) now, on the
  // calling thread, with the command processor's packet handlers, and turns
  // them into NOPs in guest memory. Sync and present packets (waits, fences,
  // interrupts, memory writes, indirect buffers, the swap) stay for the ring.
  // writes (sorted by at_dword) are applied in between, in command buffer
  // order; dwords may be 0 then.
  virtual bool ExecuteScope(uint32_t physical, uint32_t dwords, NativeScopeResult* result,
                            const NativeRegisterWrite* writes, uint32_t write_count) = 0;

  // Runs kicked commands on the calling thread until the ring is idle, a wait
  // isn't met yet or the swap belongs to another thread. Never blocks on a
  // guest condition; resumes where the last drain stopped.
  virtual NativeDrainState TryDrain() = 0;

  // Sleeps until something that may let a drain continue happened (a vblank, a
  // kick, another drain's progress) since this thread's last TryDrain, or the
  // timeout.
  virtual void WaitForRingProgress(uint32_t timeout_us) = 0;

  // A vblank came since this thread's last TryDrain: a wait in the ring may
  // be met now (a cheap check for threads that drain at API call boundaries).
  virtual bool VblankSinceLastDrain() = 0;

  // The guest thread that runs Swap: XE_SWAP (and the present) runs there.
  virtual void SetSwapOwnerThread(uint32_t guest_thread_id) = 0;

  virtual void GetStats(NativeGpuStats* out, bool reset) = 0;
};

}  // namespace rex::system
