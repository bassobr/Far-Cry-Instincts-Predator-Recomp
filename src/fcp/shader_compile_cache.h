// Disk cache for the game's own HLSL/assembler compiler (Engine_CompileShader).
//
// The engine compiles some shaders at run time with the Xbox 360 D3DX compiler linked into the EXE (Compile
// or Assemble, always flags 0x200 and the define FC_XENON). The output is a D3DX buffer with the microcode
// and, for Compile with a constant-table out pointer, a D3DX constant table built from the compiler's CTAB
// blob. A hit skips the original and builds the same objects with the game's own functions (D3DX buffer
// creation, constant-table constructor and initializer, names in config/predator/default.toml) from the
// cached bytes, then returns 1 like a successful original. Failures are never cached (the original runs and
// writes its error text as before).
//
// Key: XXH3-128 over mode, entry point, profile, source bytes, include base path and every define pair. The
// include files themselves come from the disc and are not part of the key (one cache per disc image).
//
//   FCP_SHADER_COMPILE_CACHE=0       off (original every time)
//   FCP_SHADER_COMPILE_CACHE=verify  run the original every time and compare it with the cached entry
//                                    (return value, microcode bytes, constant-table blob, the rebuilt
//                                    objects field by field); stores missing entries
//   FCP_SHADER_COMPILE_CACHE=memory  cache only in memory for this process
//   FCP_SHADER_COMPILE_CACHE_DIR=<p> cache folder (default <exe>\cache\hlsl, derived data, never committed)
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace fcp::shadercache {

/** Engine_CompileShader hook body: either serves the result from the cache or runs original. */
void Compile(PPCContext& ctx, uint8_t* base, PPCFunc& original);

/** Counters for the shutdown summary. */
void WriteSummary();

}  // namespace fcp::shadercache
