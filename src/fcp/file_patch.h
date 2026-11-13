// FCP_TEST_PATCH=<old>|<new>: equal length text replacement in file data the game reads (scenario
// setup without a debugger: move a spawn, bInvulnerable ...), so tests can set up scenarios without game progress. Off by default.
//
// Where: the archives (.dat) store most entries LZMA compressed (tools/xfat.py), so the replacement runs on
// the output of the game's archive decompressor (Archive_Decompress). With
// FCP_TEST_PATCH_RAW=1 it also runs on every NtReadFile buffer (loose files and stored archive entries; a
// match inside compressed bytes would corrupt them, hence opt-in).
// More patches: FCP_TEST_PATCH2 ... FCP_TEST_PATCH9.
#pragma once

#include <cstdint>

namespace fcp::file_patch {

/** Reads the switches (title launch). */
void Configure();

/** True when raw reads are patched too (the import wrapper asks before it records NtReadFile). */
bool PatchRawReads();

/** After NtReadFile returned: buffer and bytes read. */
void OnRead(uint8_t* base, uint32_t buffer, uint32_t size);

/** Log a summary at shutdown. */
void WriteSummary();

}  // namespace fcp::file_patch
