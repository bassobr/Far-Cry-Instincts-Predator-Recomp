// Sampling profile of the guest threads: where the frame time goes.
//
//   FCP_PROFILE=<ms>          sample every guest thread every ms milliseconds (off by default; 2 is a good value)
//   FCP_PROFILE_WINDOW=<s>    one report per window (default 10 s)
//   FCP_PROFILE_TOP=<n>       entries in the guest function lists (default 6)
//   FCP_PROFILE_DEPTH=<n>     stack frames walked per sample (default and maximum 96)
//
// Guest function view: each sample walks the host stack and maps every frame
// to its guest function through kPPCFuncMappings ("guest self" = innermost guest function, including the time in
// the imports, hooks and system calls it called; "guest incl" = every guest function on the stack once; "guest
// edge" = innermost function < its guest caller).
//
// Per window and guest thread with CPU use: CPU share of the wall time (GetThreadTimes), samples by where the
// host instruction pointer was (predator.exe = recompiled guest code plus hooks and renderer, rex*.dll = SDK
// kernel/runtime, system = ntdll/kernel32/win32u, i.e. waits and system calls, driver = GPU driver), the most
// frequent guest return addresses (ctx.lr) and host functions (module+offset of the unwind entry). The sampler
// suspends a thread only to read its context; nothing is allocated while a thread is suspended.
#pragma once

namespace rex::kernel {
class TitleRuntime;
}

namespace fcp::profile {

/** Starts the sampler thread when FCP_PROFILE is set. */
void Start(rex::kernel::TitleRuntime& runtime);

/** Stops it (before the runtime goes away). */
void Stop();

}  // namespace fcp::profile
