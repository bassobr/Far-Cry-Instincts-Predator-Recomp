/**
 * @file        gpu_progress.h
 *
 * @license     BSD 3-Clause License (FCP local change 80, see UPSTREAM.md)
 *
 * @remarks     FCP: a progress counter of the command processor, so a title-side wait loop (the D3D
 *              library polls the read-pointer write-back and fence words in guest memory) can sleep until
 *              the command processor did something instead of spinning a host core.
 */

#pragma once

#include <rex/platform.h>
#include <rex/types.h>

namespace rex {

/** Current value of the counter (acquire). */
REX_API u32 GpuProgress();

/**
 * Called by the command processor after it wrote guest-visible state (read-pointer write-back, end of
 * a batch) and before it sleeps (WAIT_REG_MEM, idle). Bumps the counter and wakes the waiters; costs an
 * atomic increment when nobody waits.
 */
REX_API void NotifyGpuProgress();

/**
 * Sleeps until the counter differs from seen or timeout_ms passed. Returns true when it changed.
 */
REX_API bool WaitGpuProgress(u32 seen, u32 timeout_ms);

}  // namespace rex
