/**
 * @file        graphics/command_processor_native.cpp
 * @brief       Native renderer substrate: guest threads run the command buffer
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     With gpu_native_renderer (latched at startup), the title's hooks
 *              run every rendering packet the moment the D3D call that wrote it
 *              returns (ExecuteScope), on the calling thread, and the guest
 *              threads that kick or wait run what is left in the ring - fences,
 *              interrupts, waits, the swap - themselves (TryDrain). The worker
 *              thread only runs CallInThread jobs and a watchdog. The packets
 *              go through the same handlers as on the emulated path.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/system/xthread.h>

namespace rex::graphics {

using namespace rex::graphics::xenos;

namespace {

// The drain running on this thread (a guest callback it dispatched may kick
// and come back here).
thread_local bool t_native_draining = false;
// The ring progress generation this thread's last drain started from.
thread_local uint64_t t_native_seen_generation = 0;
// Depth of the backend lock on this thread (CallInThread runs jobs inline).
thread_local uint32_t t_native_backend_depth = 0;

constexpr uint64_t kNativeSwapOwnerTimeoutNs = 2'000'000;
constexpr uint64_t kNativeWatchdogNs = 2'000'000;

uint64_t NativeNowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

enum class NativePacketClass {
  kNeutral,    // no effect anywhere (NOPs, INVALIDATE_STATE)
  kRendering,  // render state, shaders, constants, draws, copies: runs at API time
  kRing,       // sync, memory writes, interrupts, indirect buffers, the swap: stays in the ring
};

// Dwords a packet takes, header included.
uint32_t NativePacketDwords(uint32_t header) {
  switch (header >> 30) {
    case 0:
    case 3:
      return ((header >> 16) & 0x3FFF) + 2;
    case 1:
      return 3;
    default:
      return 1;
  }
}

// read(i): dword i of the packet (0 = the header), byte-swapped.
template <typename Read>
NativePacketClass ClassifyNativePacket(uint32_t header, Read read) {
  switch (header >> 30) {
    case 0:
      // Registers below 0x2000 are the CP's own (scratch / write-back,
      // interrupts, coherency, display): sync. Context registers and shader
      // constants are rendering.
      return (header & 0x7FFF) >= 0x2000 ? NativePacketClass::kRendering
                                         : NativePacketClass::kRing;
    case 1:
      // Two registers below 0x800.
      return NativePacketClass::kRing;
    case 2:
      return NativePacketClass::kNeutral;
    default:
      break;
  }
  switch ((header >> 8) & 0x7F) {
    case PM4_NOP:
    case PM4_INVALIDATE_STATE:
      return NativePacketClass::kNeutral;
    case PM4_DRAW_INDX:
    case PM4_DRAW_INDX_2:
    case PM4_VIZ_QUERY:
    case PM4_SET_CONSTANT:
    case PM4_LOAD_ALU_CONSTANT:
    case PM4_IM_LOAD:
    case PM4_IM_LOAD_IMMEDIATE:
    case PM4_EVENT_WRITE:
    case PM4_EVENT_WRITE_EXT:
    case PM4_EVENT_WRITE_ZPD:
    case PM4_SET_BIN_MASK:
    case PM4_SET_BIN_SELECT:
    case PM4_SET_BIN_MASK_LO:
    case PM4_SET_BIN_MASK_HI:
    case PM4_SET_BIN_SELECT_LO:
    case PM4_SET_BIN_SELECT_HI:
      return NativePacketClass::kRendering;
    case PM4_SET_CONSTANT2:
    case PM4_SET_SHADER_CONSTANTS:
      return (read(1) & 0xFFFF) >= 0x2000 ? NativePacketClass::kRendering
                                           : NativePacketClass::kRing;
    default:
      return NativePacketClass::kRing;
  }
}

// An indirect buffer runs at API time when everything in it (nested buffers
// too) is rendering, like the library's prebuilt state blocks and GPR setup.
NativePacketClass ClassifyNativeIndirectBuffer(memory::Memory* memory, uint32_t ptr,
                                               uint32_t length, uint32_t depth) {
  if (!length || depth >= 4) {
    return NativePacketClass::kRing;
  }
  const uint8_t* buffer = memory->TranslatePhysical(ptr);
  auto load = [&](uint32_t i) { return memory::load_and_swap<uint32_t>(buffer + i * 4); };
  uint32_t pos = 0;
  while (pos < length) {
    const uint32_t header = load(pos);
    const uint32_t size = NativePacketDwords(header);
    if (size > length - pos) {
      return NativePacketClass::kRing;
    }
    NativePacketClass packet_class = ClassifyNativePacket(
        header, [&](uint32_t i) { return i < size ? load(pos + i) : uint32_t(0); });
    if ((header >> 30) == 3 && size >= 3 &&
        (((header >> 8) & 0x7F) == PM4_INDIRECT_BUFFER ||
         ((header >> 8) & 0x7F) == PM4_INDIRECT_BUFFER_PFD)) {
      packet_class = ClassifyNativeIndirectBuffer(memory, GpuToCpu(CpuToGpu(load(pos + 1))),
                                                  load(pos + 2) & 0xFFFFF, depth + 1);
    }
    if (packet_class == NativePacketClass::kRing) {
      return NativePacketClass::kRing;
    }
    pos += size;
  }
  return NativePacketClass::kRendering;
}

}  // namespace

bool CommandProcessor::WaitUntilReady() {
  if (!native_mode_) {
    return false;
  }
  std::unique_lock<std::mutex> lock(native_ready_mutex_);
  native_ready_cv_.wait(lock, [this] {
    return native_ready_.load(std::memory_order_acquire) || !worker_running_;
  });
  return native_ready_.load(std::memory_order_acquire);
}

void CommandProcessor::CallInThreadNative(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(native_pending_mutex_);
    if (!pending_fns_.empty() ||
        (!t_native_backend_depth && !system::XThread::IsInThread(worker_thread_.get()))) {
      pending_fns_.push(std::move(fn));
      write_ptr_index_event_->Set();
      return;
    }
  }
  // Already where the backend runs (holding its lock, or on the worker).
  std::lock_guard<std::recursive_mutex> backend_lock(native_backend_mutex_);
  ++t_native_backend_depth;
  fn();
  --t_native_backend_depth;
}

void CommandProcessor::RunNativePendingFns() {
  for (;;) {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> lock(native_pending_mutex_);
      if (pending_fns_.empty()) {
        return;
      }
      fn = std::move(pending_fns_.front());
      pending_fns_.pop();
    }
    std::lock_guard<std::recursive_mutex> backend_lock(native_backend_mutex_);
    ++t_native_backend_depth;
    fn();
    --t_native_backend_depth;
  }
}

void CommandProcessor::NativeWorkerLoop() {
  while (worker_running_) {
    RunNativePendingFns();
    rex::thread::Wait(write_ptr_index_event_.get(), false,
                      std::chrono::milliseconds(int64_t(kNativeWatchdogNs / 1000000)));
    if (!worker_running_) {
      break;
    }
    RunNativePendingFns();
    // Watchdog: commands that were kicked but nobody drained (every guest
    // thread that kicks or waits drains, so this is expected to do nothing).
    uint32_t write_index = write_ptr_index_.load(std::memory_order_relaxed);
    bool runnable = native_ib_depth_ != 0 ||
                    (write_index != 0xBAADF00D && read_ptr_index_ != write_index);
    if (runnable && NativeNowNs() - native_last_drain_ns_.load(std::memory_order_relaxed) >=
                        kNativeWatchdogNs) {
      // Counted by the drain (watchdog_drains when it ran something).
      TryDrain();
    }
  }
}

void CommandProcessor::BumpNativeRingProgress() {
  {
    std::lock_guard<std::mutex> lock(native_progress_mutex_);
    native_progress_generation_.fetch_add(1, std::memory_order_relaxed);
  }
  native_progress_cv_.notify_all();
}

void CommandProcessor::SignalNativeRingProgress() {
  if (native_mode_) {
    BumpNativeRingProgress();
  }
}

void CommandProcessor::WaitForRingProgress(uint32_t timeout_us) {
  if (!native_mode_) {
    return;
  }
  const uint64_t seen = t_native_seen_generation;
  std::unique_lock<std::mutex> lock(native_progress_mutex_);
  native_progress_cv_.wait_for(lock, std::chrono::microseconds(timeout_us), [&] {
    return native_progress_generation_.load(std::memory_order_relaxed) != seen ||
           !worker_running_;
  });
}

void CommandProcessor::SetSwapOwnerThread(uint32_t guest_thread_id) {
  if (!guest_thread_id) {
    guest_thread_id = system::XThread::GetCurrentThreadId();
  }
  native_swap_owner_.store(guest_thread_id, std::memory_order_relaxed);
}

void CommandProcessor::GetStats(system::NativeGpuStats* out, bool reset) {
  std::lock_guard<std::mutex> lock(native_stats_mutex_);
  *out = native_stats_;
  if (reset) {
    native_stats_ = system::NativeGpuStats();
  }
}

bool CommandProcessor::ExecuteScope(uint32_t physical, uint32_t dwords,
                                    system::NativeScopeResult* result) {
  system::NativeScopeResult local_result;
  system::NativeScopeResult& r = result ? *result : local_result;
  r = system::NativeScopeResult();
  if (!native_mode_ || !dwords || !native_ready_.load(std::memory_order_acquire)) {
    return true;
  }
  const uint64_t start_ns = NativeNowNs();
  uint8_t* host = memory_->TranslatePhysical(physical);
  auto load = [&](uint32_t i) { return memory::load_and_swap<uint32_t>(host + i * 4); };

  std::lock_guard<std::recursive_mutex> backend_lock(native_backend_mutex_);
  ++t_native_backend_depth;
  // Rendering packets run in runs (between packets left for the ring); a run
  // that ran becomes NOPs so the ring skips it.
  uint32_t run_start = UINT32_MAX;
  auto flush_run = [&](uint32_t run_end) {
    if (run_start == UINT32_MAX) {
      return;
    }
    uint32_t length = run_end - run_start;
    ExecutePacket(physical + run_start * 4, length);
    uint32_t pos = run_start;
    while (length) {
      if (length == 1) {
        memory::store_and_swap<uint32_t>(host + pos * 4, MakePacketType2());
        break;
      }
      uint32_t count = std::min(length - 1, uint32_t(0x4000));
      memory::store_and_swap<uint32_t>(host + pos * 4,
                                       MakePacketType3(PM4_NOP, uint16_t(count)));
      pos += 1 + count;
      length -= 1 + count;
    }
    run_start = UINT32_MAX;
  };
  uint32_t pos = 0;
  while (pos < dwords) {
    const uint32_t header = load(pos);
    const uint32_t size = NativePacketDwords(header);
    if (size > dwords - pos) {
      // The range doesn't end on a packet boundary: leave the rest to the ring.
      r.ok = false;
      break;
    }
    NativePacketClass packet_class = ClassifyNativePacket(
        header, [&](uint32_t i) { return i < size ? load(pos + i) : uint32_t(0); });
    if ((header >> 30) == 3 && size >= 3 &&
        (((header >> 8) & 0x7F) == PM4_INDIRECT_BUFFER ||
         ((header >> 8) & 0x7F) == PM4_INDIRECT_BUFFER_PFD) &&
        !(header & 1)) {
      packet_class = ClassifyNativeIndirectBuffer(memory_, GpuToCpu(CpuToGpu(load(pos + 1))),
                                                  load(pos + 2) & 0xFFFFF, 0);
    }
    // VdSwap's frontbuffer fetch constant belongs to the XE_SWAP after it.
    if (packet_class == NativePacketClass::kRendering && (header >> 30) == 0 &&
        (header & 0x7FFF) == XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 && size == 7 &&
        pos + size < dwords) {
      const uint32_t next = load(pos + size);
      if ((next >> 30) == 3 && ((next >> 8) & 0x7F) == PM4_XE_SWAP) {
        packet_class = NativePacketClass::kRing;
      }
    }
    switch (packet_class) {
      case NativePacketClass::kRing:
        flush_run(pos);
        if (r.left_packets < 4) {
          r.left_headers[r.left_packets * 2] = header;
          r.left_headers[r.left_packets * 2 + 1] = size > 1 ? load(pos + 1) : 0;
        }
        ++r.left_packets;
        break;
      case NativePacketClass::kRendering:
        if (run_start == UINT32_MAX) {
          run_start = pos;
        }
        ++r.executed_packets;
        r.executed_dwords += size;
        break;
      case NativePacketClass::kNeutral:
        break;
    }
    pos += size;
  }
  flush_run(pos);
  --t_native_backend_depth;

  const uint64_t elapsed_ns = NativeNowNs() - start_ns;
  std::lock_guard<std::mutex> stats_lock(native_stats_mutex_);
  ++native_stats_.scopes;
  native_stats_.scope_packets += r.executed_packets;
  native_stats_.scope_dwords += r.executed_dwords;
  native_stats_.scope_left += r.left_packets;
  native_stats_.scope_ns += elapsed_ns;
  if (!r.ok) {
    ++native_stats_.scope_failures;
  }
  return r.ok;
}

system::NativeDrainState CommandProcessor::TryDrain() {
  if (!native_mode_) {
    return system::NativeDrainState::kIdle;
  }
  // Anything signalled from now on wakes this thread's next WaitForRingProgress.
  t_native_seen_generation = native_progress_generation_.load(std::memory_order_relaxed);
  if (!native_ready_.load(std::memory_order_acquire)) {
    return system::NativeDrainState::kNotReady;
  }
  if (t_native_draining) {
    return system::NativeDrainState::kBusy;
  }
  std::unique_lock<std::mutex> drain_lock(native_drain_mutex_, std::try_to_lock);
  if (!drain_lock.owns_lock()) {
    std::lock_guard<std::mutex> stats_lock(native_stats_mutex_);
    ++native_stats_.drains_busy;
    return system::NativeDrainState::kBusy;
  }
  t_native_draining = true;
  const bool on_worker = system::XThread::IsInThread(worker_thread_.get());
  const uint64_t start_ns = NativeNowNs();
  native_last_drain_ns_.store(start_ns, std::memory_order_relaxed);
  bool advanced = false;
  uint64_t excluded_ns = 0;
  uint64_t packets = 0;
  const system::NativeDrainState state =
      NativeDrain(on_worker, &advanced, &excluded_ns, &packets);
  if (advanced) {
    // Like the end of a primary buffer on the emulated path: submit what the
    // drain and the scopes before it recorded.
    std::lock_guard<std::recursive_mutex> backend_lock(native_backend_mutex_);
    ++t_native_backend_depth;
    OnPrimaryBufferEnd();
    --t_native_backend_depth;
  }
  t_native_draining = false;
  drain_lock.unlock();
  if (advanced) {
    BumpNativeRingProgress();
  }

  const uint64_t elapsed_ns = NativeNowNs() - start_ns;
  const uint64_t net_ns = elapsed_ns > excluded_ns ? elapsed_ns - excluded_ns : 0;
  std::lock_guard<std::mutex> stats_lock(native_stats_mutex_);
  ++native_stats_.drains;
  native_stats_.drain_packets += packets;
  switch (state) {
    case system::NativeDrainState::kIdle:
      ++native_stats_.drains_idle;
      break;
    case system::NativeDrainState::kBlockedOnWait:
      ++native_stats_.drains_blocked_wait;
      break;
    case system::NativeDrainState::kBlockedOnOwner:
      ++native_stats_.drains_blocked_owner;
      break;
    default:
      break;
  }
  if (on_worker && advanced) {
    ++native_stats_.watchdog_drains;
  }
  native_stats_.drain_ns += net_ns;
  native_stats_.drain_max_ns = std::max(native_stats_.drain_max_ns, net_ns);
  ++native_stats_.drain_histogram[std::min(
      uint64_t(system::NativeGpuStats::kDrainBuckets - 1),
      net_ns / system::NativeGpuStats::kDrainBucketNs)];
  return state;
}

void CommandProcessor::NativeDispatchInterrupt(uint32_t cpu_mask, bool on_worker) {
  if (!graphics_system_) {
    return;
  }
  for (uint32_t n = 0; n < 6; ++n) {
    if (!(cpu_mask & (uint32_t(1) << n))) {
      continue;
    }
    if (on_worker) {
      graphics_system_->DispatchInterruptCallback(1, n);
    } else {
      graphics_system_->DispatchInterruptOnGuestThread(1, n);
    }
    std::lock_guard<std::mutex> stats_lock(native_stats_mutex_);
    ++(on_worker ? native_stats_.interrupts_worker : native_stats_.interrupts);
  }
}

system::NativeDrainState CommandProcessor::NativeDrain(bool on_worker, bool* advanced,
                                                       uint64_t* excluded_ns,
                                                       uint64_t* packets) {
  // Resumable: the primary ring's read pointer and a stack of indirect buffers
  // (BL nests primary -> segment -> GPR setup) persist between drains, so a
  // drain stopped before a wait goes on from exactly there, on any thread.
  for (;;) {
    const bool primary = native_ib_depth_ == 0;
    uint8_t* buffer;
    uint32_t pos, size_dwords, write_index = 0;
    if (primary) {
      write_index = write_ptr_index_.load(std::memory_order_acquire);
      if (write_index == 0xBAADF00D || read_ptr_index_ == write_index) {
        return system::NativeDrainState::kIdle;
      }
      buffer = memory_->TranslatePhysical(primary_buffer_ptr_);
      pos = read_ptr_index_;
      size_dwords = primary_buffer_size_ / sizeof(uint32_t);
    } else {
      NativeIbLevel& level = native_ib_stack_[native_ib_depth_ - 1];
      if (level.pos >= level.size) {
        --native_ib_depth_;
        continue;
      }
      buffer = memory_->TranslatePhysical(level.ptr);
      pos = level.pos;
      size_dwords = level.size;
    }
    auto read = [&](uint32_t i) {
      return memory::load_and_swap<uint32_t>(buffer + ((pos + i) % size_dwords) * 4);
    };
    const uint32_t header = read(0);
    const uint32_t packet_dwords = NativePacketDwords(header);
    auto advance = [&]() {
      if (primary) {
        read_ptr_index_ = (pos + packet_dwords) % size_dwords;
        if (read_ptr_writeback_ptr_) {
          memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_),
                                           read_ptr_index_);
        }
      } else {
        native_ib_stack_[native_ib_depth_ - 1].pos = pos + packet_dwords;
      }
      *advanced = true;
    };

    bool is_swap = false;
    if ((header >> 30) == 3) {
      const uint32_t opcode = (header >> 8) & 0x7F;
      const bool predicated_out =
          (header & 1) && (!(bin_select_ & bin_mask_) || opcode == PM4_XE_SWAP);
      if (!predicated_out) {
        switch (opcode) {
          case PM4_INDIRECT_BUFFER:
          case PM4_INDIRECT_BUFFER_PFD: {
            const uint32_t list_ptr = CpuToGpu(read(1));
            const uint32_t list_length = read(2) & 0xFFFFF;
            advance();
            if (!list_length) {
              continue;
            }
            if (native_ib_depth_ >= kNativeIbStackSize) {
              // Deeper than the library ever nests: run it in one go.
              std::lock_guard<std::recursive_mutex> backend_lock(native_backend_mutex_);
              ++t_native_backend_depth;
              ExecuteIndirectBuffer(GpuToCpu(list_ptr), list_length);
              --t_native_backend_depth;
              continue;
            }
            native_ib_stack_[native_ib_depth_++] = {GpuToCpu(list_ptr), list_length, 0};
            continue;
          }
          case PM4_WAIT_REG_MEM: {
            // Never sleeps here: an unmet wait stops the drain before the
            // packet, and the next drain checks it again.
            uint32_t value = 0;
            bool matched;
            {
              std::lock_guard<std::recursive_mutex> backend_lock(native_backend_mutex_);
              matched = WaitRegMemMatches(read(1), read(2), read(3), read(4), &value);
            }
            if (!matched) {
              return system::NativeDrainState::kBlockedOnWait;
            }
            advance();
            continue;
          }
          case PM4_INTERRUPT: {
            // On this thread, without the backend lock (guest code runs).
            const uint32_t cpu_mask = read(1);
            advance();
            const uint64_t t0 = NativeNowNs();
            NativeDispatchInterrupt(cpu_mask, on_worker);
            *excluded_ns += NativeNowNs() - t0;
            continue;
          }
          case PM4_XE_SWAP: {
            // The present runs on the swap thread (it always comes back to
            // wait for its frame); anyone else after a timeout.
            const uint32_t owner = native_swap_owner_.load(std::memory_order_relaxed);
            const bool is_owner =
                !on_worker && owner && system::XThread::GetCurrentThreadId() == owner;
            if (owner && !is_owner) {
              const uint64_t now_ns = NativeNowNs();
              if (!native_swap_blocked_since_ns_) {
                native_swap_blocked_since_ns_ = now_ns;
              }
              if (now_ns - native_swap_blocked_since_ns_ < kNativeSwapOwnerTimeoutNs) {
                return system::NativeDrainState::kBlockedOnOwner;
              }
            }
            native_swap_blocked_since_ns_ = 0;
            {
              std::lock_guard<std::mutex> stats_lock(native_stats_mutex_);
              if (is_owner || !owner) {
                ++native_stats_.swaps_owner;
              } else if (on_worker) {
                ++native_stats_.swaps_worker;
              } else {
                ++native_stats_.swaps_foreign;
              }
            }
            is_swap = true;
          } break;
          default:
            break;
        }
      }
    }

    // Everything else: the packet handlers, under the backend lock.
    NativePacketClass packet_class = ClassifyNativePacket(header, read);
    if (packet_class == NativePacketClass::kRendering && (header >> 30) == 0 &&
        (header & 0x7FFF) == XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 && packet_dwords == 7) {
      // VdSwap's frontbuffer fetch constant, with its XE_SWAP: the one
      // rendering write that belongs in the ring (both renderers apply it at
      // the frame boundary).
      const uint32_t next = read(packet_dwords);
      if ((next >> 30) == 3 && ((next >> 8) & 0x7F) == PM4_XE_SWAP) {
        packet_class = NativePacketClass::kRing;
      }
    }
    const uint64_t t0 = is_swap ? NativeNowNs() : 0;
    bool ok;
    {
      std::lock_guard<std::recursive_mutex> backend_lock(native_backend_mutex_);
      ++t_native_backend_depth;
      memory::RingBuffer reader(buffer, size_dwords * sizeof(uint32_t));
      reader.set_write_offset(primary ? write_index * sizeof(uint32_t)
                                      : size_dwords * sizeof(uint32_t));
      reader.set_read_offset(pos * sizeof(uint32_t));
      ok = ExecutePacket(&reader);
      --t_native_backend_depth;
    }
    if (is_swap) {
      *excluded_ns += NativeNowNs() - t0;
    }
    if (packet_class == NativePacketClass::kRendering) {
      // Something rendering waited in the ring (expected only before the
      // title's hooks see their first call).
      std::lock_guard<std::mutex> stats_lock(native_stats_mutex_);
      if (!native_stats_.drain_render_packets++) {
        native_stats_.first_render_packet[0] = header;
        native_stats_.first_render_packet[1] = primary ? primary_buffer_ptr_ + pos * 4
                                                       : native_ib_stack_[native_ib_depth_ - 1].ptr +
                                                             pos * 4;
        native_stats_.first_render_packet[2] = read(1);
        native_stats_.first_render_packet[3] = primary ? 0 : native_ib_depth_;
      }
      static std::atomic<uint32_t> logged{0};
      if (logged.fetch_add(1, std::memory_order_relaxed) < 64) {
        const NativeIbLevel* level = primary ? nullptr : &native_ib_stack_[native_ib_depth_ - 1];
        REXGPU_WARN(
            "Native renderer: rendering packet {:08X} {:08X} in the ring at {:08X} (buffer "
            "{:08X}, {} dwords, at {}, depth {}), thread {}",
            header, read(1), level ? level->ptr + pos * 4 : primary_buffer_ptr_ + pos * 4,
            level ? level->ptr : primary_buffer_ptr_, size_dwords, pos, native_ib_depth_,
            system::XThread::GetCurrentThreadId());
      }
    }
    ++*packets;
    if (!ok) {
      REXGPU_ERROR("Native renderer: failed to execute packet {:08X} in the {} buffer", header,
                   primary ? "primary" : "indirect");
      // Like the emulated path: the rest of this buffer is dropped.
      if (primary) {
        read_ptr_index_ = write_index;
      } else {
        native_ib_stack_[native_ib_depth_ - 1].pos = native_ib_stack_[native_ib_depth_ - 1].size;
      }
      *advanced = true;
      continue;
    }
    advance();
  }
}

}  // namespace rex::graphics
