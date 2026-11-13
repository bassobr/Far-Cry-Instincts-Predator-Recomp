#include "fcp/imports.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include "fcp/devstart.h"
#include "fcp/env.h"
#include "fcp/file_patch.h"
#include "fcp/input_script.h"
#include "fcp/pc_input.h"
#include "fcp/guest.h"
#include "fcp/input_trace.h"
#include "fcp/keyboard_text.h"
#include "fcp/sound_trace.h"
#include "fcp/relaunch.h"
#include "fcp/log.h"
#include "fcp/watchdog.h"

namespace fcp::imports {
namespace {

struct Slot {
  std::atomic<uint64_t> calls{0};  // shared counter: every call when g_shared_count, else only traced imports
  std::atomic<PPCFunc*> real{nullptr};
  std::atomic<uint8_t> first{0};  // 0 never called, 1 first call running (its return is logged), 2 done
};

// kCount is only known at link time; allocate on first use (no function static: its guard reads the TLS epoch on
// every call).
std::atomic<Slot*> g_slots{nullptr};
Slot* Slots() {
  if (Slot* s = g_slots.load(std::memory_order_acquire)) {
    return s;
  }
  static std::mutex m;
  std::lock_guard lock(m);
  if (!g_slots.load(std::memory_order_relaxed)) {
    g_slots.store(new Slot[kCount], std::memory_order_release);
  }
  return g_slots.load(std::memory_order_relaxed);
}

int g_mode = 0;          // FCP_TRACE_IMPORTS
bool g_files = false;    // FCP_TRACE_FILES

// Call counts: one shared atomic per import cost a locked add on a cache
// line every guest thread writes (RtlEnter/LeaveCriticalSection ~0.5 million calls/s each in 21_dunea). Each
// thread now counts in its own block (plain load and store, single writer); readers add up all blocks. Imports
// whose every call is logged with its number keep the shared counter. FCP_IMPORT_SHARED_COUNT=1 = the shared
// counter for every call (before).
bool g_shared_count = false;
std::vector<uint8_t> g_imported;  // per wrapper: predator_code.dll imports it (MarkImported)
std::vector<uint8_t> g_exact;  // per import: shared counter (call number in the log)
std::vector<uint8_t> g_quiet;  // per import: nothing to describe or observe after the first call
std::mutex g_blocks_mutex;
std::vector<std::atomic<uint64_t>*> g_blocks;  // one block of kCount counters per thread, never freed
thread_local std::atomic<uint64_t>* t_block = nullptr;

std::atomic<uint64_t>* ThreadBlock() {
  if (!t_block) {
    auto* block = new std::atomic<uint64_t>[kCount];
    for (size_t i = 0; i < kCount; ++i) block[i].store(0, std::memory_order_relaxed);
    std::lock_guard lock(g_blocks_mutex);
    g_blocks.push_back(block);
    t_block = block;
  }
  return t_block;
}

uint64_t Calls(size_t index) {
  uint64_t n = Slots()[index].calls.load(std::memory_order_relaxed);
  std::lock_guard lock(g_blocks_mutex);
  for (const std::atomic<uint64_t>* block : g_blocks) n += block[index].load(std::memory_order_relaxed);
  return n;
}

HMODULE KernelModule() {
  static HMODULE module = [] {
    for (const wchar_t* name : {L"rexkernel.dll", L"rexkernelrd.dll", L"rexkerneld.dll"}) {
      if (HMODULE m = GetModuleHandleW(name)) {
        return m;
      }
    }
    return HMODULE(nullptr);
  }();
  return module;
}

std::string Regs(const PPCContext& ctx) {
  return std::format("r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} lr={:08X}", ctx.r3.u32,
                     ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, static_cast<uint32_t>(ctx.lr));
}

bool Is(size_t index, const char* name) {
  return std::strcmp(kTable[index].name, name) == 0;
}

// Imports that are always described (rare and important for the boot report).
bool AlwaysDescribe(size_t index) {
  return Is(index, "XamLoaderLaunchTitle") || Is(index, "XamLoaderSetLaunchData") ||
         Is(index, "XamLoaderGetLaunchData") || Is(index, "XamLoaderTerminateTitle") ||
         Is(index, "HalReturnToFirmware") || Is(index, "KeBugCheck") || Is(index, "KeBugCheckEx") ||
         Is(index, "RtlRaiseException") || Is(index, "XamShowMessageBoxUI") ||
         Is(index, "ExCreateThread") || Is(index, "MmAllocatePhysicalMemoryEx") ||
         Is(index, "XamLoaderGetLaunchDataSize") || Is(index, "XamUserWriteProfileSettings") ||
         Is(index, "XamUserReadProfileSettings") ||
         // Save games: rare calls, always in the log.
         Is(index, "XamContentCreate") || Is(index, "XamContentCreateEx") ||
         Is(index, "XamContentClose") || Is(index, "XamContentDelete") ||
         Is(index, "XamContentCreateEnumerator") || Is(index, "XamShowDeviceSelectorUI") ||
         Is(index, "XamShowKeyboardUI");
}

bool IsFileCall(size_t index) {
  return Is(index, "NtCreateFile") || Is(index, "NtOpenFile") ||
         Is(index, "NtQueryFullAttributesFile") || Is(index, "NtQueryDirectoryFile");
}

// Extra detail for selected imports (arguments read at entry).
std::string Detail(size_t index, PPCContext& ctx, uint8_t* base) {
  using namespace fcp::guest;
  if (Is(index, "XamLoaderLaunchTitle")) {
    return std::format(" path=\"{}\" flags={:08X}", CString(base, ctx.r3.u32), ctx.r4.u32);
  }
  if (Is(index, "XamLoaderSetLaunchData")) {
    return std::format(" size={} text=\"{}\"", ctx.r4.u32, CString(base, ctx.r3.u32, 200));
  }
  if (Is(index, "NtCreateFile") || Is(index, "NtOpenFile") || Is(index, "NtQueryFullAttributesFile")) {
    // NtCreateFile/NtOpenFile: r5 = OBJECT_ATTRIBUTES; NtQueryFullAttributesFile: r3.
    const uint32_t attributes = Is(index, "NtQueryFullAttributesFile") ? ctx.r3.u32 : ctx.r5.u32;
    if (Readable(base, attributes, 12)) {
      return std::format(" name=\"{}\"", AnsiString(base, U32(base, attributes + 4)));
    }
  }
  if (Is(index, "MmAllocatePhysicalMemoryEx")) {
    // (flags, size, protect, min, max, alignment)
    return std::format(" size={:X} protect={:08X} min={:08X} max={:08X} align={:X}", ctx.r4.u32,
                       ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
  }
  if (Is(index, "ExCreateThread")) {
    // ExCreateThread(handle*, stack_size, thread_id*, xapi_startup, start_address, start_context,
    // creation_flags): r3..r9.
    return std::format(" stack={:X} startup={:08X} start={:08X} ctx={:08X} flags={:08X}", ctx.r4.u32,
                       ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
  }
  if (Is(index, "XamContentCreate") || Is(index, "XamContentCreateEx") || Is(index, "XamContentDelete")) {
    // XamContentCreate(user, root, XCONTENT_DATA*, flags, ...); XamContentDelete(user, XCONTENT_DATA*, ...).
    // XCONTENT_DATA: device id, content type, display name WCHAR[128], file name CHAR[42] at +0x108.
    const bool del = Is(index, "XamContentDelete");
    const uint32_t data = del ? ctx.r4.u32 : ctx.r5.u32;
    if (Readable(base, data, 0x108 + 42)) {
      return std::format(" root=\"{}\" device={:08X} type={:08X} file=\"{}\" flags={:08X}",
                         del ? std::string() : CString(base, ctx.r4.u32, 32), U32(base, data),
                         U32(base, data + 4), CString(base, data + 0x108, 42), del ? 0u : ctx.r6.u32);
    }
  }
  if (Is(index, "XamUserWriteProfileSettings") && ctx.r5.u32 <= 8 &&
      Readable(base, ctx.r6.u32, 40 * ctx.r5.u32)) {
    // (title, user, count, XUSER_PROFILE_SETTING[count] (40 bytes: id +0x10, type +0x18, size +0x20), ...)
    std::string s = " settings:";
    for (uint32_t n = 0; n < ctx.r5.u32; ++n) {
      const uint32_t e = ctx.r6.u32 + 40 * n;
      s += std::format(" {:08X}/type {}/size {}", U32(base, e + 0x10), unsigned(base[e + 0x18]), U32(base, e + 0x20));
    }
    return s;
  }
  if (Is(index, "XamContentClose")) {
    return std::format(" root=\"{}\"", CString(base, ctx.r3.u32, 32));
  }
  if (Is(index, "RtlRaiseException") && Readable(base, ctx.r3.u32, 16)) {
    return std::format(" code={:08X} flags={:08X} address={:08X}", U32(base, ctx.r3.u32),
                       U32(base, ctx.r3.u32 + 4), U32(base, ctx.r3.u32 + 12));
  }
  return {};
}

thread_local std::string t_pending;

// Per import: bit 0 always describe, bit 1 file call, bit 2 input script, bit 3 launch data,
// bit 4 launch observers, bit 5 sound archive trace (fcp::sound_trace), bit 6 input trace,
// bit 7 notifications (XNotifyGetNext), bit 8 raw read patch (FCP_TEST_PATCH_RAW), bit 9 keyboard/mouse and
// the pad connection state (fcp::pc_input), bit 10 the keyboard text (fcp::keyboard_text, before the call).
std::vector<uint16_t> g_flags;

// --- XNotifyGetNext: every dequeued notification is logged (rare). The SDK queues a startup set into
// the first system listener (as Xenia): XN_SYS_UI on, XN_SYS_UI off, sign-in, input devices, ... On the
// console "UI on" means the guide is up; the startup pair is transient. In the -map path (dev start and
// every level restart, game +0x3FC = 1) the title's XN_SYS_UI handler answers "UI on" with
// "Switch pause handlemenu" and nothing resumes on "UI off", so the level started paused
// (found with the test tools). The first, startup "UI on" is therefore delivered as
// "UI off"; FCP_STARTUP_SYS_UI=1 keeps the SDK behaviour.
constexpr uint32_t kXnSysUi = 0x00000009;
bool g_startup_ui_fix = true;
std::atomic<bool> g_first_sys_ui_seen{false};
thread_local uint32_t t_entry_r6 = 0, t_entry_r7 = 0, t_entry_r8 = 0;

// --- Launch data the title gets from XamLoaderGetLaunchData when the SDK has none: the parent's data
// after a relaunch (fcp::relaunch), else FCP_START_LEVEL / FCP_LAUNCH_DATA (fcp::devstart).
bool g_provide_launch_data = false;
constexpr uint32_t kLaunchDataSize = 0x3F8;  // what Game::Run asks for (measured in the trace)
constexpr uint32_t kErrorNotFound = 0x490;

thread_local uint32_t t_entry_r3 = 0, t_entry_r4 = 0, t_entry_r5 = 0;
thread_local std::string t_open_name;  // NtCreateFile/NtOpenFile name for the sound trace

void ApplyLaunchData(size_t index, PPCContext& ctx, uint8_t* base) {
  if (ctx.r3.u32 != kErrorNotFound) {
    return;  // the title has launch data of its own (relaunch): leave it
  }
  const std::vector<uint8_t>* handover = fcp::relaunch::HandoverData();
  if (Is(index, "XamLoaderGetLaunchDataSize")) {
    if (fcp::guest::Readable(base, t_entry_r3, 4)) {
      const uint32_t be =
          __builtin_bswap32(handover ? static_cast<uint32_t>(handover->size()) : kLaunchDataSize);
      std::memcpy(fcp::guest::Ptr(base, t_entry_r3), &be, 4);
      ctx.r3.u64 = 0;
    }
    return;
  }
  if (t_entry_r4 == 0 || !fcp::guest::Readable(base, t_entry_r3, t_entry_r4)) {
    return;
  }
  uint8_t* buffer = fcp::guest::Ptr(base, t_entry_r3);
  if (handover) {
    // As XamLoaderGetLaunchData after a console relaunch: copy, at most the buffer size.
    const size_t n = std::min<size_t>(handover->size(), t_entry_r4);
    std::memcpy(buffer, handover->data(), n);
    ctx.r3.u64 = 0;
    FCPLOG("relaunch: gave the title the parent's launch data ({} of {} bytes)", n, handover->size());
    return;
  }
  if (fcp::devstart::FillLaunchData(buffer, t_entry_r4)) {
    ctx.r3.u64 = 0;
  }
}

void ObserveNotification(PPCContext& ctx, uint8_t* base) {
  if (ctx.r3.u32 == 0 || !fcp::guest::Readable(base, t_entry_r5, 4)) {
    return;  // nothing dequeued
  }
  const uint32_t id = fcp::guest::U32(base, t_entry_r5);
  const bool has_param = fcp::guest::Readable(base, t_entry_r6, 4);
  const uint32_t param = has_param ? fcp::guest::U32(base, t_entry_r6) : 0;
  std::string note;
  if (id == kXnSysUi && !g_first_sys_ui_seen.exchange(true)) {
    if (param != 0 && has_param && g_startup_ui_fix) {
      const uint32_t zero = 0;
      std::memcpy(fcp::guest::Ptr(base, t_entry_r6), &zero, 4);
      note = " -> delivered as 0 (startup notification of the SDK; FCP_STARTUP_SYS_UI=1 keeps it)";
    }
  }
  FCPLOG("notify: id={:08X} param={:08X} listener={:08X} (swaps={}){}", id, param, t_entry_r3,
         fcp::watch::SwapCount(), note);
}

void ObserveLaunch(size_t index, PPCContext& ctx, uint8_t* base) {
  if (Is(index, "XamLoaderSetLaunchData")) {
    const uint32_t size = std::min<uint32_t>(ctx.r4.u32, 0x10000);
    if (size && fcp::guest::Readable(base, ctx.r3.u32, size)) {
      fcp::relaunch::OnSetLaunchData(fcp::guest::Ptr(base, ctx.r3.u32), size);
    }
  } else if (Is(index, "XamLoaderLaunchTitle")) {
    const uint32_t name = ctx.r3.u32;
    fcp::relaunch::OnLaunchTitle(name != 0, name ? fcp::guest::CString(base, name) : std::string(),
                                 ctx.r4.u32);
  }
}

}  // namespace

void Configure() {
  g_mode = static_cast<int>(fcp::env::Int("FCP_TRACE_IMPORTS", 0));
  g_files = fcp::env::Flag("FCP_TRACE_FILES");
  g_flags.assign(kCount, 0);
  for (size_t i = 0; i < kCount; ++i) {
    g_flags[i] = uint16_t((AlwaysDescribe(i) ? 1 : 0) | (IsFileCall(i) ? 2 : 0));
  }
  g_startup_ui_fix = !fcp::env::Flag("FCP_STARTUP_SYS_UI");
  g_shared_count = fcp::env::Flag("FCP_IMPORT_SHARED_COUNT");
  g_exact.assign(kCount, 0);
  for (size_t i = 0; i < kCount; ++i) {
    g_exact[i] = uint8_t(g_mode >= 2 || (g_flags[i] & 1) || (g_files && (g_flags[i] & 2)));
  }
  fcp::input_trace::Configure();
  fcp::sound_trace::Configure();
  fcp::input_script::Configure();
  fcp::file_patch::Configure();
  // Always watch the launch imports: relaunch data, dev start, launch targets.
  g_provide_launch_data = true;
  for (size_t i = 0; i < kCount; ++i) {
    if (fcp::input_script::Active() && (Is(i, "XamInputGetState") || Is(i, "XamInputGetCapabilities"))) {
      g_flags[i] |= 4;
    }
    if (fcp::pc_input::Active() &&
        (Is(i, "XamInputGetState") || Is(i, "XamInputGetCapabilities") || Is(i, "XamInputSetState"))) {
      g_flags[i] |= 512;
    }
    if (fcp::file_patch::PatchRawReads() && Is(i, "NtReadFile")) {
      g_flags[i] |= 256;
    }
    if (g_provide_launch_data && (Is(i, "XamLoaderGetLaunchData") || Is(i, "XamLoaderGetLaunchDataSize"))) {
      g_flags[i] |= 8;
    }
    if (Is(i, "XamLoaderSetLaunchData") || Is(i, "XamLoaderLaunchTitle")) {
      g_flags[i] |= 16;
    }
    if (fcp::keyboard_text::Enabled() && Is(i, "XamShowKeyboardUI")) {
      g_flags[i] |= 1024;
    }
    if (Is(i, "XNotifyGetNext")) {
      g_flags[i] |= 128;
    }
    if (fcp::sound_trace::Enabled() && (Is(i, "NtCreateFile") || Is(i, "NtOpenFile") ||
                                        Is(i, "NtReadFile") || Is(i, "NtClose") ||
                                        Is(i, "NtSetInformationFile"))) {
      g_flags[i] |= 32;
    }
    if (fcp::input_trace::Enabled() && (Is(i, "XamInputGetState") || Is(i, "XamInputSetState"))) {
      g_flags[i] |= 64;
    }
  }
  // After the first call these need nothing but the count (Enter would return false): flags 2 only matter with
  // FCP_TRACE_FILES, every other flag observes or describes calls.
  g_quiet.assign(kCount, 0);
  for (size_t i = 0; i < kCount; ++i) {
    const uint16_t active = uint16_t(g_flags[i] & ~(g_files ? 0 : 2));
    g_quiet[i] = uint8_t(!g_shared_count && !g_exact[i] && g_mode < 2 && active == 0);
  }
}

PPCFunc* Resolve(size_t index) {
  Slot& slot = Slots()[index];
  if (PPCFunc* f = slot.real.load(std::memory_order_acquire)) {
    return f;
  }
  const std::string symbol = std::string("__imp__") + kTable[index].name;
  auto* f = reinterpret_cast<PPCFunc*>(GetProcAddress(KernelModule(), symbol.c_str()));
  if (!f) {
    FCPLOG("FATAL import {} not exported by the kernel DLL", kTable[index].name);
    fcp::log::Drain(std::chrono::milliseconds(1000));
    std::abort();
  }
  slot.real.store(f, std::memory_order_release);
  return f;
}

bool Enter(size_t index, PPCContext& ctx, uint8_t* base) {
  Slot& slot = Slots()[index];
  // Quiet imports (no trace, no observer) after their first call: count and go.
  if (index < g_quiet.size() && g_quiet[index] && slot.first.load(std::memory_order_relaxed) != 0) {
    std::atomic<uint64_t>& c = ThreadBlock()[index];
    c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    return false;
  }
  uint64_t n = 0;
  bool first;
  if (g_shared_count || (index < g_exact.size() && g_exact[index])) {
    n = slot.calls.fetch_add(1, std::memory_order_relaxed);
    first = n == 0;
    if (first) slot.first.store(1, std::memory_order_relaxed);
  } else {
    std::atomic<uint64_t>& c = ThreadBlock()[index];
    c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    uint8_t never = 0;
    first = slot.first.load(std::memory_order_relaxed) == 0 &&
            slot.first.compare_exchange_strong(never, 1, std::memory_order_relaxed);
  }
  const uint16_t flags = index < g_flags.size() ? g_flags[index] : 0;
  const bool describe =
      (first && g_mode >= 1) || g_mode >= 2 || (flags & 1) || (g_files && (flags & 2));
  if (first) {
    fcp::watch::SetGuestBase(base);
    // Always: first use of each import, with the stub marker (the report's import table).
    FCPLOG("import first {}!{}{} {}{}", kTable[index].module, kTable[index].name,
           kTable[index].kind == Kind::kStub ? " [STUB]" : "", Regs(ctx), Detail(index, ctx, base));
  } else if (describe) {
    t_pending = Detail(index, ctx, base);
    FCPLOG("import call {} #{} {}{}", kTable[index].name, n + 1, Regs(ctx), t_pending);
  }
  if (flags & (12 | 32 | 64 | 128 | 256 | 512)) {
    t_entry_r3 = ctx.r3.u32;
    t_entry_r4 = ctx.r4.u32;
    t_entry_r5 = ctx.r5.u32;
    t_entry_r6 = ctx.r6.u32;
    t_entry_r7 = ctx.r7.u32;
    t_entry_r8 = ctx.r8.u32;
  }
  bool observe = (flags & 64) != 0;
  if (flags & 32) {
    if (Is(index, "NtReadFile")) {
      fcp::sound_trace::OnRead(ctx.r3.u32, ctx.r9.u32, base, ctx.r10.u32,
                               static_cast<uint32_t>(ctx.lr));
    } else if (Is(index, "NtClose")) {
      fcp::sound_trace::OnClose(ctx.r3.u32);
    } else if (Is(index, "NtSetInformationFile")) {
      fcp::sound_trace::OnSetInformation(ctx.r3.u32, ctx.r7.u32, base, ctx.r5.u32);
    } else if (fcp::guest::Readable(base, ctx.r5.u32, 12)) {
      // NtCreateFile/NtOpenFile: r3 = handle out, r5 = OBJECT_ATTRIBUTES (name at +4).
      t_open_name = fcp::guest::AnsiString(base, fcp::guest::U32(base, ctx.r5.u32 + 4));
      observe = true;
    }
  }
  if (flags & 1024) {
    fcp::keyboard_text::OnShowKeyboard(ctx, base);  // FCP_KEYBOARD_TEXT, before the SDK reads r5
  }
  if ((flags & 512) && Is(index, "XamInputSetState")) {
    // Rumble strength and focus pause (fcp::pc_input), before the SDK reads the vibration (r5).
    fcp::pc_input::BeforeSetState(ctx, base, ctx.r3.u32, ctx.r5.u32);
  }
  if (flags & 16) {
    ObserveLaunch(index, ctx, base);  // XamLoaderLaunchTitle does not return: act before the call
  }
  return describe || first || (flags & (12 | 128 | 256 | 512)) || observe;
}

void Leave(size_t index, PPCContext& ctx, uint8_t* base) {
  const uint16_t flags = index < g_flags.size() ? g_flags[index] : 0;
  // The return of the first call (as before: lost when a second call returns first).
  std::atomic<uint8_t>& first = Slots()[index].first;
  uint8_t running = 1;
  const bool first_ret = first.load(std::memory_order_relaxed) == 1 &&
                         first.compare_exchange_strong(running, 2, std::memory_order_relaxed);
  const bool log = first_ret || g_mode >= 2 || (flags & 1) || (g_files && (flags & 2));
  if (flags & 512) {
    // Keyboard/mouse and the connection state of user 0, before the input script.
    if (Is(index, "XamInputGetState")) {
      fcp::pc_input::OnGetState(ctx, base, t_entry_r3, t_entry_r5);
    } else if (Is(index, "XamInputSetState")) {
      fcp::pc_input::AfterSetState(base);  // the title's own values back (the trace below sees them)
    } else {
      fcp::pc_input::OnGetCapabilities(ctx, base, t_entry_r3, t_entry_r5);
    }
  }
  if (flags & 4) {
    // XamInputGetState / XamInputGetCapabilities (user r3, flags r4, state or caps r5).
    if (Is(index, "XamInputGetState")) {
      fcp::input_script::OnGetState(ctx, base, t_entry_r3, t_entry_r5);
    } else {
      fcp::input_script::OnGetCapabilities(ctx, base, t_entry_r3, t_entry_r5);
    }
  }
  if ((flags & 256) && ctx.r3.u32 == 0 && fcp::guest::Readable(base, t_entry_r7, 8)) {
    // NtReadFile(handle, event, apc, apc context, io status r7, buffer r8, length r9, offset r10).
    fcp::file_patch::OnRead(base, t_entry_r8, fcp::guest::U32(base, t_entry_r7 + 4));
  }
  if (flags & 8) {
    ApplyLaunchData(index, ctx, base);
  }
  if (flags & 128) {
    ObserveNotification(ctx, base);
  }
  if (flags & 64) {
    // After the script: what the title really receives.
    if (Is(index, "XamInputGetState")) {
      fcp::input_trace::OnGetState(t_entry_r3, ctx.r3.u32, base, t_entry_r5);
    } else {
      fcp::input_trace::OnSetState(t_entry_r3, ctx.r3.u32, base, t_entry_r5);
    }
  }
  if ((flags & 32) && !t_open_name.empty()) {
    if (ctx.r3.u32 == 0 && fcp::guest::Plausible(t_entry_r3)) {
      fcp::sound_trace::OnOpen(t_open_name, fcp::guest::U32(base, t_entry_r3));
    }
    t_open_name.clear();
  }
  if (log) {
    FCPLOG("import ret  {} -> r3={:08X}", kTable[index].name, ctx.r3.u32);
  }
}

void MarkImported(const std::vector<std::string>& names) {
  g_imported.assign(kCount, 0);
  size_t marked = 0;
  for (size_t i = 0; i < kCount; ++i) {
    if (std::binary_search(names.begin(), names.end(), std::string("__imp__") + kTable[i].name)) {
      g_imported[i] = 1;
      ++marked;
    }
  }
  FCPLOG("imports: the game code imports {} of {} kernel/XAM wrappers of the EXE", marked, kCount);
}

void WriteSummary() {
  std::vector<uint64_t> counts(kCount);
  std::vector<size_t> order;
  for (size_t i = 0; i < kCount; ++i) {
    counts[i] = Calls(i);
    // the wrappers exist for every export of the SDK; listed are the ones the game code imports (or calls)
    if (g_imported.empty() || g_imported[i] || counts[i]) order.push_back(i);
  }
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return counts[a] > counts[b]; });
  size_t used = 0, used_stub = 0, unused = 0;
  FCPLOG("import summary: {} imported functions", order.size());
  for (size_t i : order) {
    const uint64_t calls = counts[i];
    if (calls) {
      ++used;
      used_stub += kTable[i].kind == Kind::kStub;
    } else {
      ++unused;
    }
    FCPLOG("import count {:>10} {}!{}{}", calls, kTable[i].module, kTable[i].name,
           kTable[i].kind == Kind::kStub ? " [STUB]" : "");
  }
  FCPLOG("import summary: {} called ({} stubs), {} never called", used, used_stub, unused);
  fcp::input_trace::WriteSummary();
  fcp::sound_trace::WriteSummary();
}

uint64_t CountOf(const char* name) {
  for (size_t i = 0; i < kCount; ++i) {
    if (std::strcmp(kTable[i].name, name) == 0) {
      return Calls(i);
    }
  }
  return 0;
}

}  // namespace fcp::imports
