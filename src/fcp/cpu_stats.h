// CPU use of the whole process and of each thread.
//
//   FCP_CPU_STATS=<s>    one line "cpu:" every s seconds (off by default): process CPU time per wall second
//                        (sum over all cores, 100 % = one core), user/kernel split, frames per second,
//                        level; then "cpu:   <thread>" for every thread with at least 2 % of a core
//                        (name from the SDK / SetThreadDescription, guest entry point for guest threads, host thread id,
//                        user/kernel split).
//                        At shutdown "cpu total" over the whole run and over the level part.
//
// Reads GetProcessTimes/GetThreadTimes from its own thread; it never suspends a thread.
#pragma once

namespace rex::kernel {
class TitleRuntime;
}

namespace fcp::cpu {

/** Starts the reporter thread when FCP_CPU_STATS is set. */
void Start(rex::kernel::TitleRuntime& runtime);

/** Stops it and writes the totals. */
void Stop();

}  // namespace fcp::cpu
