// Builds predator_code.dll (fcp/game_code_build.h). The compile driver follows src/app/game_code.cpp of
// Far Cry Instincts Recomp (MIT, same author), which adapted it from SCDA-XBERecomp (MIT, TGP482 and Joshhhuaaa).
#include "fcp/game_code_build.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <rex/platform.h>

#include "fcp/build_process.h"
#include "fcp/env.h"
#include "fcp/game_code.h"
#include "fcp/hash.h"
#include "fcp/log.h"
#include "fcp/progress_window.h"
#include "fcp_game_code_abi.h"

#ifndef FCP_GAME_CODE_MARCH
#define FCP_GAME_CODE_MARCH "x86-64-v3"
#endif

namespace fcp::game_code_build {
namespace {

using build::Narrow;
using build::Quote;
using build::Widen;

constexpr uint64_t kJobMemory = 640ull << 20;  // peak working set of one compiler, with room to spare
constexpr uint64_t kDiskNeeded = 3ull << 30;   // codegen output (~190 MB), objects (~150 MB), DLL, with room

bool g_command_mode = false;  // --fcp-build-game-code: messages on stdout as well

void Say(const std::string& line) {
  if (g_command_mode) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
  }
  FCPLOG("{}", line);
}

int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string U8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

std::string Slashes(const fs::path& p) {
  std::string s = U8(p);
  std::replace(s.begin(), s.end(), '\\', '/');
  return s;
}

bool Fail(std::wstring& why, const std::wstring& what) {
  why = what;
  Say("game code: " + Narrow(what));
  return false;
}

// The flags of the generated code, the same in the development build and on the first start. Release flags of
// the former static build (CMake, cmake/fcp_codegen.cmake) plus the freestanding headers.
std::wstring BaseFlags() {
  std::wstring f = L"--driver-mode=g++ --target=x86_64-pc-windows-msvc -fms-compatibility-version=19.44";
  f += L" -std=c++23 -O3 -DNDEBUG -march=" + Widen(FCP_GAME_CODE_MARCH) + L" -ffp-contract=off -msse4.1";
  f += L" -fasync-exceptions -g0 -nostdlibinc -DREX_GUEST_FREESTANDING -DNOMINMAX -DWIN32_LEAN_AND_MEAN";
  f += L" -DREX_PHYSICAL_OFFSET_IN_CODE=" + std::to_wstring(REX_PHYSICAL_OFFSET_IN_CODE);
  return f;
}

std::wstring CompileCommand(const Toolset& t, const Job& job, const fs::path& src, const fs::path& obj) {
  std::wstring c = Quote(t.clang) + L" " + BaseFlags();
  if (!job.pgo_profile.empty()) {
    c += L" \"-fprofile-instr-use=" + job.pgo_profile.wstring() +
         L"\" -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-profile-instr-missing";
  }
  // ThinLTO: the objects are bitcode, the link optimizes across files (calls between guest functions of
  // different files can be inlined) and generates the code in parallel.
  if (job.lto) c += L" -flto=thin";
  for (const fs::path& inc : t.system_includes) c += L" -isystem " + Quote(inc);
  c += L" -I" + Quote(job.gen) + L" -I" + Quote(job.work);
  for (const fs::path& inc : t.includes) c += L" -I" + Quote(inc);
  return c + L" -c " + Quote(src) + L" -o " + Quote(obj);
}

size_t Slots(size_t files) {
  MEMORYSTATUSEX mem{sizeof(mem)};
  GlobalMemoryStatusEx(&mem);
  const size_t by_memory = size_t(mem.ullAvailPhys / kJobMemory);
  size_t by_cpu = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  if (const int64_t cpus = fcp::env::Int("FCP_GAME_CODE_CPUS", 0); cpus > 0) by_cpu = std::min<size_t>(by_cpu, cpus);
  if (const int64_t jobs = fcp::env::Int("FCP_GAME_CODE_JOBS", 0); jobs > 0) {
    return std::clamp<size_t>(size_t(jobs), 1, MAXIMUM_WAIT_OBJECTS);
  }
  return std::clamp<size_t>(std::min({by_memory, by_cpu, std::max<size_t>(files, 1)}), 1, MAXIMUM_WAIT_OBJECTS);
}

struct Source {
  fs::path src, obj;
  uint64_t bytes = 0;
  std::string key;  // compile inputs; the object is current when its .key holds this
};

std::string ReadAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

// Sections of a PE file, for the checks after the link.
std::vector<std::string> SectionNames(const std::string& image) {
  std::vector<std::string> out;
  if (image.size() < 0x40) return out;
  uint32_t nt;
  std::memcpy(&nt, image.data() + 0x3C, 4);
  if (nt + 24 > image.size()) return out;
  uint16_t count, optional_size;
  std::memcpy(&count, image.data() + nt + 6, 2);
  std::memcpy(&optional_size, image.data() + nt + 20, 2);
  size_t at = nt + 24 + optional_size;
  for (uint16_t i = 0; i < count && at + 40 <= image.size(); ++i, at += 40) {
    out.emplace_back(image.data() + at, strnlen(image.data() + at, 8));
  }
  return out;
}

bool WriteFileText(const fs::path& p, const std::string& text) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
  return bool(out);
}

}  // namespace

Toolset FromCompilerFolder(const fs::path& compiler) {
  Toolset t;
  t.clang = compiler / "bin" / "clang.exe";
  t.lld_link = compiler / "bin" / "lld-link.exe";
  t.rexglue = compiler / "bin" / "rexglue.exe";
  t.sdk_bin = compiler.parent_path();
  t.includes = {compiler / "include", compiler / "include" / "simde", compiler / "include" / "simde" / "simde"};
  t.system_includes = {compiler / "include" / "rex" / "guest_std"};
  for (const char* lib : {"rexppc.lib", "rexbase.lib", "vcruntime140.lib", "vcruntime140_1.lib",
                          "api-ms-win-crt-math-l1-1-0.lib", "api-ms-win-crt-environment-l1-1-0.lib",
                          "api-ms-win-crt-heap-l1-1-0.lib", "api-ms-win-crt-runtime-l1-1-0.lib",
                          "api-ms-win-crt-string-l1-1-0.lib"}) {
    t.libs.push_back(compiler / "lib" / lib);
  }
  t.entry = compiler / "src" / "entry.cpp";
  t.runtime = compiler / "src" / "runtime.cpp";
  t.title_config = compiler / "config" / "default.toml";
  t.names_config = compiler / "config" / "d3d_names.toml";
  return t;
}

std::wstring Blocker(const Toolset& t, const fs::path& out_dir) {
  std::error_code ec;
  std::vector<fs::path> needed = {t.clang, t.lld_link, t.rexglue, t.entry, t.runtime, t.title_config, t.names_config};
  needed.insert(needed.end(), t.libs.begin(), t.libs.end());
  for (const fs::path& p : needed) {
    if (!fs::exists(p, ec)) {
      return L"The game's code has to be compiled for this PC, but " + p.wstring() +
             L" is missing. Extract the whole release again, then start the game.";
    }
  }
  fs::create_directories(out_dir, ec);
  const fs::path probe = out_dir / "write_test.tmp";
  if (!WriteFileText(probe, "x")) {
    return L"The folder " + out_dir.wstring() + L" cannot be written to. Choose another user folder (FCP_USER_ROOT).";
  }
  fs::remove(probe, ec);
  ULARGE_INTEGER free{};
  if (GetDiskFreeSpaceExW(out_dir.c_str(), &free, nullptr, nullptr) && free.QuadPart < kDiskNeeded) {
    return L"Preparing the game for this PC needs 3 GB of free space on the drive of " + out_dir.wstring() + L".";
  }
  return {};
}

bool Codegen(const Toolset& t, const fs::path& xex, const fs::path& work, build::Processes& run, Stats& stats,
             std::wstring& why) {
  const int64_t start = NowMs();
  std::error_code ec;
  const fs::path gen = work / "gen";
  if (fs::exists(gen / "codegen.done", ec)) {
    Say("game code: codegen output of an earlier, interrupted build is complete; reused");
    return true;
  }
  fs::remove_all(gen, ec);
  fs::create_directories(gen, ec);
  fcp::progress::SetStage(fcp::progress::Stage::kCodegen, L"Translating the game code (default.xex)");
  // The manifest the development build writes from config/predator/predator_manifest.toml.in, with this build's
  // paths. Without hook_sources: no mid-asm hooks exist (cmake/fcp_game_code.cmake fails the build otherwise).
  const std::string manifest = std::format(
      "# Written by predator.exe for the first-start build of the game code. Deleted after the build.\n"
      "[project]\nname = \"predator\"\nsdk_version = \"{}\"\ngame_root = \"{}\"\n\n"
      "[entrypoint]\nfile_path = \"{}\"\nout_directory_path = \"{}\"\nincludes = [\n  \"{}\",\n  \"{}\",\n]\n",
      FCP_REXGLUE_SDK_VERSION, Slashes(xex.parent_path()), Slashes(xex), Slashes(gen / "default"),
      Slashes(t.title_config), Slashes(t.names_config));
  const fs::path manifest_path = gen / "predator_manifest.toml";
  if (!WriteFileText(manifest_path, manifest)) return Fail(why, L"Writing the codegen manifest failed.");
  void* process = run.Start(Quote(t.rexglue) + L" codegen " + Quote(manifest_path) + L" --ignore-stamp", gen,
                            t.sdk_bin);
  if (!process) return Fail(why, L"rexglue.exe could not be started.");
  // codegen reports no progress; the bar moves on time (measured on 16 cores: see erster-start-dist.md)
  const int64_t wait_start = NowMs();
  while (WaitForSingleObject(process, 250) == WAIT_TIMEOUT) {
    if (fcp::progress::Cancelled()) {
      run.Cancel();
      return Fail(why, L"Stopped.");
    }
    fcp::progress::SetStageFraction(float(std::min(1.0, double(NowMs() - wait_start) / 45000.0)));
  }
  stats.cpu_s += build::Processes::CpuSeconds(process);
  const unsigned long code = build::Processes::ExitCode(process);
  if (code == 0xC0000135) return Fail(why, L"rexglue.exe is missing a DLL it needs.");  // STATUS_DLL_NOT_FOUND
  if (code) return Fail(why, L"Translating default.xex failed (rexglue.exe exit code " + std::to_wstring(code) + L").");
  if (!fs::exists(gen / "default" / "sources.cmake", ec)) return Fail(why, L"rexglue.exe wrote no code.");
  WriteFileText(gen / "codegen.done", "translated\n");
  stats.codegen_s = double(NowMs() - start) / 1000.0;
  Say(std::format("game code: codegen in {:.1f} s", stats.codegen_s));
  return true;
}

bool CompileAndLink(const Toolset& t, const Job& job, build::Processes& run, Stats& stats, std::wstring& why) {
  std::error_code ec;
  fs::create_directories(job.work / "obj", ec);
  fs::create_directories(job.dll.parent_path(), ec);

  // Sources: init + recomp files of the codegen and the entry. predator_register.cpp registers the functions of a
  // guest DLL module; the main module uses kPPCFuncMappings (init), so it is left out, unless it defines code.
  // The semantic test: every .cpp of its folder.
  const bool semtest = job.kind == "semtest";
  std::vector<Source> sources;
  std::string headers;  // hash input: every generated header
  for (const auto& e : fs::directory_iterator(job.gen, ec)) {
    const fs::path& p = e.path();
    const std::wstring name = p.filename().wstring();
    if (p.extension() == ".h") headers += fcp::hash::Sha256File(p) + "\n";
    if (p.extension() != ".cpp") continue;
    if (semtest) {
      sources.push_back({p, job.work / "obj" / (p.stem().wstring() + L".obj"), e.file_size(ec), {}});
      continue;
    }
    if (name.ends_with(L"_register.cpp")) {
      if (ReadAll(p).find("DEFINE_REX_FUNC") != std::string::npos) {
        return Fail(why, L"predator_register.cpp defines functions (guest DLL imports); not supported yet.");
      }
      continue;
    }
    if (!name.ends_with(L"_init.cpp") && name.find(L"_recomp.") == std::wstring::npos) continue;
    sources.push_back({p, job.work / "obj" / (p.stem().wstring() + L".obj"), e.file_size(ec), {}});
  }
  if (sources.empty()) return Fail(why, L"No generated code in " + job.gen.wstring() + L".");
  std::sort(sources.begin(), sources.end(), [](const Source& a, const Source& b) { return a.src < b.src; });

  // game_code_config.h: the values the DLL reports (src/fcp/game_code/entry.cpp)
  const std::string exports_hash = fcp::game_code::ExportsHash();
  const std::string config = std::format(
      "// Written by the game code build (src/fcp/game_code_build.cpp).\n#pragma once\n"
      "#define FCP_GAME_CODE_ABI \"{}\"\n#define FCP_GAME_CODE_EXPORTS \"{}\"\n#define FCP_GAME_CODE_IMAGE \"{}\"\n"
      "#define FCP_GAME_CODE_ORIGIN \"{}\"\n",
      FCP_GAME_CODE_ABI, exports_hash, job.image_sha256, job.origin);
  const fs::path config_path = job.work / "game_code_config.h";
  if (ReadAll(config_path) != config && !WriteFileText(config_path, config)) {
    return Fail(why, L"Writing game_code_config.h failed.");
  }
  if (!semtest) sources.push_back({t.entry, job.work / "obj" / "fcp_entry.obj", fs::file_size(t.entry, ec), {}});
  sources.push_back({t.runtime, job.work / "obj" / "fcp_runtime.obj", fs::file_size(t.runtime, ec), {}});

  // The key of every object: flags, ABI (SDK headers, codegen), generated headers, the source itself. A changed
  // flag or header rebuilds everything, a changed source only itself.
  const std::wstring probe_cmd = CompileCommand(t, job, L"X", L"Y");
  const std::string common = fcp::hash::Sha256(Narrow(probe_cmd) + "\n" + FCP_GAME_CODE_ABI + "\n" + headers +
                                               (semtest ? std::string() : config));
  std::vector<Source*> todo;
  uint64_t total_bytes = 0, done_bytes = 0;
  for (Source& s : sources) {
    s.key = fcp::hash::Sha256(common + fcp::hash::Sha256File(s.src));
    total_bytes += s.bytes;
    const fs::path key_file = s.obj.wstring() + L".key";
    if (fs::exists(s.obj, ec) && ReadAll(key_file) == s.key) {
      done_bytes += s.bytes;
    } else {
      todo.push_back(&s);
    }
  }
  std::sort(todo.begin(), todo.end(), [](const Source* a, const Source* b) { return a->bytes > b->bytes; });
  stats.files = sources.size();
  stats.compiled = todo.size();
  stats.slots = Slots(todo.size());
  Say(std::format("game code: compiling {} of {} files, {} at a time{}", todo.size(), sources.size(), stats.slots,
                  fcp::progress::WindowNote()));
  run.Note(std::format("---- compile: {} of {} files, {} at a time, flags {} ----", todo.size(), sources.size(),
                       stats.slots, Narrow(BaseFlags())));

  const int64_t compile_start = NowMs();
  uint32_t files_done = 0;
  const int64_t cancel_after = fcp::env::Int("FCP_TEST_GAME_CODE_CANCEL_AFTER", 0);
  const uint32_t first_wave = uint32_t(std::min(stats.slots, todo.size()));
  fcp::progress::SetStage(fcp::progress::Stage::kCompile, L"Compiling the game code");
  fcp::progress::SetCompile(done_bytes, total_bytes, 0, uint32_t(todo.size()), first_wave);
  std::vector<HANDLE> running;
  std::vector<Source*> running_src;
  size_t next = 0;
  int64_t next_note = NowMs() + 15000;
  while (next < todo.size() || !running.empty()) {
    if (NowMs() >= next_note) {
      next_note += 15000;
      double cpu = 0;
      for (HANDLE p : running) cpu += build::Processes::CpuSeconds(p);
      Say(std::format("game code: {} of {} files compiled, {} compilers running ({:.1f} s CPU in them){}", files_done,
                      todo.size(), running.size(), cpu, fcp::progress::WindowNote()));
    }
    for (; running.size() < stats.slots && next < todo.size(); ++next) {
      Source& s = *todo[next];
      fs::remove(s.obj.wstring() + L".key", ec);
      void* process = run.Start(CompileCommand(t, job, s.src, s.obj.wstring() + L".tmp"), job.work);
      if (!process) {
        run.Cancel();
        return Fail(why, L"clang.exe could not be started.");
      }
      running.push_back(static_cast<HANDLE>(process));
      running_src.push_back(&s);
    }
    const DWORD wait = WaitForMultipleObjects(DWORD(running.size()), running.data(), FALSE, 250);
    if (fcp::progress::Cancelled()) {
      run.Cancel();
      return Fail(why, L"Stopped.");
    }
    if (wait == WAIT_TIMEOUT) continue;
    const size_t index = wait - WAIT_OBJECT_0;
    if (index >= running.size()) {
      run.Cancel();
      return Fail(why, L"Waiting on the compilers failed.");
    }
    Source& s = *running_src[index];
    stats.cpu_s += build::Processes::CpuSeconds(running[index]);
    const unsigned long code = build::Processes::ExitCode(running[index]);
    running.erase(running.begin() + ptrdiff_t(index));
    running_src.erase(running_src.begin() + ptrdiff_t(index));
    if (code) {
      run.Cancel();
      return Fail(why, L"Compiling " + s.src.filename().wstring() + L" failed (clang.exe exit code " +
                           std::to_wstring(code) + L").");
    }
    fs::rename(s.obj.wstring() + L".tmp", s.obj, ec);
    if (ec || !WriteFileText(s.obj.wstring() + L".key", s.key)) {
      run.Cancel();
      return Fail(why, L"Saving the compiled " + s.src.filename().wstring() + L" failed.");
    }
    done_bytes += s.bytes;
    ++files_done;
    fcp::progress::SetCompile(done_bytes, total_bytes, files_done, uint32_t(todo.size()), first_wave);
    if (cancel_after > 0 && files_done >= uint32_t(cancel_after)) {
      Say(std::format("game code: FCP_TEST_GAME_CODE_CANCEL_AFTER={}: stopping as the player would", cancel_after));
      fcp::progress::RequestCancel();
    }
  }
  stats.compile_s = double(NowMs() - compile_start) / 1000.0;

  // ---- link ----
  const int64_t link_start = NowMs();
  fcp::progress::SetStage(fcp::progress::Stage::kLink, L"Linking the game code");
  // predator.lib from this executable's own export table (the hooks and the kernel import wrappers): always the
  // one the running executable has.
  const std::vector<std::string> exports = semtest ? std::vector<std::string>() : fcp::game_code::ExportNames();
  const fs::path exe_def_path = job.work / "predator_exe.def", exe_lib = job.work / "predator_exe.lib";
  if (!semtest) {
    std::string exe_def = "LIBRARY predator.exe\nEXPORTS\n";
    for (const std::string& name : exports) exe_def += "  " + name + "\n";
    WriteFileText(exe_def_path, exe_def);
    void* lib_process = run.Start(Quote(t.lld_link) + L" /lib /nologo /machine:x64 /def:" + Quote(exe_def_path) +
                                      L" /out:" + Quote(exe_lib),
                                  job.work);
    if (!lib_process || !run.Wait(lib_process) || build::Processes::ExitCode(lib_process)) {
      return Fail(why, L"Making the import library of predator.exe failed.");
    }
  }
  // The DLL's exports: the table and every original the executable imports (semtest: its data exports).
  std::string dll_def = "LIBRARY " + Narrow(job.dll.filename().wstring()) + "\nEXPORTS\n";
  if (semtest) {
    for (const std::string& name : job.data_exports) dll_def += "  " + name + " DATA\n";
  } else {
    dll_def += "  fcp_game_code\n";
    for (const std::string& name : fcp::game_code::DelayImportNames()) dll_def += "  " + name + "\n";
  }
  const fs::path dll_def_path = job.work / (job.dll.stem().wstring() + L".def");
  WriteFileText(dll_def_path, dll_def);
  std::wstring rsp = L"/nologo /dll /noentry /nodefaultlib /lldignoreenv /machine:x64 /incremental:no\n";
  // /opt:ref,icf as the former static build (lld's default without /debug); /debug for the profiler and crash
  // stacks (the PDB stays in the user folder like the DLL).
  rsp += L"/debug /opt:ref /opt:icf\n";
  if (job.lto) {
    // Code generation runs here, as many jobs as the compile used (CPU and memory bound, FCP_GAME_CODE_CPUS).
    // _fltused (runtime.cpp) is only referenced by the code generation itself, after LTO dropped unused symbols.
    rsp += std::format(L"/opt:lldltojobs={}\n/include:_fltused\n", Slots(sources.size()));
    rsp += L"/lldltocache:" + Quote(job.work / "lto-cache") + L"\n";
  }
  rsp += L"/pdb:" + Quote(fs::path(job.dll).replace_extension(".pdb")) + L"\n";
  rsp += L"/out:" + Quote(job.dll) + L"\n/def:" + Quote(dll_def_path) + L"\n/implib:" +
         Quote(job.work / (job.dll.stem().wstring() + L".lib")) + L"\n";
  // Every export of predator.exe that the generated code defines as a weak alias (the hooks) must come from
  // predator.lib: /include makes the linker take the import first, so the alias stays unused (a weak alias does
  // not pull a library member by itself). The kernel/XAM wrappers __imp__<name> are plain references of the
  // generated code and come from predator.lib only when used.
  size_t hooks = 0;
  for (const std::string& name : exports) {
    if (name.starts_with("__imp__")) continue;
    rsp += L"/include:" + Widen(name) + L"\n";
    ++hooks;
  }
  if (!semtest) {
    run.Note(std::format("---- link: {} hooks of predator.exe, {} originals exported ----", hooks,
                         fcp::game_code::DelayImportNames().size()));
    rsp += Quote(exe_lib) + L"\n";
  }
  for (const fs::path& lib : t.libs) rsp += Quote(lib) + L"\n";
  for (const Source& s : sources) rsp += Quote(s.obj) + L"\n";
  const fs::path rsp_path = job.work / "link.rsp";
  {
    std::ofstream out(rsp_path, std::ios::binary | std::ios::trunc);
    out << Narrow(rsp);
  }
  fs::remove(job.dll, ec);
  if (fs::exists(job.dll, ec)) return Fail(why, job.dll.wstring() + L" cannot be replaced (is the game running?).");
  void* link = run.Start(Quote(t.lld_link) + L" @" + Quote(rsp_path), job.work);
  if (!link) return Fail(why, L"lld-link.exe could not be started.");
  if (!run.Wait(link)) return Fail(why, L"Stopped.");
  stats.cpu_s += build::Processes::CpuSeconds(link);
  if (const unsigned long code = build::Processes::ExitCode(link)) {
    fs::remove(job.dll, ec);
    return Fail(why, L"Linking predator_code.dll failed (lld-link.exe exit code " + std::to_wstring(code) + L").");
  }
  // Nothing in the DLL may need the C runtime's start-up (it has no entry point).
  for (const std::string& section : SectionNames(ReadAll(job.dll))) {
    if (section == ".tls" || section.starts_with(".CRT")) {
      fs::remove(job.dll, ec);
      return Fail(why, L"predator_code.dll has a section " + Widen(section) +
                           L" (static initialisation or thread locals), which the DLL cannot run.");
    }
  }
  stats.link_s = double(NowMs() - link_start) / 1000.0;
  Say(std::format("game code: {} built: {} files ({} compiled, {} at a time) {:.1f} s, link {:.1f} s, {:.0f} s CPU",
                  U8(job.dll), stats.files, stats.compiled, stats.slots, stats.compile_s, stats.link_s, stats.cpu_s));
  return true;
}

bool RunCommand(int& exit_code) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv || argc < 2 || std::wcscmp(argv[1], L"--fcp-build-game-code") != 0) {
    if (argv) LocalFree(argv);
    return false;
  }
  g_command_mode = true;
  exit_code = 2;
  if (argc < 3) {
    std::printf("usage: predator.exe --fcp-build-game-code <settings.txt>\n");
    LocalFree(argv);
    return true;
  }
  const fs::path settings_path = argv[2];
  LocalFree(argv);
  // key=value lines (cmake/fcp_game_code.cmake writes them)
  Toolset t;
  Job job;
  std::ifstream in(settings_path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const fs::path value = Widen(line.substr(eq + 1));
    if (key == "gen") job.gen = value;
    else if (key == "dll") job.dll = value;
    else if (key == "work") job.work = value;
    else if (key == "log") job.log = value;
    else if (key == "origin") job.origin = line.substr(eq + 1);
    else if (key == "image_sha256") job.image_sha256 = line.substr(eq + 1);
    else if (key == "pgo_profile") job.pgo_profile = value;
    else if (key == "lto") job.lto = line.substr(eq + 1) == "thin";
    else if (key == "clang") t.clang = value;
    else if (key == "lld_link") t.lld_link = value;
    else if (key == "include") t.includes.push_back(value);
    else if (key == "isystem") t.system_includes.push_back(value);
    else if (key == "lib") t.libs.push_back(value);
    else if (key == "entry") t.entry = value;
    else if (key == "runtime") t.runtime = value;
    else if (key == "kind") job.kind = line.substr(eq + 1);
    else if (key == "data_export") job.data_exports.push_back(line.substr(eq + 1));
  }
  if (job.gen.empty() || job.dll.empty() || job.work.empty() || t.clang.empty() || t.lld_link.empty()) {
    std::printf("fcp-build-game-code: %s lacks gen, dll, work, clang or lld_link\n", U8(settings_path).c_str());
    return true;
  }
  if (job.log.empty()) job.log = job.work / "build.log";
  if (job.origin.empty()) job.origin = "build";
  std::error_code ec;
  fs::create_directories(job.work, ec);
  fs::remove(job.log, ec);
  build::Processes run(job.log);
  Stats stats;
  std::wstring why;
  if (!CompileAndLink(t, job, run, stats, why)) {
    std::printf("fcp-build-game-code: FAILED: %s\n--- end of %s ---\n%s\n", Narrow(why).c_str(), U8(job.log).c_str(),
                Narrow(fcp::progress::LogTail(job.log)).c_str());
    exit_code = 1;
    return true;
  }
  exit_code = 0;
  return true;
}

}  // namespace fcp::game_code_build
