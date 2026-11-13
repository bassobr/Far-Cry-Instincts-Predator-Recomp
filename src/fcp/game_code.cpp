// The recompiled game code in predator_code.dll (fcp/game_code.h).
#include "fcp/game_code.h"

#include <windows.h>
#include <delayimp.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>

#include <rex/platform/file.h>
#include <rex/ppc/image.h>
#include <rex/ppc/recompiled_code.h>

#include "fcp/build_process.h"
#include "fcp/env.h"
#include "fcp/first_start.h"
#include "fcp/game_code_build.h"
#include "fcp/hash.h"
#include "fcp/imports.h"
#include "fcp/log.h"
#include "fcp/progress_window.h"
#include "fcp/user_paths.h"
#include "fcp_game_code_abi.h"

namespace fcp::game_code {
namespace {

namespace fs = std::filesystem;

constexpr wchar_t kTitle[] = L"Far Cry Instincts Predator";
constexpr char kDllName[] = "predator_code.dll";

HMODULE g_module = nullptr;
const FcpGameCode* g_info = nullptr;
uintptr_t g_code_begin = 0, g_code_end = 0;

std::string U8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

const IMAGE_NT_HEADERS64* Nt(const uint8_t* base) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  return reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
}

bool InCode(const uint8_t* base, uint32_t rva) {
  const IMAGE_NT_HEADERS64* nt = Nt(base);
  const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
    if (rva >= s->VirtualAddress && rva < s->VirtualAddress + std::max(s->Misc.VirtualSize, s->SizeOfRawData)) {
      return (s->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    }
  }
  return false;
}

void Section(HMODULE module, const char* name, uintptr_t& begin, uintptr_t& end) {
  const auto* base = reinterpret_cast<const uint8_t*>(module);
  const IMAGE_NT_HEADERS64* nt = Nt(base);
  const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
    if (std::strncmp(reinterpret_cast<const char*>(s->Name), name, 8) == 0) {
      begin = uintptr_t(base) + s->VirtualAddress;
      end = begin + s->Misc.VirtualSize;
      return;
    }
  }
  begin = end = 0;
}

// The delay-load helper asks before it loads predator_code.dll: the module Prepare loaded, from wherever it is.
FARPROC WINAPI DelayHook(unsigned notify, PDelayLoadInfo info) {
  if (notify == dliNotePreLoadLibrary && info && _stricmp(info->szDll, kDllName) == 0) {
    return reinterpret_cast<FARPROC>(g_module);
  }
  if (notify == dliFailLoadLib || notify == dliFailGetProc) {
    const char* what = info && info->dlp.fImportByName ? info->dlp.szProcName : "?";
    FCPLOG("FATAL game code: delay load of {}!{} failed (error {})", info ? info->szDll : "?", what,
           info ? info->dwLastError : 0);
    fcp::log::Drain(std::chrono::milliseconds(1000));
  }
  return nullptr;
}

fs::path ExeFolder() { return rex::platform::GetExecutableFolder(); }

// After a failed load with ERROR_PROC_NOT_FOUND: the first import of the DLL file that its module lacks.
std::string MissingImport(const fs::path& dll) {
  HMODULE image = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!image) return "?";
  // the low bits of a resource handle mark the mapping; the image starts at the aligned address
  const auto* base = reinterpret_cast<const uint8_t*>(uintptr_t(image) & ~uintptr_t(0xFFFF));
  std::string missing = "?";
  const IMAGE_DATA_DIRECTORY& dir = Nt(base)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
       dir.VirtualAddress && d->Name && missing == "?"; ++d) {
    const char* module_name = reinterpret_cast<const char*>(base + d->Name);
    HMODULE module = GetModuleHandleA(module_name);
    if (!module) module = LoadLibraryExA(module_name, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
      missing = std::string(module_name) + " (not found)";
      break;
    }
    for (const auto* t = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + d->OriginalFirstThunk);
         t->u1.AddressOfData; ++t) {
      if (IMAGE_SNAP_BY_ORDINAL64(t->u1.Ordinal)) continue;
      const char* name =
          reinterpret_cast<const char*>(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + t->u1.AddressOfData)->Name);
      if (!GetProcAddress(module, name)) {
        missing = std::string(module_name) + "!" + name;
        break;
      }
    }
  }
  FreeLibrary(image);
  return missing;
}

// Loads one candidate and checks it; on success the module stays loaded.
bool TryLoad(const fs::path& dll, const std::string& image_sha256, std::string& why) {
  std::error_code ec;
  if (!fs::exists(dll, ec)) return why = "not built yet", false;
  // A DLL that imports something this executable no longer exports fails here: no message box, a reason.
  const UINT mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
  HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  const DWORD error = GetLastError();
  SetErrorMode(mode);
  if (!m) {
    why = error == ERROR_PROC_NOT_FOUND
              ? "it imports a function that is missing: " + MissingImport(dll)
              : std::format("it does not load (error {})", error);
    return false;
  }
  const auto get = reinterpret_cast<FcpGameCodeFunc>(GetProcAddress(m, "fcp_game_code"));
  const FcpGameCode* info = get ? get() : nullptr;
  if (!info || info->interface_version != FCP_GAME_CODE_INTERFACE || info->size != sizeof(FcpGameCode)) {
    why = "it has another interface version";
  } else if (std::strcmp(info->abi, FCP_GAME_CODE_ABI) != 0) {
    why = std::format("it was built for another executable (ABI {}, this one {})", info->abi, FCP_GAME_CODE_ABI);
  } else if (ExportsHash() != info->exports) {
    why = std::format("it was linked against other hooks (exports {}, this executable {})", info->exports,
                      ExportsHash());
  } else if (_stricmp(info->image_sha256, FCP_DISC_XEX_SHA256) != 0) {
    why = std::format("it was made from another default.xex ({})", info->image_sha256);
  } else if (!image_sha256.empty() && _stricmp(info->image_sha256, image_sha256.c_str()) != 0) {
    why = std::format("the disc image has another default.xex ({})", image_sha256);
  } else {
    g_module = m;
    g_info = info;
    Section(m, "rexguest", g_code_begin, g_code_end);
    return true;
  }
  FreeLibrary(m);
  return false;
}

// Resolves every import of the executable from the DLL now, so no guest thread runs the delay-load helper later.
// A missing export raises an exception in the helper (no C++ objects here, for __try).
HRESULT LoadAllImports() {
  __try {
    return __HrLoadAllImportsForDll(kDllName);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return HRESULT(0x8007007F);  // ERROR_PROC_NOT_FOUND
  }
}

bool BindImports(std::string& why) {
  const HRESULT hr = LoadAllImports();
  if (FAILED(hr)) {
    why = std::format("binding the executable's imports to the DLL failed ({:08X})", unsigned(hr));
    return false;
  }
  return true;
}

// Names the loaded DLL imports from predator.exe (its import table), sorted.
std::vector<std::string> DllImportsFromExe(HMODULE module) {
  std::vector<std::string> names;
  const auto* base = reinterpret_cast<const uint8_t*>(module);
  const IMAGE_DATA_DIRECTORY& dir = Nt(base)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return names;
  for (const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
    if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "predator.exe") != 0) continue;
    for (const auto* t = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + d->OriginalFirstThunk);
         t->u1.AddressOfData; ++t) {
      if (IMAGE_SNAP_BY_ORDINAL64(t->u1.Ordinal)) continue;
      names.emplace_back(
          reinterpret_cast<const char*>(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + t->u1.AddressOfData)->Name));
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

void Message(const std::wstring& text) {
  FCPLOG("game code: {}", build::Narrow(text));
  if (!fcp::env::Flag("FCP_NO_DIALOGS")) MessageBoxW(nullptr, text.c_str(), kTitle, MB_ICONERROR);
}

// The whole first start: default.xex from the disc image, codegen, compile, link. True when the DLL was built.
bool BuildFromDisc(const fs::path& image, const fs::path& dll) {
  const fs::path compiler = ExeFolder() / "compiler";
  game_code_build::Toolset tools = game_code_build::FromCompilerFolder(compiler);
  const fs::path folder = dll.parent_path();
  if (const std::wstring blocker = game_code_build::Blocker(tools, folder); !blocker.empty()) {
    Message(blocker);
    return false;
  }
  const fs::path work = folder / "work";
  const fs::path log = folder / "build.log";
  std::error_code ec;
  fs::create_directories(work, ec);
  // Another default.xex, executable or compiler than an interrupted build had: nothing of it is of use.
  const std::string key = std::format("{} {} {}", FCP_GAME_CODE_ABI, FCP_DISC_XEX_SHA256, ExportsHash());
  {
    std::string old;
    if (std::ifstream in(work / "key.txt"); in) std::getline(in, old);
    if (old != key || fcp::env::Flag("FCP_GAME_CODE_REBUILD")) {
      fs::remove_all(work, ec);
      fs::create_directories(work, ec);
      std::ofstream(work / "key.txt") << key << "\n";
    }
  }
  const int64_t start = NowMs();
  fcp::progress::Begin();
  game_code_build::Stats stats;
  std::wstring why;
  bool ok = false;
  {
    build::Processes run(log, int(fcp::env::Int("FCP_GAME_CODE_CPUS", 0)));
    run.Note(std::format("---- game code {} ----", key));
    fcp::progress::SetStage(fcp::progress::Stage::kDisc, L"Reading default.xex from the disc image");
    std::vector<uint8_t> xex;
    std::string read_why;
    const fs::path xex_path = work / "game" / "default.xex";
    if (!first_start::ReadDiscFile(image, "default.xex", xex, read_why)) {
      why = build::Widen(read_why);
    } else if (const std::string sha = fcp::hash::Sha256(xex.data(), xex.size());
               _stricmp(sha.c_str(), FCP_DISC_XEX_SHA256) != 0) {
      why = std::format(L"The disc image has another default.xex than the one this release supports (SHA-256 {}, "
                        L"expected {}, {} bytes). Use the disc version without a title update.",
                        build::Widen(sha), build::Widen(FCP_DISC_XEX_SHA256), FCP_DISC_XEX_SIZE);
    } else {
      fs::create_directories(xex_path.parent_path(), ec);
      std::ofstream(xex_path, std::ios::binary).write(reinterpret_cast<const char*>(xex.data()),
                                                       std::streamsize(xex.size()));
      fcp::progress::SetStageFraction(1.0f);
      if (game_code_build::Codegen(tools, xex_path, work, run, stats, why)) {
        game_code_build::Job job;
        job.gen = work / "gen" / "default";
        job.dll = dll;
        job.work = work;
        job.log = log;
        job.origin = "first-start";
        job.image_sha256 = FCP_DISC_XEX_SHA256;
        ok = game_code_build::CompileAndLink(tools, job, run, stats, why);
      }
    }
  }
  const bool cancelled = fcp::progress::Cancelled();
  fcp::progress::End(ok);
  if (ok) {
    FCPLOG("game code: first-start build in {:.1f} s (codegen {:.1f} s, compile {} files {:.1f} s with {} at a time, "
           "link {:.1f} s, {:.0f} s CPU in the tools){}",
           double(NowMs() - start) / 1000.0, stats.codegen_s, stats.compiled, stats.compile_s, stats.slots,
           stats.link_s, stats.cpu_s, fcp::progress::WindowNote());
    if (!fcp::env::Flag("FCP_GAME_CODE_KEEP")) fs::remove_all(work, ec);  // game code: only the DLL stays
    return true;
  }
  if (cancelled) {
    FCPLOG("game code: build stopped by the player; the next start carries on");
  } else {
    FCPLOG("game code: build FAILED: {}", build::Narrow(why));
    fcp::progress::ShowFailure(why, log);
  }
  return false;
}

}  // namespace

#if FCP_STATIC_GAME_CODE
}  // namespace fcp::game_code
// -DFCP_GAME_CODE_DLL=OFF: the generated code is linked into this executable (the former build).
extern const rex::ppc::PPCImage kPPCImageConfig;
namespace fcp::game_code {
#endif

const rex::ppc::PPCImage* Prepare() {
#if FCP_STATIC_GAME_CODE
  g_module = GetModuleHandleW(nullptr);
  Section(g_module, "rexguest", g_code_begin, g_code_end);
  FCPLOG("game code: linked into the executable (FCP_GAME_CODE_DLL=OFF), rexguest {} MB",
         (g_code_end - g_code_begin) >> 20);
  if (const fs::path image = first_start::ImagePath(); !image.empty()) {
    const std::string target = first_start::PackTarget();
    if (!first_start::UseExistingPack(target)) FCPLOG("game code: no shader pack (static build: not built here)");
  }
  return &kPPCImageConfig;
#endif
  const int64_t start = NowMs();
  {
    // The DLL imports from "predator.exe": a renamed executable would make Windows look for another one.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (_wcsicmp(fs::path(self).filename().c_str(), L"predator.exe") != 0) {
      Message(L"The game must be started as predator.exe (the recompiled code links against that name). "
              L"Rename the file back to predator.exe.");
      return nullptr;
    }
  }
  const fs::path image = first_start::ImagePath();
  std::string image_sha256, image_why;
  if (!image.empty()) {
    image_sha256 = first_start::XexSha256(image, Folder() / "disc_check.txt", image_why);
    FCPLOG("game code: disc image {}: default.xex {}", U8(image), image_sha256.empty() ? "not readable: " + image_why : "SHA-256 " + image_sha256);
  } else {
    FCPLOG("game code: no disc image set (FCP_GAME_ISO / [game] image in predator.toml)");
  }
  // One build at a time: a second copy started meanwhile waits for it, then finds the DLL fitting.
  HANDLE lock = CreateMutexW(nullptr, FALSE, L"Far-Cry-Instincts-Predator-Recomp-GameCode");
  WaitForSingleObject(lock, INFINITE);
  std::vector<fs::path> candidates;
  if (std::string explicit_dll = fcp::env::Get("FCP_GAME_CODE_DLL"); !explicit_dll.empty()) {
    candidates.push_back(build::Widen(explicit_dll));
  } else {
    candidates.push_back(Folder() / "predator_code.dll");
    std::error_code ec;
    const fs::path beside = ExeFolder() / "game_code" / "predator_code.dll";
    if (!fs::equivalent(Folder(), beside.parent_path(), ec)) candidates.push_back(beside);
  }
  const bool rebuild = fcp::env::Flag("FCP_GAME_CODE_REBUILD");
  bool loaded = false;
  for (const fs::path& dll : candidates) {
    if (rebuild && dll == candidates.front()) {
      FCPLOG("game code: {} not used (FCP_GAME_CODE_REBUILD)", U8(dll));
      continue;
    }
    std::string why;
    if (TryLoad(dll, image_sha256, why)) {
      loaded = true;
      break;
    }
    FCPLOG("game code: {} not used: {}", U8(dll), why);
  }
  if (!loaded) {
    if (!fcp::env::Flag("FCP_GAME_CODE_BUILD", true)) {
      Message(L"The game code (predator_code.dll) is missing or does not fit, and FCP_GAME_CODE_BUILD=0.");
    } else if (image.empty()) {
      Message(L"The game code has to be prepared once from your disc image. Set its path in predator.toml "
              L"([game] image = \"...\"), in " + (fcp::paths::Root() / "predator.toml").wstring() + L".");
    } else if (image_sha256.empty()) {
      Message(L"The disc image cannot be read: " + build::Widen(image_why) + L". Check [game] image in " + (fcp::paths::Root() / "predator.toml").wstring() + L".");
    } else if (_stricmp(image_sha256.c_str(), FCP_DISC_XEX_SHA256) != 0) {
      Message(std::format(L"The disc image {} has another default.xex than the one this release supports "
                          L"(SHA-256 {}, expected {}). Use the disc version without a title update.",
                          image.wstring(), build::Widen(image_sha256), build::Widen(FCP_DISC_XEX_SHA256)));
    } else {
      const fs::path dll = Folder() / "predator_code.dll";
      FCPLOG("game code: building {} from {}", U8(dll), U8(image));
      std::string why;
      if (BuildFromDisc(image, dll)) {
        if (TryLoad(dll, image_sha256, why)) {
          loaded = true;
        } else {
          Message(L"The new game code does not load: " + build::Widen(why) + L".");
        }
      }
    }
  }
  ReleaseMutex(lock);
  CloseHandle(lock);
  if (!loaded) return nullptr;
  std::string why;
  if (!BindImports(why)) {
    Message(L"The game code does not fit this executable: " + build::Widen(why) + L".");
    return nullptr;
  }
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(g_module, path, MAX_PATH);
  const auto* base = reinterpret_cast<const uint8_t*>(g_module);
  {
    const std::vector<std::string> from_exe = DllImportsFromExe(g_module);
    const size_t hooks = size_t(std::count_if(from_exe.begin(), from_exe.end(),
                                              [](const std::string& n) { return !n.starts_with("__imp__"); }));
    FCPLOG("game code: the DLL imports {} symbols from predator.exe ({} hooks, {} kernel/XAM wrappers); the EXE "
           "imports {} originals from the DLL",
           from_exe.size(), hooks, from_exe.size() - hooks, DelayImportNames().size());
    fcp::imports::MarkImported(from_exe);
  }
  FCPLOG("game code: {} (made by: {}, {} functions, image {}), at 0x{:X}..0x{:X}, rexguest {} MB, ready in {} ms",
         U8(path), g_info->origin, g_info->function_count, g_info->image_sha256, uintptr_t(base),
         uintptr_t(base) + Nt(base)->OptionalHeader.SizeOfImage, (g_code_end - g_code_begin) >> 20,
         NowMs() - start);
  // The shader pack of the renderer, from the same image (first start, or when it was deleted).
  const std::string target = first_start::PackTarget();
  if (!first_start::UseExistingPack(target)) {
    if (image.empty()) {
      FCPLOG("game code: no shader pack and no disc image; the renderer translates the shaders at run time");
    } else if (!fcp::env::Flag("FCP_SHADER_PACK_BUILD", true)) {
      FCPLOG("game code: no shader pack (FCP_SHADER_PACK_BUILD=0); the renderer translates at run time");
    } else {
      fcp::progress::Begin();
      std::string pack_why;
      const fs::path log = Folder() / "build.log";
      const bool ok = first_start::BuildShaderPack(image, Folder() / "pack", target, log, pack_why);
      const bool cancelled = fcp::progress::Cancelled();
      fcp::progress::End(ok);
      if (!ok && !cancelled) {
        // Not fatal: without the pack the renderer translates every shader at run time and caches it.
        FCPLOG("game code: shader pack FAILED ({}); the renderer translates at run time", pack_why);
        fcp::progress::ShowFailure(build::Widen("Building the shader pack failed: " + pack_why +
                                                ". The game starts anyway and translates the shaders while it runs."),
                                   log);
      } else if (cancelled) {
        FCPLOG("game code: shader pack stopped by the player; the game starts without it");
      }
    }
  }
  return static_cast<const rex::ppc::PPCImage*>(g_info->image);
}

const FcpGameCode* Info() { return g_info; }

const PPCFuncMapping* FuncMappings() {
#if FCP_STATIC_GAME_CODE
  return kPPCFuncMappings;
#else
  return g_info ? static_cast<const PPCFuncMapping*>(g_info->func_mappings) : nullptr;
#endif
}

void CodeRange(uintptr_t& begin, uintptr_t& end) {
  begin = g_code_begin;
  end = g_code_end;
}

void* FpStats() { return g_info ? g_info->fp_stats : nullptr; }

std::vector<std::string> ExportNames() {
  std::vector<std::string> names;
  const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  const IMAGE_DATA_DIRECTORY& dir = Nt(base)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (!dir.VirtualAddress) return names;
  const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
  const auto* name_rvas = reinterpret_cast<const uint32_t*>(base + exp->AddressOfNames);
  const auto* ordinals = reinterpret_cast<const uint16_t*>(base + exp->AddressOfNameOrdinals);
  const auto* functions = reinterpret_cast<const uint32_t*>(base + exp->AddressOfFunctions);
  for (uint32_t i = 0; i < exp->NumberOfNames; ++i) {
    // functions only (no data exports such as NvOptimusEnablement)
    if (InCode(base, functions[ordinals[i]])) names.emplace_back(reinterpret_cast<const char*>(base + name_rvas[i]));
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::string ExportsHash() {
  static const std::string hash = [] {
    std::string text;
    for (const std::string& n : ExportNames()) text += n + "\n";
    return fcp::hash::Sha256(text);
  }();
  return hash;
}

std::vector<std::string> DelayImportNames() {
  std::vector<std::string> names;
  const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  const IMAGE_DATA_DIRECTORY& dir = Nt(base)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
  if (!dir.VirtualAddress) return names;
  for (const auto* d = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dir.VirtualAddress); d->DllNameRVA;
       ++d) {
    if (_stricmp(reinterpret_cast<const char*>(base + d->DllNameRVA), kDllName) != 0) continue;
    for (const auto* t = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + d->ImportNameTableRVA); t->u1.AddressOfData;
         ++t) {
      if (IMAGE_SNAP_BY_ORDINAL64(t->u1.Ordinal)) continue;
      const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + t->u1.AddressOfData);
      names.emplace_back(reinterpret_cast<const char*>(by_name->Name));
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

const char* AbiHash() { return FCP_GAME_CODE_ABI; }
const char* ExpectedImageSha256() { return FCP_DISC_XEX_SHA256; }
uint64_t ExpectedImageSize() { return FCP_DISC_XEX_SIZE; }

fs::path Folder() { return fcp::paths::Root() / "game_code"; }

}  // namespace fcp::game_code

extern "C" const PfnDliHook __pfnDliNotifyHook2 = fcp::game_code::DelayHook;
extern "C" const PfnDliHook __pfnDliFailureHook2 = fcp::game_code::DelayHook;
