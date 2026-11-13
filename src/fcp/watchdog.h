// Watchdog, run time limit and periodic statistics (one host thread, started at title launch).
//  - 5 s without D3DDevice_Swap: host and guest stacks of all threads into the log, at most 3 times
//    (FCP_WATCHDOG=0 off, FCP_WATCHDOG_SECONDS=<s>).
//  - FCP_EXIT_AFTER=<s>: requests a clean shutdown after s seconds of title run time; if the process
//    is still alive 15 s later it terminates itself (after draining the log).
//  - every 5 s a "stat" line: swaps, guest threads, time sources (FCP_STATS=0 off).
#pragma once

#include <cstdint>

namespace rex::kernel {
class TitleRuntime;
}

namespace fcp::watch {

/** Starts the thread. The runtime must outlive Stop. */
void Start(rex::kernel::TitleRuntime& runtime);

/** Stops and joins the thread. */
void Stop();

/** Called by the D3DDevice_Swap hook after the original returned. */
void OnSwap();

/** Swaps so far. */
uint64_t SwapCount();

/** Guest memory base, recorded from the first hook or import call. */
void SetGuestBase(uint8_t* base);
uint8_t* GuestBase();

/** Writes host and guest stacks of every thread into the log now. */
void DumpAllThreads(const char* reason);

}  // namespace fcp::watch
