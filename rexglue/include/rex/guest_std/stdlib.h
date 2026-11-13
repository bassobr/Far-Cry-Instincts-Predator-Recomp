// FCP guest_std (rexglue/UPSTREAM.md, change 140): freestanding replacement, see README.md.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllimport) void* __cdecl malloc(size_t n);
__declspec(dllimport) void* __cdecl calloc(size_t count, size_t n);
__declspec(dllimport) void* __cdecl realloc(void* p, size_t n);
__declspec(dllimport) void __cdecl free(void* p);
[[noreturn]] __declspec(dllimport) void __cdecl abort(void);
__declspec(dllimport) char* __cdecl getenv(const char* name);
int __cdecl abs(int v);
long long __cdecl llabs(long long v);
#ifdef __cplusplus
}
#endif
