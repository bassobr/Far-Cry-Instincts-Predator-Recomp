// Interface between the generated test functions and sem_runner.cpp.
#pragma once

#include <cstdint>

struct PPCContext;

using FcpSemFn = void (*)(PPCContext&, uint8_t*);

// One recompiled test function. The table ends with {0, nullptr}.
struct FcpSemFunc {
  uint32_t address;
  FcpSemFn fn;
};

extern "C" const FcpSemFunc fcp_sem_funcs[];
