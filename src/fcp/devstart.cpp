#include "fcp/devstart.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>

#include "fcp/env.h"
#include "fcp_addresses.h"
#include "fcp/guest.h"
#include "fcp/guest_call.h"
#include "fcp/log.h"
#include "fcp/relaunch.h"
#include "fcp/watchdog.h"

REX_EXTERN(__imp__Profile_ReadLaunchData);
REX_EXTERN(__imp__Profile_WriteLaunchData);
REX_EXTERN(__imp__Game_PushCommand);
REX_EXTERN(__imp__Menu_LaunchMapEditor);
REX_EXTERN(__imp__Session_Create);
REX_EXTERN(__imp__Session_Setup);
REX_EXTERN(__imp__Profile_Save);

namespace fcp::devstart {
namespace {

// Launch data layout as written by the game's initializer (0x82143838 in the image, see the report)
// and checked by Game_Run: magic at +0x3A0, i32 mode < 4 at +0x3AC, command (< 64 chars) at +0x3B2,
// i8 controller port < 100 at +0x3F2, u32 3 at +0x36C, profile block at +0x180.
constexpr uint32_t kSize = 0x3F8;
constexpr uint32_t kMagicAt = 0x3A0;
constexpr uint32_t kMagic = 0x46434931;  // 'FCI1'
constexpr uint32_t kTextAt = 0x3B2;
constexpr uint32_t kTextMax = 63;
constexpr uint32_t kPortAt = 0x3F2;
constexpr uint32_t kSessionTypeAt = 0x36C;
constexpr uint32_t kProfileBlockAt = 0x180;

struct TestCommand {
  double at;
  std::string text;
  bool done = false;
};

std::string g_command;       // command text in the launch data
bool g_carry_profile = false;  // FCP_START_LEVEL: identity hook on Profile_ReadLaunchData
int g_port = 0;
bool g_provided = false;     // the launch data was ours
bool g_carry_done = false;
bool g_session_done = false;
bool g_bad_magic = false;  // FCP_TEST_DASHBOARD
int g_campaign = 0;      // session +0x158/+0xC: 0 Instincts, 1 Evolution (menu field +0x418)
int g_session_type = 1;  // session +0x160: 1 as in the launch data of a story level restart (measured)
std::vector<TestCommand> g_tests;
double g_mapeditor_at = -1;
std::chrono::steady_clock::time_point g_t0;
rex::kernel::TitleRuntime* g_runtime = nullptr;
bool g_title_quit = false;

void PutU32(uint8_t* p, uint32_t v) {
  const uint32_t be = __builtin_bswap32(v);
  std::memcpy(p, &be, 4);
}

std::vector<std::string> Split(const std::string& text, char sep) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find(sep, pos);
    if (end == std::string::npos) {
      end = text.size();
    }
    out.push_back(text.substr(pos, end - pos));
    pos = end + 1;
  }
  return out;
}

void ParseStartLevel(const std::string& value) {
  const std::vector<std::string> parts = Split(value, ':');
  const std::string& level = parts[0];
  if (level.empty() || level.find(' ') != std::string::npos) {
    FCPLOG("FCP_START_LEVEL: '{}' ignored (no level name)", value);
    return;
  }
  if (parts.size() > 1 && !parts[1].empty() && parts[1] != level) {
    FCPLOG("FCP_START_LEVEL: mission '{}' refused: the game's -map path always uses the level name "
           "as mission ('{}')", parts[1], level);
  }
  std::string command = "-map " + level;
  if (parts.size() > 2 && !parts[2].empty()) {
    const char players = parts[2][0];
    if (parts[2].size() == 1 && players >= '1' && players <= '4') {
      command += std::string(" -lpc") + players;
    } else {
      FCPLOG("FCP_START_LEVEL: players '{}' ignored (1..4)", parts[2]);
    }
  }
  g_command = command;
  g_carry_profile = true;
  // Campaign by level number: 0x/1x Instincts, 2x/3x Evolution (menu: 00_training / 21_dunea);
  // multiplayer maps (M..) get 2, as the relaunch reader's multiplayer branch.
  const char c = level[0];
  g_campaign = (c == '2' || c == '3') ? 1 : (c == 'm' || c == 'M') ? 2 : 0;
  g_campaign = static_cast<int>(fcp::env::Int("FCP_START_CAMPAIGN", g_campaign));
  g_session_type = static_cast<int>(fcp::env::Int("FCP_START_SESSION_TYPE", g_session_type));
}

void ParseTests(const std::string& value) {
  for (const std::string& item : Split(value, '|')) {
    const size_t colon = item.find(':');
    if (colon == std::string::npos || colon + 1 >= item.size()) {
      if (!item.empty()) {
        FCPLOG("FCP_TEST_COMMAND: entry '{}' ignored (<s>:<command>)", item);
      }
      continue;
    }
    g_tests.push_back({std::strtod(item.c_str(), nullptr), item.substr(colon + 1)});
  }
}

std::string ProfileState(uint8_t* base, uint32_t profile) {
  if (!fcp::guest::Readable(base, profile, 0xD4)) {
    return std::format("profile {:08X} (unreadable)", profile);
  }
  std::string name;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint16_t c = fcp::guest::U16(base, profile + 2 + 2 * i);
    if (!c) {
      break;
    }
    name.push_back(c >= 0x20 && c < 0x7F ? char(c) : '?');
  }
  uint8_t* p = fcp::guest::Ptr(base, profile);
  return std::format("profile {:08X} name=\"{}\" +0={} +1(valid)={} user={} signin={} options b0={:02X} "
                     "bc={} c4={}",
                     profile, name, p[0], p[1], fcp::guest::U32(base, profile + 0x8C),
                     fcp::guest::U32(base, profile + 0x90), p[0xB0], fcp::guest::U32(base, profile + 0xBC),
                     fcp::guest::U32(base, profile + 0xC4));
}

}  // namespace

bool ActiveInGeneration(const char* gens_variable) {
  const std::string gens = fcp::env::Get(gens_variable);
  const int gen = fcp::relaunch::Generation();
  if (gens.empty()) {
    return gen == 0;
  }
  if (gens == "all") {
    return true;
  }
  for (const std::string& g : Split(gens, ',')) {
    if (!g.empty() && std::atoi(g.c_str()) == gen) {
      return true;
    }
  }
  return false;
}

void Configure() {
  g_t0 = std::chrono::steady_clock::now();
  const int gen = fcp::relaunch::Generation();
  if (const std::string level = fcp::env::Get("FCP_START_LEVEL"); !level.empty()) {
    if (gen == 0) {
      ParseStartLevel(level);
      g_port = static_cast<int>(std::clamp<int64_t>(fcp::env::Int("FCP_START_PORT", 0), 0, 3));
      FCPLOG("FCP_START_LEVEL: command \"{}\", controller port {}, profile carried over by the game",
             g_command, g_port);
    } else {
      FCPLOG("FCP_START_LEVEL: ignored in relaunch generation {} (the launch data comes from the game)",
             gen);
    }
  } else if (const std::string raw = fcp::env::Get("FCP_LAUNCH_DATA"); !raw.empty() && gen == 0) {
    g_command = raw.substr(0, kTextMax);
    g_port = -1;  // as the game's initializer: no controller port
    FCPLOG("FCP_LAUNCH_DATA: raw command \"{}\" (no profile carry-over)", g_command);
  }
  if (gen == 0 && fcp::env::Flag("FCP_TEST_DASHBOARD")) {
    // Test: launch data with a wrong magic; Game_Run answers with XLaunchNewImage(NULL) (dashboard).
    g_command = "-dashboard-test";
    g_port = -1;
    g_bad_magic = true;
    FCPLOG("FCP_TEST_DASHBOARD: launch data with a wrong magic (the game exits to the dashboard)");
  }
  if (ActiveInGeneration("FCP_TEST_COMMAND_GENS")) {
    ParseTests(fcp::env::Get("FCP_TEST_COMMAND"));
    for (const TestCommand& t : g_tests) {
      FCPLOG("FCP_TEST_COMMAND: \"{}\" at {} s (generation {})", t.text, t.at, gen);
    }
  }
  if (gen == 0) {
    if (const std::string at = fcp::env::Get("FCP_TEST_MAPEDITOR"); !at.empty()) {
      g_mapeditor_at = std::strtod(at.c_str(), nullptr);
      FCPLOG("FCP_TEST_MAPEDITOR: map editor launcher at {} s", g_mapeditor_at);
    }
  }
}

bool FillLaunchData(uint8_t* buffer, uint32_t size) {
  if (g_command.empty() || size < kSize) {
    return false;
  }
  std::memset(buffer, 0, size);
  PutU32(buffer + kMagicAt, g_bad_magic ? 0u : kMagic);
  PutU32(buffer + kSessionTypeAt, 3);
  buffer[kPortAt] = static_cast<uint8_t>(static_cast<int8_t>(g_port));
  std::memcpy(buffer + kTextAt, g_command.data(), std::min<size_t>(g_command.size(), kTextMax));
  g_provided = true;
  FCPLOG("launch data: gave the title \"{}\" port {} ({} byte buffer, game relaunch format)", g_command,
         g_port, size);
  return true;
}

void OnMainLoop(PPCContext& ctx, uint8_t* base) {
  if (g_tests.empty() && g_mapeditor_at < 0) {
    return;
  }
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
  const uint32_t game = ctx.r3.u32;
  for (TestCommand& t : g_tests) {
    if (!t.done && now >= t.at) {
      t.done = true;
      FCPLOG("FCP_TEST_COMMAND: pushing \"{}\" at {:.2f} s (swaps={}, game {:08X})", t.text, now,
             fcp::watch::SwapCount(), game);
      fcp::guest::Call(ctx, base, __imp__Game_PushCommand, {.r3 = game, .string_r4 = t.text});
    }
  }
  if (g_mapeditor_at >= 0 && now >= g_mapeditor_at) {
    g_mapeditor_at = -1;
    FCPLOG("FCP_TEST_MAPEDITOR: calling the map editor launcher at {:.2f} s (swaps={})", now,
           fcp::watch::SwapCount());
    fcp::guest::Call(ctx, base, __imp__Menu_LaunchMapEditor, {.r3 = game});
    FCPLOG("FCP_TEST_MAPEDITOR: the launcher returned");
  }
}

void OnGameRunReturned(PPCContext& ctx, uint8_t* /*base*/) {
  if (fcp::env::Flag("FCP_TITLE_EXIT_CRT")) {
    FCPLOG("title: Game_Run returned {} (swaps={}), FCP_TITLE_EXIT_CRT=1: the title's CRT exit runs", ctx.r3.u32,
           fcp::watch::SwapCount());
    return;
  }
  FCPLOG("title: Game_Run returned {} (swaps={}): the title quit its main loop; title exit instead of the "
         "CRT exit (FCP_TITLE_EXIT_CRT=1 runs it)",
         ctx.r3.u32, fcp::watch::SwapCount());
  if (!g_runtime) {
    return;
  }
  g_title_quit = true;
  // As a title that ends itself: terminate notifications, guest threads stop, this thread exits here and
  // never returns into the CRT exit. The app's OnTitleExit sees TitleQuit() and shuts down without a dialog.
  g_runtime->GetTitleExit().ExitTitle(rex::kernel::TitleExitReason::kNormalExit);
}

void SetRuntime(rex::kernel::TitleRuntime& runtime) {
  g_runtime = &runtime;
}

rex::kernel::TitleRuntime* Runtime() { return g_runtime; }

bool TitleQuit() {
  return g_title_quit;
}

void AfterReadLaunchData(PPCContext& ctx, uint8_t* base, uint32_t game) {
  if (!g_carry_profile || !g_provided || g_session_done) {
    return;
  }
  g_session_done = true;
  const uint32_t manager = fcp::guest::U32(base, fcp::addr::session_manager_ptr);
  if (!fcp::guest::Readable(base, manager, 0x34)) {
    FCPLOG("devstart: no session manager ({:08X}), no session created", manager);
    return;
  }
  if (const uint32_t existing = fcp::guest::U32(base, manager + 0x30); existing) {
    FCPLOG("devstart: a session exists already ({:08X}), left as is", existing);
    return;
  }
  // As the menu start (0x821B735C ff.): Session_Create(manager, campaign, type), then the session's
  // difficulty from the game object (+0x3F0, default 1 like the menu's default).
  fcp::guest::Call(ctx, base, __imp__Session_Create,
                   {.r3 = manager, .r4 = uint32_t(g_campaign), .r5 = uint32_t(g_session_type)});
  const uint32_t session = fcp::guest::U32(base, manager + 0x30);
  if (!fcp::guest::Readable(base, session, 0x164)) {
    FCPLOG("devstart: Session_Create left no session ({:08X})", session);
    return;
  }
  const uint32_t info = fcp::guest::U32(base, session + 0x158);
  const uint32_t difficulty = fcp::guest::U32(base, game + 0x3F0);
  if (fcp::guest::Readable(base, info, 0x14)) {
    PutU32(fcp::guest::Ptr(base, info + 0x10), difficulty);
  }
  // Then, as the menu: Session_Setup(manager, &id 0, "" , check flag set) names the save of the
  // session (+0x118); a level restart carries that name in the launch data (+0x370).
  fcp::guest::Call(ctx, base, __imp__Session_Setup, {.r3 = manager, .r6 = 1, .zero_r4_r5 = true});
  std::string save_name = fcp::guest::CString(base, session + 0x118, 40);
  FCPLOG("devstart: Session_Setup named the session save \"{}\"", save_name);
  FCPLOG("devstart: session {:08X} created: campaign {} ({}), type {}, difficulty {} (game default)",
         session, fcp::guest::U32(base, info + 0xC),
         g_campaign == 0 ? "Instincts" : g_campaign == 1 ? "Evolution" : "other",
         fcp::guest::U32(base, session + 0x160), difficulty);
  // Save the profile with the game's own function, as the menu path does before a level is played.
  // Without stored title settings the startup "sign-in changed" notification makes Profile_CheckStored
  // fail (20 bytes at profile +0x9C against setting 0x63E83FFF) and the title answers with "the gamer
  // profile in use has changed" and a return to the menu (testwerkzeuge.md, Fehler A).
  if (fcp::env::Flag("FCP_START_SAVE_PROFILE", true)) {
    const uint32_t manager_p = fcp::guest::U32(base, fcp::addr::profile_manager_ptr);
    const uint32_t profile =
        fcp::guest::Readable(base, manager_p, 0x18) ? fcp::guest::U32(base, manager_p + 0x14) : 0;
    if (fcp::guest::Readable(base, profile, 0xD4)) {
      fcp::guest::Call(ctx, base, __imp__Profile_Save, {.r3 = profile});
      FCPLOG("devstart: profile saved with the game's Profile_Save (FCP_START_SAVE_PROFILE=0 skips it)");
    } else {
      FCPLOG("devstart: no current profile ({:08X}), not saved", profile);
    }
  }
}

}  // namespace fcp::devstart

// Profile_ReadLaunchData(profile r3, launch data r4): with FCP_START_LEVEL, first write the selected
// profile's own block into the launch data (the game's serializer), so the read changes nothing.
REX_HOOK_RAW(Profile_ReadLaunchData) {
  using namespace fcp::devstart;
  const uint32_t profile = ctx.r3.u32;
  const uint32_t data = ctx.r4.u32;
  if (g_carry_profile && g_provided && !g_carry_done) {
    g_carry_done = true;
    FCPLOG("devstart: before Profile_ReadLaunchData: {}", ProfileState(base, profile));
    fcp::guest::Call(ctx, base, __imp__Profile_WriteLaunchData, {.r3 = profile, .r4 = data});
    if (fcp::guest::Readable(base, data + kProfileBlockAt, 12)) {
      const uint8_t* b = fcp::guest::Ptr(base, data + kProfileBlockAt);
      FCPLOG("devstart: profile block from the profile itself: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
             "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
    }
  }
  __imp__Profile_ReadLaunchData(ctx, base);
  if (g_carry_done && g_carry_profile) {
    FCPLOG("devstart: after Profile_ReadLaunchData: {}", ProfileState(base, profile));
  }
}
