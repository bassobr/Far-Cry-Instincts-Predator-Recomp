// From King Kong Recompiled (github.com/TekRantGaming/king-kong-recompiled, e3e5d3f, native-renderer/backend/cpu_profile.h),
// MIT, Copyright (c) 2026 TekRantGaming and contributors: src/renderer/kk/LICENSE-KingKongRecompiled.txt.
// Changes: src/renderer/kk/CHANGES.md.
#pragma once
#include <cstdint>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <time.h>
#endif

namespace fcp::nr {
// CPU execution time, excluding sleeps and GPU/queue waits. Zero on unsupported hosts.
inline uint64_t CurrentThreadCpuMicroseconds() {
#if defined(_WIN32)
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
  auto ticks = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
  return (ticks(kernel) + ticks(user)) / 10;
#elif defined(__linux__)
  // The calling thread's CPU time (user and system), as GetThreadTimes gives on Windows.
  timespec t{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0) return 0;
  return uint64_t(t.tv_sec) * 1000000u + uint64_t(t.tv_nsec) / 1000u;
#else
  return 0;
#endif
}
}  // namespace fcp::nr
