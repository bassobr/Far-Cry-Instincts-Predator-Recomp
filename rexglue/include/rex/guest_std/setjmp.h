// FCP guest_std (rexglue/UPSTREAM.md, change 140): freestanding replacement, see README.md.
// Same layout and names as the MSVC x64 jmp_buf: rex::ppc::GetJumpBuffer (rexppc.dll) returns one.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct __declspec(align(16)) _SETJMP_FLOAT128 {
  unsigned __int64 Part[2];
} SETJMP_FLOAT128;
#define _JBLEN 16
typedef SETJMP_FLOAT128 _JBTYPE;
typedef _JBTYPE jmp_buf[_JBLEN];
int __cdecl _setjmp(jmp_buf buf);
#define setjmp(buf) _setjmp(buf)
#ifdef __cplusplus
}
#endif
