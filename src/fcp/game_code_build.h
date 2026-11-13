// Builds predator_code.dll, the recompiled game code.
//
// One build path for both cases, so the DLL of the first start is made exactly like the developer's:
//   - development build: CMake runs `predator.exe --fcp-build-game-code <settings.txt>` on the codegen output in
//     generated/default (cmake/fcp_game_code.cmake);
//   - first start: fcp::game_code::Prepare runs codegen (compiler\bin\rexglue.exe) on default.xex from the player's
//     disc image, then the same compile and link, all from the compiler folder next to predator.exe.
//
// Compile: every generated source and src/fcp/game_code/entry.cpp, freestanding (rex/ppc/guest_prelude.h with
// REX_GUEST_FREESTANDING, rexglue/include/rex/guest_std, -nostdlibinc), with the flags of the generated code of the
// development build (-O3, -march=<FCP_MARCH>, -ffp-contract=off, -fasync-exceptions, -g0). The biggest files first,
// as many at a time as there are logical processors and free memory (640 MB each), below normal priority; objects
// are written beside their name and renamed when whole, so a stopped build carries on where it stopped.
// Link: lld-link, no runtime libraries, against import libraries of predator.exe (made from its own export table),
// the SDK DLLs and the C runtime DLLs (made from name lists); exports fcp_game_code and every original
// predator.exe imports through its delay-load table.
//
// Switches: FCP_GAME_CODE_JOBS=<n> compilers at a time; FCP_GAME_CODE_CPUS=<n> the build's processes only on the
// first n logical processors (measuring smaller PCs); FCP_GAME_CODE_KEEP=1 keeps the work folder.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fcp::build {
class Processes;
}

namespace fcp::game_code_build {

namespace fs = std::filesystem;

/** Tools, headers and libraries of a build. */
struct Toolset {
  fs::path clang;     // clang.exe (driven as clang++)
  fs::path lld_link;  // lld-link.exe
  fs::path rexglue;   // rexglue.exe (codegen; first start only)
  fs::path sdk_bin;   // folder of the SDK DLLs rexglue.exe needs (the EXE folder)
  std::vector<fs::path> includes;         // -I (SDK headers, simde, src for fcp/game_code_abi.h)
  std::vector<fs::path> system_includes;  // -isystem (rex/guest_std)
  std::vector<fs::path> libs;             // import libraries (SDK DLLs, C runtime)
  fs::path entry;                         // src/fcp/game_code/entry.cpp (the table of predator_code.dll)
  fs::path runtime;                       // src/fcp/game_code/runtime.cpp (_fltused, __chkstk)
  fs::path title_config;                  // config/predator/default.toml (codegen)
  fs::path names_config;                  // d3d_names.toml (codegen)
};

/** The toolset of a release: everything below <compiler> (see tools/dist.ps1 for the layout). */
Toolset FromCompilerFolder(const fs::path& compiler);

/** One build. */
struct Job {
  fs::path gen;          // generated sources (codegen output)
  fs::path dll;          // ...\predator_code.dll
  fs::path work;         // objects, .def, import library of predator.exe
  fs::path log;          // build.log
  std::string origin;    // "build" or "first-start"
  std::string image_sha256;
  fs::path pgo_profile;  // FCP_PGO=use: merged profile (empty = none)
  bool lto = false;      // ThinLTO across the generated files (FCP_GAME_CODE_LTO): code generation in the link
  // "semtest": the test functions of predator_semtest (every .cpp in gen) into a DLL that exports the table
  // fcp_sem_funcs, with the flags of the game code; no predator.exe, no entry table (cmake/fcp_semtest.cmake).
  std::string kind = "game";
  std::vector<std::string> data_exports;  // semtest: exported data symbols
};

/** Timings of the last build, for the log. */
struct Stats {
  double codegen_s = 0, compile_s = 0, link_s = 0, cpu_s = 0;
  size_t files = 0, compiled = 0, slots = 0;
};

/** What stops a build before it starts (missing tool, folder not writable, disk full); empty when nothing does. */
std::wstring Blocker(const Toolset& tools, const fs::path& out_dir);

/** rexglue codegen of default.xex into <work>\gen (the manifest is written next to it). */
bool Codegen(const Toolset& tools, const fs::path& xex, const fs::path& work, build::Processes& run, Stats& stats,
             std::wstring& why);

/** Compile and link. */
bool CompileAndLink(const Toolset& tools, const Job& job, build::Processes& run, Stats& stats, std::wstring& why);

/** True when the command line asks for `--fcp-build-game-code <settings>`; runs it and sets exit_code. */
bool RunCommand(int& exit_code);

}  // namespace fcp::game_code_build
