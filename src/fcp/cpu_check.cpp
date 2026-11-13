// Host ISA check for builds with FCP_MARCH=x86-64-v3. This file is compiled
// for x86-64-v2 (cmake/fcp_boot.cmake) and runs in the compiler initializer segment, before the initializers of the
// rest of the EXE: a CPU without AVX2/FMA/BMI/MOVBE (or an OS without AVX state) gets a message instead of an
// illegal instruction somewhere. Best effort: the SDK DLLs initialize earlier and are built for the same level.
#if defined(FCP_MARCH_V3)

#include <intrin.h>
#include <windows.h>

namespace {

bool HostHasV3() {
  int r[4];
  __cpuid(r, 0);
  if (r[0] < 7) return false;
  __cpuid(r, 1);
  const unsigned ecx1 = unsigned(r[2]);
  const bool fma = ecx1 & (1u << 12), movbe = ecx1 & (1u << 22), osxsave = ecx1 & (1u << 27),
             avx = ecx1 & (1u << 28), f16c = ecx1 & (1u << 29);
  __cpuidex(r, 7, 0);
  const unsigned ebx7 = unsigned(r[1]);
  const bool bmi1 = ebx7 & (1u << 3), avx2 = ebx7 & (1u << 5), bmi2 = ebx7 & (1u << 8);
  __cpuid(r, int(0x80000001u));
  const bool lzcnt = unsigned(r[2]) & (1u << 5);
  if (!(fma && movbe && osxsave && avx && f16c && bmi1 && avx2 && bmi2 && lzcnt)) return false;
  return (_xgetbv(0) & 6) == 6;  // the OS saves XMM and YMM state
}

int Check() {
  if (HostHasV3()) return 0;
  if (!GetEnvironmentVariableA("FCP_NO_DIALOGS", nullptr, 0)) {
    MessageBoxW(nullptr,
                L"This build needs a CPU with AVX2, FMA, BMI2 and MOVBE (x86-64-v3: AMD Zen / Intel Haswell or "
                L"newer). Build with -DFCP_MARCH=x86-64-v2 for older CPUs.",
                L"predator", MB_OK | MB_ICONERROR);
  }
  ExitProcess(3);
}

#pragma warning(disable : 4073)
#pragma init_seg(compiler)
[[maybe_unused]] const int g_cpu_check = Check();

}  // namespace

#endif
