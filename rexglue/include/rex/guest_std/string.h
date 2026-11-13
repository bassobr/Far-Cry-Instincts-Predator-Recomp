// FCP guest_std (rexglue/UPSTREAM.md, change 140): freestanding replacement, see README.md.
// The functions link against vcruntime140.dll / ucrtbase.dll; clang treats them as builtins.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* __cdecl memcpy(void* dst, const void* src, size_t n);
void* __cdecl memmove(void* dst, const void* src, size_t n);
void* __cdecl memset(void* dst, int c, size_t n);
int __cdecl memcmp(const void* a, const void* b, size_t n);
const void* __cdecl memchr(const void* p, int c, size_t n);
size_t __cdecl strlen(const char* s);
int __cdecl strcmp(const char* a, const char* b);
int __cdecl strncmp(const char* a, const char* b, size_t n);
#ifdef __cplusplus
}
#endif
