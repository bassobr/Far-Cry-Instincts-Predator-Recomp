#include "kernel/apu.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>

#include <rex/platform/perf.h>
#include <rex/logging/log_category.h>
#include <rex/apu/audio_driver.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/platform/sync.h>

#include "kernel/thread_wait.h"
#include "kernel/xma/decoder.h"
#include "kernel/xma/fcp_xma_stats.h"  // FCP: changes 31, 32
#include <rex/platform/process.h>
#include "kernel/title_state.h"

namespace rex::kernel {

namespace {

constexpr std::chrono::milliseconds kWorkerShutdownTimeout{500};
constexpr u32 kDefaultQueuedFrames = 8;

}  // namespace

APU::APU(TitleRuntime& runtime, rex::apu::IAudioSystem* audio)
    : runtime_(runtime),
      audio_(audio),
      queued_frames_(audio ? audio->GetSettings().max_queued_frames.Get() : kDefaultQueuedFrames) {
  for (u32 i = 0; i < kMaxClients; ++i) {
    semaphores_[i] = platform::Semaphore::Create(0, static_cast<i32>(queued_frames_));
    wait_handles_[i] = semaphores_[i].get();
  }
  shutdown_event_ = platform::Event::CreateAutoResetEvent(false);
  wait_handles_[kMaxClients] = shutdown_event_.get();
  xma_decoder_ = std::make_unique<XMADecoder>(StateOf(runtime).GetFunctionDispatcher());
  resume_event_ = platform::Event::CreateAutoResetEvent(false);
  // FCP: changes 31 and 32 (UPSTREAM.md).
  fcp_unregister_wait_ =
      rex::platform::GetEnvironmentValue("FCP_AUDIO_UNREGISTER_NOWAIT").value_or("") != "1";
  fcp_stats_ = rex::platform::GetEnvironmentValue("FCP_AUDIO_STATS").value_or("") != "0";
  // Test only: widens the window between taking the callback and running it, to provoke the race of
  // change 32 with an unregister.
  fcp_test_callback_delay_ms_ = static_cast<u32>(std::strtoul(
      rex::platform::GetEnvironmentValue("FCP_TEST_AUDIO_CALLBACK_DELAY_MS").value_or("0").c_str(),
      nullptr, 10));
}

APU::~APU() {
  Shutdown();
}

Result<void> APU::Setup() {
  if (X_STATUS status = xma_decoder_->Setup(); status != X_STATUS_SUCCESS) {
    return Err(StatusError(status));
  }
  worker_running_ = true;
  auto thread = std::make_unique<KernelThread>(runtime_, 128 * 1024, 0, [this]() {
    WorkerMain();
    return 0;
  });
  thread->SetName("Audio Worker");
  auto launched = runtime_.GetThreadRegistry().Launch(std::move(thread));
  if (!launched) {
    worker_running_ = false;
    return std::unexpected(launched.error());
  }
  worker_thread_ = *launched;
  return Ok();
}

void APU::FcpLogStats(bool final) {
  // FCP: change 31, counters of the XMA decoder and the render clients.
  const FcpXmaStats& x = GetFcpXmaStats();
  REXAPU_INFO(
      "fcp xma{}: contexts created={} released={} live={} live_max={} kicks={} works={} "
      "frames decoded={} skipped={} decode_errors={} stream_errors={} loops={} output_blocks={} "
      "codec_opens={} (rate idx 24k/32k/44.1k/48k={}/{}/{}/{}, mono/stereo={}/{}) | clients "
      "registered={} unregistered={} callbacks={} callbacks_overlapping_unregister={} "
      "unregister_waits={}",
      final ? " (final)" : "", x.contexts_created.load(), x.contexts_released.load(),
      x.contexts_live.load(), x.contexts_live_max.load(), x.kicks.load(), x.works.load(),
      x.frames_decoded.load(), x.frames_skipped.load(), x.decode_errors.load(),
      x.stream_errors.load(), x.loops.load(), x.output_blocks.load(), x.codec_opens.load(),
      x.rate_index[0].load(), x.rate_index[1].load(), x.rate_index[2].load(),
      x.rate_index[3].load(), x.channels[0].load(), x.channels[1].load(),
      x.clients_registered.load(), x.clients_unregistered.load(), x.client_callbacks.load(),
      x.callbacks_after_unregister.load(), x.unregister_waits.load());
}

void APU::WorkerMain() {
  fcp_worker_id_ = std::this_thread::get_id();  // FCP: change 32
  fcp_stats_next_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (worker_running_) {
    if (fcp_stats_ && std::chrono::steady_clock::now() >= fcp_stats_next_) {  // FCP: change 31
      fcp_stats_next_ += std::chrono::seconds(10);
      FcpLogStats(false);
    }
    auto result = platform::WaitAny(wait_handles_.data(), wait_handles_.size(), true,
                                    std::chrono::milliseconds(500));
    if (result.first == platform::WaitResult::kFailed) {
      REXAPU_WARN("Audio worker: WaitAny failed");
      continue;
    }

    if (result.first == platform::WaitResult::kSuccess && result.second == kMaxClients) {
      if (paused_) {
        pause_fence_.Signal();
        platform::Wait(resume_event_.get(), false);
      }
      continue;
    }

    bool pumped = false;
    if (result.first == platform::WaitResult::kSuccess) {
      const auto index = result.second;
      u32 client_callback;
      u32 client_callback_arg;
      bool fcp_was_in_use = false;
      {
        std::lock_guard lock(mutex_);
        client_callback = clients_[index].callback;
        client_callback_arg = clients_[index].wrapped_callback_arg;
        fcp_was_in_use = clients_[index].in_use;
        if (client_callback) {
          fcp_active_callback_ = index;  // FCP: change 32
        }
      }
      if (client_callback) {
        REX_PROFILE_SCOPE_TEXT("apu", "rex::kernel::APU->client_callback");
        if (fcp_test_callback_delay_ms_) {
          platform::SleepFor(std::chrono::milliseconds(fcp_test_callback_delay_ms_));
        }
        u64 args[] = {client_callback_arg};
        StateOf(runtime_).GetFunctionDispatcher()->Execute(
            KernelThread::GetCallingThread()->GetThreadState(), client_callback, args,
            std::size(args));
        GetFcpXmaStats().client_callbacks.fetch_add(1, std::memory_order_relaxed);
        {
          // FCP: change 32, the callback has returned; an UnregisterClient may wait for this.
          std::lock_guard lock(mutex_);
          if (fcp_was_in_use && !clients_[index].in_use) {
            GetFcpXmaStats().callbacks_after_unregister.fetch_add(1, std::memory_order_relaxed);
          }
          fcp_active_callback_ = kMaxClients;
        }
        fcp_callback_done_.notify_all();
      }
      pumped = true;
    }

    if (!worker_running_) {
      break;
    }

    if (!pumped) {
      REX_PROFILE_SCOPE_TEXT("apu", "Sleep");
      platform::SleepFor(std::chrono::milliseconds(500));
    }
  }
  worker_running_ = false;
}

void APU::StopWorker() {
  {
    std::lock_guard pause_lock(pause_mutex_);
    if (worker_running_) {
      xma_decoder_->Shutdown();
      worker_running_ = false;
      shutdown_event_->Set();
    }
    paused_ = false;
    if (parked_) {
      parked_ = false;
      resume_event_->Set();
    }
  }
  if (worker_thread_) {
    {
      if (ThreadPin pin = runtime_.GetThreadRegistry().FindByHandle(worker_thread_)) {
        pin->RequestStop();
        std::array<ThreadPin, 1> threads{std::move(pin)};
        WaitForThreadsToStop(threads, kWorkerShutdownTimeout, [](const ThreadPin&) {
          REXAPU_WARN("Audio worker still running after {} ms", kWorkerShutdownTimeout.count());
        });
      }
    }
    CloseOwned(runtime_.GetObjectTable(), worker_thread_);
    worker_thread_ = 0;
  }
}

void APU::Shutdown() {
  StopWorker();
  if (fcp_stats_ && fcp_worker_id_ != std::thread::id{}) {
    FcpLogStats(true);  // FCP: change 31
    fcp_worker_id_ = {};
  }
  std::lock_guard lock(mutex_);
  for (Client& client : clients_) {
    if (!client.in_use) {
      continue;
    }
    audio_->CloseStream(client.stream);
    client = {};
  }
  // FCP: change 32, the wrapped arguments live per slot until here.
  for (u32& arg : fcp_slot_arg_) {
    if (arg) {
      StateOf(runtime_).GetAddressSpace()->SystemHeapFree(arg);
      arg = 0;
    }
  }
}

Result<u32> APU::RegisterClient(u32 callback, u32 callback_arg) {
  if (!audio_) {
    REXAPU_ERROR("the program has no audio system, a render client is refused");
    return Err<u32>(StatusError(X_STATUS_UNSUCCESSFUL));
  }
  std::lock_guard lock(mutex_);
  auto free = std::ranges::find_if(clients_, [](const Client& c) { return !c.in_use; });
  if (free == clients_.end()) {
    return Err<u32>(StatusError(X_STATUS_NO_MEMORY));
  }
  const u32 index = static_cast<u32>(free - clients_.begin());
  platform::Semaphore* semaphore = semaphores_[index].get();
  auto stream = audio_->OpenStream([semaphore] { semaphore->Release(1, nullptr); });
  if (!stream) {
    REXAPU_ERROR("the audio system refused a stream, {}", stream.error().message);
    return Err<u32>(StatusError(X_STATUS_UNSUCCESSFUL));
  }
  semaphore->Release(static_cast<i32>(queued_frames_), nullptr);
  // FCP: change 32, one wrapped argument per slot, kept across registrations, so a callback that
  // overlaps an unregister never reads freed memory.
  u32& ptr = fcp_slot_arg_[index];
  if (!ptr) {
    ptr = StateOf(runtime_).GetAddressSpace()->SystemHeapAlloc(0x4);
  }
  *StateOf(runtime_).GetAddressSpace()->TranslateVirtual<be_u32*>(ptr) = callback_arg;
  *free = {*stream, callback, callback_arg, ptr, true};
  GetFcpXmaStats().clients_registered.fetch_add(1, std::memory_order_relaxed);
  REXAPU_INFO("audio client {} registered: callback {:08X} arg {:08X}", index, callback,
              callback_arg);  // FCP: change 31
  return index;
}

void APU::UnregisterClient(u32 index) {
  REX_PROFILE_SCOPE("apu");
  std::unique_lock lock(mutex_);
  if (index >= kMaxClients || !clients_[index].in_use) {
    return;
  }
  audio_->CloseStream(clients_[index].stream);
  clients_[index] = {};
  GetFcpXmaStats().clients_unregistered.fetch_add(1, std::memory_order_relaxed);
  REXAPU_INFO("audio client {} unregistered", index);  // FCP: change 31
  // FCP: change 32. Upstream returned while the worker could still be inside this client's guest
  // callback, so the title freed its voices under a running callback. Wait until it returns
  // (bounded, so a callback that waits for a lock the caller holds cannot deadlock). Not on the
  // worker itself (a callback that unregisters its own client).
  if (fcp_unregister_wait_ && fcp_active_callback_ == index &&
      std::this_thread::get_id() != fcp_worker_id_) {
    GetFcpXmaStats().unregister_waits.fetch_add(1, std::memory_order_relaxed);
    if (!fcp_callback_done_.wait_for(lock, std::chrono::milliseconds(250),
                                     [&] { return fcp_active_callback_ != index; })) {
      REXAPU_WARN("audio client {}: callback still running 250 ms after unregister", index);
    }
  }
  platform::Semaphore* semaphore = semaphores_[index].get();
  while (platform::Wait(semaphore, false, std::chrono::milliseconds(0)) ==
         platform::WaitResult::kSuccess) {}
}

void APU::SubmitFrame(u32 index, u32 samples_ptr) {
  REX_PROFILE_SCOPE("apu");
  std::lock_guard lock(mutex_);
  if (index >= kMaxClients || !clients_[index].in_use) {
    return;
  }
  const auto* samples =
      StateOf(runtime_).GetAddressSpace()->TranslateVirtual<const be_f32*>(samples_ptr);
  std::array<f32, rex::apu::kAudioFrameSamples> frame;
  for (size_t i = 0; i < frame.size(); ++i) {
    frame[i] = samples[i];
  }
  if (auto submitted = audio_->Submit(clients_[index].stream, frame);
      !submitted && !clients_[index].refused_logged) {
    clients_[index].refused_logged = true;
    REXAPU_WARN("the audio system refused a frame for client {}, {}", index,
                submitted.error().message);
  }
}

u32 APU::GetOutputChannels() const {
  return audio_ ? audio_->GetOutputChannels() : 2;
}

void APU::SetPaused(bool paused) {
  std::lock_guard pause_lock(pause_mutex_);
  if (paused_ == paused) {
    return;
  }
  paused_ = paused;
  if (paused) {
    if (worker_running_) {
      shutdown_event_->Set();
      pause_fence_.Wait();
      parked_ = true;
    }
    xma_decoder_->Pause();
  } else {
    if (parked_) {
      parked_ = false;
      resume_event_->Set();
    }
    xma_decoder_->Resume();
  }
}

u32 APU::CountCredits(u32 index) {
  platform::Semaphore* semaphore = semaphores_[index].get();
  u32 count = 0;
  while (platform::Wait(semaphore, false, std::chrono::milliseconds(0)) ==
         platform::WaitResult::kSuccess) {
    ++count;
  }
  if (count > 0) {
    semaphore->Release(static_cast<i32>(count), nullptr);
  }
  return count;
}

bool APU::TakeCredit(u32 index) {
  return platform::Wait(semaphores_[index].get(), false, std::chrono::milliseconds(0)) ==
         platform::WaitResult::kSuccess;
}

}  // namespace rex::kernel
