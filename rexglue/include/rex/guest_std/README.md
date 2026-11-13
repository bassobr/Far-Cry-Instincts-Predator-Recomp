# guest_std: freestanding headers for the recompiled code

FCP: local addition of the Far Cry Instincts Predator project (rexglue/UPSTREAM.md, change 140), BSD 3-Clause like
the SDK. Written for this project; no code taken from a C or C++ standard library.

The recompiled game code is compiled on the player's PC (first start) with the clang that ships with the release.
That PC has neither Visual Studio nor the Windows SDK, and their headers may not be redistributed. The generated
code and the SDK headers it includes (`rex/ppc/guest_prelude.h` with `REX_GUEST_FREESTANDING`) need only a small
part of the C and C++ standard libraries. These headers declare exactly that part, mostly on top of clang builtins:

- `clang -nostdlibinc -isystem <this folder>` keeps clang's own headers (`stdint.h`, `intrin.h`, `immintrin.h`, ...)
  and replaces the system ones with these.
- Functions that are not inline (`memcpy`, `memset`, `sqrt`, `nextafterf`, `_setjmp`, ...) link against import
  libraries made from name lists (`vcruntime140.dll`, `ucrtbase.dll`) by the first-start build, the same DLLs the
  executable uses.
- Types that cross into the SDK DLLs keep the layout and the mangled names of the MSVC types (`jmp_buf`,
  `std::basic_string_view`, `std::span`); no std type crosses into predator.exe (src/fcp/game_code_abi.h).

The development build compiles predator_code.dll with the same headers and flags, and the semantic test runs the
same way (cmake/fcp_game_code.cmake), so a deviation of these headers shows up there.
