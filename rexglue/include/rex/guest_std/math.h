// FCP guest_std (rexglue/UPSTREAM.md, change 140): freestanding replacement, see README.md.
// Declarations of the C math functions (ucrtbase.dll), dllimport where the UCRT headers have it. clang lowers most of them to instructions (sqrt, fma with
// FMA, trunc/floor/ceil with SSE4.1, fabs, copysign); the others stay calls into the same DLL the host uses.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
double __cdecl sqrt(double);
__declspec(dllimport) float __cdecl sqrtf(float);
double __cdecl fabs(double);
__declspec(dllimport) float __cdecl fabsf(float);
__declspec(dllimport) double __cdecl fma(double, double, double);
__declspec(dllimport) float __cdecl fmaf(float, float, float);
__declspec(dllimport) double __cdecl trunc(double);
__declspec(dllimport) float __cdecl truncf(float);
double __cdecl floor(double);
__declspec(dllimport) float __cdecl floorf(float);
double __cdecl ceil(double);
__declspec(dllimport) float __cdecl ceilf(float);
__declspec(dllimport) double __cdecl round(double);
__declspec(dllimport) float __cdecl roundf(float);
__declspec(dllimport) double __cdecl rint(double);
__declspec(dllimport) float __cdecl rintf(float);
__declspec(dllimport) double __cdecl nearbyint(double);
__declspec(dllimport) float __cdecl nearbyintf(float);
__declspec(dllimport) long __cdecl lrint(double);
__declspec(dllimport) long __cdecl lrintf(float);
__declspec(dllimport) long long __cdecl llrint(double);
__declspec(dllimport) long long __cdecl llrintf(float);
__declspec(dllimport) double __cdecl copysign(double, double);
__declspec(dllimport) float __cdecl copysignf(float, float);
__declspec(dllimport) double __cdecl nextafter(double, double);
__declspec(dllimport) float __cdecl nextafterf(float, float);
double __cdecl fmod(double, double);
__declspec(dllimport) float __cdecl fmodf(float, float);
double __cdecl ldexp(double, int);
__declspec(dllimport) float __cdecl ldexpf(float, int);
double __cdecl frexp(double, int*);
__declspec(dllimport) float __cdecl frexpf(float, int*);
__declspec(dllimport) double __cdecl exp2(double);
__declspec(dllimport) float __cdecl exp2f(float);
__declspec(dllimport) double __cdecl log2(double);
__declspec(dllimport) float __cdecl log2f(float);
double __cdecl exp(double);
__declspec(dllimport) float __cdecl expf(float);
double __cdecl log(double);
__declspec(dllimport) float __cdecl logf(float);
double __cdecl pow(double, double);
__declspec(dllimport) float __cdecl powf(float, float);
double __cdecl sin(double);
__declspec(dllimport) float __cdecl sinf(float);
double __cdecl cos(double);
__declspec(dllimport) float __cdecl cosf(float);
__declspec(dllimport) double __cdecl fmin(double, double);
__declspec(dllimport) float __cdecl fminf(float, float);
__declspec(dllimport) double __cdecl fmax(double, double);
__declspec(dllimport) float __cdecl fmaxf(float, float);
#ifdef __cplusplus
}
#endif
#define INFINITY __builtin_huge_valf()
#define NAN __builtin_nanf("")
#define HUGE_VAL __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define FP_NAN 2
#define FP_INFINITE 1
#define FP_ZERO 0
#define FP_SUBNORMAL (-2)
#define FP_NORMAL (-1)
