// Import trace: every kernel/XAM import the title calls passes through a generated wrapper
// (tools/boot/gen_import_thunks.py) that counts the call and logs the first one. FCP_TRACE_IMPORTS=1
// logs the first call of each import with arguments and result; =2 logs every call (very noisy).
// The table of counts is written at shutdown regardless of the switch.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <rex/ppc/context.h>

namespace fcp::imports {

enum class Kind : uint8_t { kImplemented, kStub };

struct Entry {
  const char* name;    // export name without __imp__
  const char* module;  // xboxkrnl / xam
  Kind kind;           // kStub: the SDK defines it with REX_EXPORT_STUB*
};

/** Generated: one entry per __imp__ export of rexkernel (every kernel/XAM function of the SDK). */
extern const Entry kTable[];
extern const size_t kCount;

/** The wrappers predator_code.dll imports (its import table, "__imp__<name>", sorted); the summary lists those. */
void MarkImported(const std::vector<std::string>& names);

/** Resolves the SDK implementation (__imp__<name> exported by the kernel DLL). Fatal when missing. */
PPCFunc* Resolve(size_t index);

/** Before the call. Returns true when this call must be described after it returns. */
bool Enter(size_t index, PPCContext& ctx, uint8_t* base);

/** After the call (only when Enter returned true). */
void Leave(size_t index, PPCContext& ctx, uint8_t* base);

/** Reads FCP_TRACE_IMPORTS / FCP_TRACE_FILES once. */
void Configure();

/** Writes the table of calls (count, stub) into the log. */
void WriteSummary();

/** Number of calls of an import so far (for the watchdog and tests), by name. */
uint64_t CountOf(const char* name);

// --- Time inside imports per thread (diagnosis of frame-time spikes). On when FCP_TRACE_HITCHES or
// FCP_TRACE_FRAMES is set (FCP_TRACE_IMPORT_TIME=0 keeps it off): every import call is timed with the TSC
// around the SDK function, per calling thread, by category. Costs two rdtsc per call (~1 % CPU in 21_dunea).
enum Category : int { kCatWait, kCatFile, kCatMemory, kCatAudio, kCatSync, kCatVideo, kCatOther, kCatCount };

struct ThreadFrame {
  double total_ms = 0;                 // time inside any import
  double cat_ms[kCatCount] = {};       // by category
  uint32_t calls = 0;                  // timed calls
  const char* longest = "";            // the import with the longest single call
  double longest_ms = 0;
  uint32_t longest_lr = 0;             // its guest caller (lr at entry)
  uint32_t longest_r3 = 0;             // its first argument (handle of a wait)
  uint32_t longest_owner = 0;          // RtlEnterCriticalSection: owner (KTHREAD) at entry
  std::string top;                     // up to four imports with the most time: "name count/ms"
};

/** True when import calls are timed. */
bool Timing();

/** Import time of the calling thread since its previous call of this function (then resets it). */
ThreadFrame TakeThreadFrame(bool with_top);

/** Converts TSC ticks to milliseconds (calibrated against QueryPerformanceCounter). */
double TicksToMs(uint64_t ticks);

/** Category names for logs: wait, file, mem, audio, sync, video, other. */
const char* CategoryName(int category);

}  // namespace fcp::imports
