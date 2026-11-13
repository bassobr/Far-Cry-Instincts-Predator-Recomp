// The two symbols the C runtime would provide to the freestanding code of predator_code.dll (and of the semantic
// test DLL). Compiled with the generated code by src/fcp/game_code_build.cpp; needs no headers.

// The compiler references _fltused in every object that uses floating point (MSVC convention).
extern "C" int _fltused = 0x9875;

// Stack probe for frames above 4 KiB (the MSVC runtime's __chkstk is in its static libraries, which the DLL does
// not link). RAX = bytes the caller will allocate; touches every page from the caller's stack pointer down so the
// guard page moves in order. Preserves every register but the flags, as the convention requires.
__asm__(
    ".text\n"
    ".globl __chkstk\n"
    ".p2align 4\n"
    "__chkstk:\n"
    "  pushq %rcx\n"
    "  pushq %rax\n"
    "  leaq 24(%rsp), %rcx\n"
    "  cmpq $0x1000, %rax\n"
    "  jb 2f\n"
    "1:\n"
    "  subq $0x1000, %rcx\n"
    "  testq %rcx, (%rcx)\n"
    "  subq $0x1000, %rax\n"
    "  cmpq $0x1000, %rax\n"
    "  ja 1b\n"
    "2:\n"
    "  subq %rax, %rcx\n"
    "  testq %rcx, (%rcx)\n"
    "  popq %rax\n"
    "  popq %rcx\n"
    "  retq\n");
