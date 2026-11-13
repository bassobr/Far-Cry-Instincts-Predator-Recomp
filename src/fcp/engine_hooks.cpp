// Logging hooks on engine anchors (names from [function_names] in config/predator/default.toml; no
// addresses here). The original always runs. They prove the main loop (frame timer, Bink, commands) and
// count runtime shader compiles. FCP_ENGINE_LOG_FIRST=<n> logs the first n calls of each (default 5);
// every call of the rare ones (BinkOpen, Video_Open, PushCommand, Relaunch, map editor) is logged.
#include "fcp/engine_hooks.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <format>
#include <string>

#include <rex/ppc/hook.h>

#include "fcp/devstart.h"
#include "fcp/gametime.h"
#include "fcp/env.h"
#include "fcp/gpu_wait.h"
#include "fcp/guest.h"
#include "fcp/level_test.h"
#include "fcp/log.h"
#include "fcp/shader_compile_cache.h"
#include "fcp/state_trace.h"
#include "fcp/watchdog.h"

// clang-format off
#define FCP_ENGINE_HOOK_LIST(X)   \
  X(Game_Run, true)               \
  X(Game_MainLoop, false)         \
  X(Game_DispatchCommands, false) \
  X(Game_PushCommand, true)       \
  X(FrameTimer_Update, false)     \
  X(FrameTimer_GetFrameTime, false) \
  X(System_Update, false)         \
  X(BinkOpen, true)               \
  X(Video_Open, true)             \
  X(Engine_CompileShader, true)   \
  X(Game_Relaunch, true)          \
  X(Menu_LaunchMapEditor, true)   \
  X(Game_ReadLaunchData, true)    \
  X(Game_NoProfileToMenu, true)   \
  X(ProfileMgr_SelectUser, true)  \
  X(Profile_WriteLaunchData, true) \
  X(Session_Setup, true)          \
  X(Session_Create, true)         \
  X(Profile_CheckStored, true)    \
  X(Game_OnSystemUi, true)        \
  X(Weapon_Fire, false)           \
  X(Bink_NextFrame, false)
// clang-format on

namespace fcp::engine {
namespace {

enum Id : int {
#define FCP_X(name, always) k_##name,
  FCP_ENGINE_HOOK_LIST(FCP_X)
#undef FCP_X
      kCount
};

struct Info {
  const char* name;
  bool always;
};
constexpr std::array<Info, kCount> kInfo = {{
#define FCP_X(name, always) {#name, always},
    FCP_ENGINE_HOOK_LIST(FCP_X)
#undef FCP_X
}};

std::array<std::atomic<uint64_t>, kCount> g_calls{};
uint64_t g_log_first = 5;
bool g_configured = false;

// A register as a C string when it points at at least three printable characters.
std::string MaybeString(uint8_t* base, uint32_t address) {
  if (!fcp::guest::Plausible(address)) {
    return {};
  }
  std::string s = fcp::guest::CString(base, address, 120);
  if (s.size() < 3 || s.find('?') != std::string::npos) {
    return {};
  }
  return s;
}

std::string Strings(const PPCContext& c, uint8_t* base) {
  std::string out;
  const uint32_t regs[4] = {c.r3.u32, c.r4.u32, c.r5.u32, c.r6.u32};
  for (int i = 0; i < 4; ++i) {
    if (std::string s = MaybeString(base, regs[i]); !s.empty()) {
      out += std::format(" r{}=\"{}\"", 3 + i, s);
    }
  }
  return out;
}

// [game] skip_intro / FCP_SKIP_INTRO: the start videos are not played. Video_Open
// (0x8233FFA0, config) plays a Bink video to its end and returns nothing; its two start callers (the boot after
// the Ubisoft logo, 0x82315F54, and the intro 0x8220CDA4) go on right after the call. Skipping the whole player keeps
// its own pairs (begin/end at 0x82626AC0/0x82626B98, BinkOpen/BinkClose) intact; a failing BinkOpen would leave the
// player through an early return without its end call. Only Ubisoft.bik and Intro_FCI.bik: the menu backgrounds
// (StartMenu_boat, MenusBack_sun*) and the attract mode stay. Read at every video, so the menu switch holds from
// the next video on. Original: FCP_SKIP_INTRO=0 (default).
bool SkipIntroVideo(uint8_t* base, uint32_t name) {
  if (!fcp::env::Flag("FCP_SKIP_INTRO")) return false;
  std::string path = MaybeString(base, name);
  for (char& c : path) c = char(std::tolower(static_cast<unsigned char>(c)));
  const std::string file = path.substr(path.find_last_of("\\/:") + 1);
  if (file != "ubisoft.bik" && file != "intro_fci.bik") return false;
  FCPLOG("video: {} skipped (skip_intro)", path);
  return true;
}

template <Id kId>
void Run(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  if (!g_configured) {
    g_log_first = static_cast<uint64_t>(fcp::env::Int("FCP_ENGINE_LOG_FIRST", 5));
    g_configured = true;
  }
  const uint64_t n = g_calls[kId].fetch_add(1, std::memory_order_relaxed) + 1;
  const bool log = n <= g_log_first || (kInfo[kId].always && n <= 2000);
  uint32_t lr = static_cast<uint32_t>(ctx.lr);
  if (log) {
    FCPLOG("engine > {} #{} f{} r3={:08X} r4={:08X} r5={:08X} r6={:08X} f1={:g} lr={:08X}{}",
           kInfo[kId].name, n, fcp::watch::SwapCount(), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
           ctx.r6.u32, ctx.f1.f64, lr, Strings(ctx, base));
  }
  if constexpr (kId == k_Game_MainLoop) {
    fcp::devstart::OnMainLoop(ctx, base);  // FCP_TEST_COMMAND, FCP_TEST_MAPEDITOR
    fcp::gametime::OnMainLoop(ctx, base);  // frame clock, limiter, hitches, state trace
    fcp::level_test::OnMainLoop(ctx, base);  // FCP_TEST_TELEPORT (level clock)
  }
  if constexpr (kId == k_Game_PushCommand) {
    fcp::gametime::OnPushCommand(base, ctx.r4.u32, lr);  // FCP_TRACE_COMMANDS
  }
  if constexpr (kId == k_Game_DispatchCommands) {
    fcp::gametime::OnDispatchBegin(base, ctx.r4.u32);  // level start, FCP_TRACE_COMMANDS
  }
  if constexpr (kId == k_Weapon_Fire) {
    fcp::state::OnWeaponFire(base, ctx.r4.u32);  // FCP_TRACE_STATE: bullets of the local player
  }
  const uint32_t entry_r3 = ctx.r3.u32;
  if constexpr (kId == k_Engine_CompileShader) {
    fcp::shadercache::Compile(ctx, base, original);  // FCP_SHADER_COMPILE_CACHE
  } else if (kId == k_Video_Open && SkipIntroVideo(base, ctx.r3.u32)) {
    // skip_intro: the player is not run, as if the video had ended at once (both callers go on without a result;
    // r3 = 0 as the original leaves it at the end of a video, measured)
    ctx.r3.u64 = 0;
  } else {
    original(ctx, base);
  }
  if constexpr (kId == k_Game_ReadLaunchData) {
    fcp::devstart::AfterReadLaunchData(ctx, base, entry_r3);  // FCP_START_LEVEL: game session
  }
  if constexpr (kId == k_Game_DispatchCommands) {
    fcp::gametime::OnDispatchEnd();
  }
  if constexpr (kId == k_FrameTimer_Update) {
    fcp::gametime::OnFrameTimer(base);
  }
  if constexpr (kId == k_Game_Run) {
    fcp::devstart::OnGameRunReturned(ctx, base);  // Quit-Yes: the title left its main loop
  }
  if (log && kId != k_Game_Run && kId != k_Game_MainLoop) {
    FCPLOG("engine < {} #{} -> r3={:08X} f1={:g}", kInfo[kId].name, n, ctx.r3.u32, ctx.f1.f64);
  }
}

}  // namespace

void WriteSummary() {
  fcp::shadercache::WriteSummary();
  fcp::gpuwait::WriteSummary();
  for (int i = 0; i < kCount; ++i) {
    FCPLOG("engine count {:>10} {}", g_calls[i].load(), kInfo[i].name);
  }
}

uint64_t CountOf(const char* name) {
  for (int i = 0; i < kCount; ++i) {
    if (std::strcmp(kInfo[i].name, name) == 0) {
      return g_calls[i].load(std::memory_order_relaxed);
    }
  }
  return 0;
}

}  // namespace fcp::engine

#define FCP_X(name, always)                                                  \
  REX_EXTERN(__imp__##name);                                                 \
  REX_HOOK_RAW(name) {                                                       \
    ::fcp::engine::Run<::fcp::engine::k_##name>(ctx, base, __imp__##name);   \
  }
FCP_ENGINE_HOOK_LIST(FCP_X)
#undef FCP_X
