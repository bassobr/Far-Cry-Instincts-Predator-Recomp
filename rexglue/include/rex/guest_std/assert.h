// FCP guest_std (rexglue/UPSTREAM.md, change 140): freestanding replacement, see README.md.
#pragma once
#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : __builtin_trap())
#endif
