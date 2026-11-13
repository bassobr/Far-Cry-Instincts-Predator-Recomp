#include "fcp/thread_profile.h"

#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <format>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>
// windows.h first
#include <psapi.h>
#include <timeapi.h>

#include <rex/app/app.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/recompiled_code.h>

#include "fcp/env.h"
#include "fcp/game_code.h"
#include "fcp/gametime.h"
#include "fcp/log.h"

namespace fcp::profile {
namespace {

using Clock = std::chrono::steady_clock;

enum Where : int { kExe, kRex, kSystem, kDriver, kOther, kWhereCount };
constexpr const char* kWhereNames[kWhereCount] = {"exe", "rex", "system", "driver", "other"};

struct Module {
  uintptr_t lo = 0, hi = 0;
  std::string name;
  Where where = kOther;
};

struct ThreadStats {
  std::string name;
  HANDLE handle = nullptr;
  uint64_t cpu100ns_at_window = 0;
  uint64_t samples = 0;
  uint64_t where[kWhereCount] = {};
  std::unordered_map<uint32_t, uint32_t> guest_lr;  // samples in predator.exe by guest lr
  std::unordered_map<uintptr_t, uint32_t> host_fn;  // samples by host function start
  std::unordered_map<uintptr_t, uint32_t> caller;   // samples outside predator.exe: first predator.exe frame
  std::unordered_map<uintptr_t, uint32_t> caller2;  // and the predator.exe frame above it
  std::unordered_map<uint32_t, uint32_t> guest_self;  // innermost guest function on the stack (rip or caller)
  std::unordered_map<uint32_t, uint32_t> guest_incl;  // every guest function on the stack, once per sample
  std::unordered_map<uint64_t, uint32_t> guest_edge;  // innermost guest function and its guest caller
};

constexpr int kMaxDepth = 96;

// Walks the stack of a suspended thread (no allocation): writes up to max return addresses into out and returns
// their number. Used for the guest function view (self and inclusive).
int WalkStack(CONTEXT c, uintptr_t* out, int max) {
  int n = 0;
  for (int i = 0; i < max; ++i) {
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
    if (!fe) {
      MEMORY_BASIC_INFORMATION info{};
      if (!VirtualQuery(reinterpret_cast<void*>(c.Rsp), &info, sizeof(info)) || info.State != MEM_COMMIT) break;
      c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
      c.Rsp += 8;
    } else {
      PVOID handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fe, &c, &handler_data, &establisher, nullptr);
    }
    if (c.Rip == 0) break;
    out[n++] = uintptr_t(c.Rip);
  }
  return n;
}

// Host function -> guest function (kPPCFuncMappings of the generated code). Recompiled functions live in the
// section "rexguest"; an address there belongs to the nearest mapped function below it. Elsewhere (hooks) only an
// exact function start counts.
struct GuestFns {
  std::vector<std::pair<uintptr_t, uint32_t>> by_host;  // sorted by host address
  uintptr_t code_lo = 0, code_hi = 0;                   // section rexguest

  void Load() {
    // The table and the section of predator_code.dll (fcp/game_code.h). A hooked function maps to the import
    // thunk in the DLL, which jumps to the hook in the EXE: neither is in rexguest, so only exact starts count.
    if (const PPCFuncMapping* maps = fcp::game_code::FuncMappings()) {
      for (const PPCFuncMapping* m = maps; m->address; ++m)
        if (m->host) by_host.emplace_back(reinterpret_cast<uintptr_t>(m->host), uint32_t(m->address));
    }
    std::sort(by_host.begin(), by_host.end());
    fcp::game_code::CodeRange(code_lo, code_hi);
  }
  // Guest address of the function that holds host address a, 0 when none.
  uint32_t Of(uintptr_t a, bool exact_start) const {
    auto it = std::upper_bound(by_host.begin(), by_host.end(), std::make_pair(a, UINT32_MAX));
    if (it == by_host.begin()) return 0;
    --it;
    if (it->first == a) return it->second;
    if (exact_start) return 0;
    return (a >= code_lo && a < code_hi) ? it->second : 0;
  }
};
GuestFns g_guest;
int g_depth = kMaxDepth;  // FCP_PROFILE_DEPTH
int g_top = 6;            // FCP_PROFILE_TOP

std::atomic<bool> g_stop{false};
std::thread g_thread;
rex::kernel::TitleRuntime* g_runtime = nullptr;
std::vector<Module> g_modules;
uintptr_t g_exe_lo = 0, g_exe_hi = 0;
uintptr_t g_dll_lo = 0, g_dll_hi = 0;  // predator_code.dll (the recompiled code; counted as "exe")

bool InExe(uintptr_t a) { return (a >= g_exe_lo && a < g_exe_hi) || (a >= g_dll_lo && a < g_dll_hi); }

Where Classify(const std::string& lower) {
  if (lower == "predator.exe" || lower == "predator_code.dll") return kExe;
  if (lower.rfind("rex", 0) == 0) return kRex;
  if (lower == "ntdll.dll" || lower == "kernel32.dll" || lower == "kernelbase.dll" || lower == "win32u.dll" ||
      lower == "user32.dll" || lower == "ucrtbase.dll" || lower.rfind("msvcp", 0) == 0 ||
      lower.rfind("vcruntime", 0) == 0)
    return kSystem;
  if (lower.rfind("nv", 0) == 0 || lower.rfind("vulkan", 0) == 0 || lower.rfind("d3d12", 0) == 0 ||
      lower.rfind("dxgi", 0) == 0 || lower.rfind("amd", 0) == 0 || lower.rfind("igd", 0) == 0)
    return kDriver;
  return kOther;
}

void LoadModules() {
  g_modules.clear();
  HMODULE mods[1024];
  DWORD needed = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
  for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
    MODULEINFO info{};
    char path[MAX_PATH] = {};
    if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &info, sizeof(info)) ||
        !GetModuleFileNameA(mods[i], path, MAX_PATH))
      continue;
    std::string name = path;
    if (const size_t slash = name.find_last_of("\\/"); slash != std::string::npos) name = name.substr(slash + 1);
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    Module m;
    m.lo = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    m.hi = m.lo + info.SizeOfImage;
    m.name = name;
    m.where = Classify(lower);
    if (lower == "predator.exe") g_exe_lo = m.lo, g_exe_hi = m.hi;
    if (lower == "predator_code.dll") g_dll_lo = m.lo, g_dll_hi = m.hi;
    g_modules.push_back(m);
  }
}

const Module* FindModule(uintptr_t rip) {
  for (const Module& m : g_modules)
    if (rip >= m.lo && rip < m.hi) return &m;
  return nullptr;
}

uint64_t ThreadCpu(HANDLE h) {
  FILETIME c, e, k, u;
  if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
  return (uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime);
}

std::string HostName(uintptr_t fn) {
  const Module* m = FindModule(fn);
  return m ? std::format("{}+{:#x}", m->name, fn - m->lo) : std::format("{:#x}", fn);
}

// Guest function of a host address: inside rexguest the enclosing recompiled function, elsewhere in predator.exe a
// hook registered for a guest address (exact start of its unwind entry), else 0.
uint32_t GuestAt(uintptr_t a) {
  if (!InExe(a)) return 0;
  if (const uint32_t g = g_guest.Of(a, false)) return g;
  DWORD64 image_base = 0;
  PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(a, &image_base, nullptr);
  return fe ? g_guest.Of(uintptr_t(image_base + fe->BeginAddress), true) : 0;
}

std::string TopEdges(const std::unordered_map<uint64_t, uint32_t>& counts, uint64_t total, int n) {
  std::vector<std::pair<uint64_t, uint32_t>> v(counts.begin(), counts.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
  std::string out;
  for (int i = 0; i < n && i < int(v.size()); ++i)
    out += std::format(" {:08X}<{:08X}:{:.1f}%", uint32_t(v[i].first >> 32), uint32_t(v[i].first),
                       total ? 100.0 * v[i].second / double(total) : 0.0);
  return out;
}

template <typename K>
std::string Top(const std::unordered_map<K, uint32_t>& counts, uint64_t total, int n, bool guest) {
  std::vector<std::pair<K, uint32_t>> v(counts.begin(), counts.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
  std::string out;
  for (int i = 0; i < n && i < int(v.size()); ++i) {
    const double pct = total ? 100.0 * v[i].second / double(total) : 0.0;
    if constexpr (std::is_same_v<K, uint32_t>) {
      out += std::format(" {:08X}:{:.1f}%", v[i].first, pct);
    } else {
      out += std::format(" {}:{:.0f}%", HostName(v[i].first), pct);
    }
  }
  (void)guest;
  return out;
}

void Loop(int interval_ms, double window_s) {
  SetThreadDescription(GetCurrentThread(), L"fcp profile");
  timeBeginPeriod(1);
  std::unordered_map<uint32_t, ThreadStats> threads;  // by host thread id
  Clock::time_point window_start = Clock::now();
  uint64_t frame0 = fcp::gametime::Frame();
  int module_refresh = 0;
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    if (rex::app::IsShutdownRequested()) break;
    if (module_refresh-- <= 0) {
      LoadModules();
      module_refresh = 500;
    }
    auto pins = g_runtime->GetThreadRegistry().All();
    for (auto& pin : pins) {
      rex::kernel::KernelThread& t = *pin;
      if (!t.IsGuestThread() || !t.GetThread() || !t.GetThreadState()) continue;
      PPCContext* ctx = t.GetThreadState()->GetContext();
      const uint32_t id = t.GetThread()->GetSystemID();
      auto [it, inserted] = threads.try_emplace(id);
      ThreadStats& s = it->second;
      if (inserted) {
        s.name = std::string(t.GetName());
        s.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
        if (s.handle) s.cpu100ns_at_window = ThreadCpu(s.handle);
      }
      if (!s.handle) continue;
      s.host_fn.reserve(256);
      s.guest_lr.reserve(256);
      CONTEXT c{};
      c.ContextFlags = CONTEXT_FULL;
      uint32_t lr = 0;
      bool ok = false;
      uintptr_t caller = 0, caller2 = 0;
      uintptr_t frames[kMaxDepth];
      int depth = 0;
      if (SuspendThread(s.handle) != DWORD(-1)) {
        ok = GetThreadContext(s.handle, &c) != FALSE;
        lr = ctx ? uint32_t(ctx->lr) : 0;
        if (ok) depth = WalkStack(c, frames, g_depth);
        ResumeThread(s.handle);
      }
      if (!ok) continue;
      const uintptr_t rip = uintptr_t(c.Rip);
      if (!InExe(rip)) {
        for (int i = 0; i < depth; ++i) {
          if (InExe(frames[i])) {
            if (!caller) {
              caller = frames[i];
            } else {
              caller2 = frames[i];
              break;
            }
          }
        }
      }
      {
        // Guest view: the innermost guest function (the rip itself, else the first guest frame), its guest caller,
        // and every guest function once.
        uint32_t chain[kMaxDepth + 1];
        int nchain = 0;
        if (const uint32_t g = GuestAt(rip)) chain[nchain++] = g;
        for (int i = 0; i < depth; ++i)
          if (const uint32_t g = GuestAt(frames[i] - 1)) chain[nchain++] = g;
        if (nchain) {
          ++s.guest_self[chain[0]];
          if (nchain > 1) ++s.guest_edge[uint64_t(chain[0]) << 32 | chain[1]];
          for (int i = 0; i < nchain; ++i) {
            bool seen = false;
            for (int j = 0; j < i && !seen; ++j) seen = chain[j] == chain[i];
            if (!seen) ++s.guest_incl[chain[i]];
          }
        }
      }
      const Module* m = FindModule(rip);
      const Where w = m ? m->where : kOther;
      ++s.samples;
      ++s.where[w];
      if (w == kExe) ++s.guest_lr[lr];
      if (caller) ++s.caller[caller];
      if (caller2) ++s.caller2[caller2];
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(rip, &image_base, nullptr);
      ++s.host_fn[fe ? uintptr_t(image_base + fe->BeginAddress) : rip];
    }
    const Clock::time_point now = Clock::now();
    const double wall = std::chrono::duration<double>(now - window_start).count();
    if (wall < window_s) continue;
    const uint64_t frames = fcp::gametime::Frame() - frame0;
    FCPLOG("profile: window {:.1f} s, {} frames ({:.1f} fps), level {} {:.1f} s", wall, frames, frames / wall,
           fcp::gametime::InLevel() ? fcp::gametime::LevelName() : std::string("-"), fcp::gametime::LevelSeconds());
    for (auto& [id, s] : threads) {
      const uint64_t cpu = ThreadCpu(s.handle);
      const double cpu_pct = 100.0 * double(cpu - s.cpu100ns_at_window) / 1e7 / wall;
      s.cpu100ns_at_window = cpu;
      if (cpu_pct >= 3.0 && s.samples) {
        std::string where;
        for (int i = 0; i < kWhereCount; ++i)
          if (s.where[i]) where += std::format(" {} {:.0f}%", kWhereNames[i], 100.0 * s.where[i] / double(s.samples));
        FCPLOG("profile:   {} host {}: cpu {:.0f}% |{} | guest lr{} | host{} | outside exe, called from{} | and "
               "above{}",
               s.name, id, cpu_pct, where, Top(s.guest_lr, s.samples, 6, true), Top(s.host_fn, s.samples, 6, false),
               Top(s.caller, s.samples, 6, false), Top(s.caller2, s.samples, 6, false));
        FCPLOG("profile:   {} guest self{}", s.name, Top(s.guest_self, s.samples, g_top, true));
        FCPLOG("profile:   {} guest incl{}", s.name, Top(s.guest_incl, s.samples, g_top, true));
        FCPLOG("profile:   {} guest edge{}", s.name, TopEdges(s.guest_edge, s.samples, g_top));
        FCPLOG("profile:   {} host top{}", s.name, Top(s.host_fn, s.samples, g_top, false));
      }
      s.guest_self.clear();
      s.guest_incl.clear();
      s.guest_edge.clear();
      s.samples = 0;
      std::fill(std::begin(s.where), std::end(s.where), 0);
      s.guest_lr.clear();
      s.host_fn.clear();
      s.caller.clear();
      s.caller2.clear();
    }
    window_start = now;
    frame0 = fcp::gametime::Frame();
  }
  for (auto& [id, s] : threads)
    if (s.handle) CloseHandle(s.handle);
  timeEndPeriod(1);
}

}  // namespace

void Start(rex::kernel::TitleRuntime& runtime) {
  const int64_t interval = fcp::env::Int("FCP_PROFILE", 0);
  if (interval <= 0 || g_thread.joinable()) return;
  g_runtime = &runtime;
  g_depth = int(std::clamp<int64_t>(fcp::env::Int("FCP_PROFILE_DEPTH", kMaxDepth), 0, kMaxDepth));
  g_top = int(std::clamp<int64_t>(fcp::env::Int("FCP_PROFILE_TOP", 6), 1, 200));
  if (g_guest.by_host.empty()) g_guest.Load();
  const double window = double(std::max<int64_t>(1, fcp::env::Int("FCP_PROFILE_WINDOW", 10)));
  FCPLOG("FCP_PROFILE={}: sampling the guest threads every {} ms, report every {:.0f} s", interval, interval, window);
  g_stop = false;
  g_thread = std::thread(Loop, int(interval), window);
}

void Stop() {
  g_stop = true;
  if (g_thread.joinable()) g_thread.join();
  g_runtime = nullptr;
}

}  // namespace fcp::profile
