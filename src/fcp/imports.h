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

}  // namespace fcp::imports
