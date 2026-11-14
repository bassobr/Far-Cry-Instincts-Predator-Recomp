// Interface between the generated test functions and sem_runner.cpp.
#pragma once

#include <cstdint>

struct PPCContext;

#if defined(FCP_SEM_XR)
using FcpSemFn = void (*)(PPCContext&, uint8_t*);
#else
// ReXGlue: the type of every recompiled function (calling convention, rexglue/UPSTREAM.md change 292).
#include <rex/ppc/recompiled_code.h>
using FcpSemFn = PPCFunc*;
#endif

// One recompiled test function. The table ends with {0, nullptr}.
struct FcpSemFunc {
  uint32_t address;
  FcpSemFn fn;
};

extern "C" const FcpSemFunc fcp_sem_funcs[];
