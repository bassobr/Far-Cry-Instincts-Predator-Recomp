// FCP guest_std (rexglue/UPSTREAM.md, change 140): freestanding replacement, see README.md.
// For clang's mm_malloc.h; the recompiled code never allocates.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* __cdecl _aligned_malloc(size_t size, size_t alignment);
void __cdecl _aligned_free(void* p);
#ifdef __cplusplus
}
#endif
