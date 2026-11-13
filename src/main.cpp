// Far Cry Instincts Predator (Xbox 360) recompiled for Windows x64: host entry point.
//
// App on the ReXGlue SDK (rex::app::TitleApp). The recompiled game code is not part of this executable: it is
// predator_code.dll, loaded (and on the first start built from the player's disc image) before the app is made
// (src/fcp/game_code.h). Title addresses live in config/predator/, never in
// this code.
//
// Boot layer: asynchronous log next to the EXE, import trace, logging D3D
// hooks, watchdog and run time limit. Every deviation from the SDK defaults is listed in OnConfigure.

#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>

#include <rex/app/title_app.h>
#include <rex/kernel/title_runtime.h>
#include <rex/logging/logger.h>
#include <rex/path_settings.h>
#include <rex/platform/file.h>
#include <rex/settings_registry.h>
#include <rex/version.h>

#include "fcp/cpu_stats.h"
#include "fcp/d3d_hooks.h"
#include "fcp/devstart.h"
#include "fcp/relaunch.h"
#include "fcp/virtual_pads.h"
#include "fcp/engine_hooks.h"
#include "fcp/env.h"
#include "fcp/file_patch.h"
#include "fcp/fp_stats.h"
#include "fcp/game_code.h"
#include "fcp/game_code_build.h"
#include "fcp/gametime.h"
#include "fcp/imports.h"
#include "fcp/level_test.h"
#include "fcp/log.h"
#include "fcp/pc_input.h"
#include "fcp/render_bridge.h"
#include "fcp/thread_profile.h"
#include "fcp/user_paths.h"
#include "fcp/user_settings.h"
#include "fcp/watchdog.h"
#include "fcp_version.h"

namespace {

const char* ExitReasonName(rex::kernel::TitleExitReason reason) {
  switch (reason) {
    case rex::kernel::TitleExitReason::kNormalExit:
      return "normal exit";
    case rex::kernel::TitleExitReason::kHardExit:
      return "hard exit (dashboard)";
    case rex::kernel::TitleExitReason::kTitleDataError:
      return "title data error";
    case rex::kernel::TitleExitReason::kBugCheck:
      return "bug check";
  }
  return "?";
}

// Sets a setting on a layer and logs it, so the log shows every value the boot layer changed.
void Set(rex::SettingsRegistry& settings, rex::SettingLayer layer, std::string_view name,
         std::string_view value, std::string_view why) {
  if (auto set = settings.SetText(layer, name, value); !set) {
    FCPLOG("setting {} = {} FAILED: {}", name, value, set.error().GetDescription());
    return;
  }
  FCPLOG("setting {} = {} ({}, {})", name, value,
         layer == rex::SettingLayer::kTitle ? "title layer" : "environment layer", why);
}

// Before the app: the user folder, the log and predator.toml (the disc image), then the game code, which the app
// needs to be made (TitleApp copies the PPCImage of the generated code).
void StartLog() {
  const std::filesystem::path exe_dir = rex::platform::GetExecutableFolder();
  fcp::relaunch::Configure();  // generation and hand-over data decide the log name
  fcp::paths::Configure();     // user folder: logs, saves, caches, settings
  fcp::log::Start(fcp::paths::Root(), fcp::relaunch::LogName());
  std::atexit([] { fcp::log::Drain(std::chrono::milliseconds(2000)); });

  FCPLOG("predator {} (commit {}{}, built {}), rexglue {} {}", FCP_BUILD_CONFIG, FCP_GIT_COMMIT,
         FCP_GIT_DIRTY ? "-dirty" : "", FCP_BUILD_TIME, REXGLUE_VERSION_STRING, REXGLUE_BUILD_PLATFORM);
  FCPLOG("exe folder: {}", exe_dir.string());
  const auto vars = fcp::env::AllFcp();
  FCPLOG("environment: {} FCP_*/REX_* variables", vars.size());
  for (const auto& [name, value] : vars) {
    FCPLOG("  {}={}", name, value.size() > 120 ? value.substr(0, 120) + "..." : value);
  }
  fcp::relaunch::LogStart();
  fcp::paths::LogSummary();
  fcp::paths::Migrate();
  fcp::settings::Load();  // predator.toml -> FCP_* switches the environment leaves open
}

}  // namespace

class PredatorApp : public rex::app::TitleApp {
 public:
  explicit PredatorApp(const rex::ppc::PPCImage& image) : TitleApp("predator", image) {}

 protected:
  // Native renderer: presents for the null Xenos driver (src/fcp/render_bridge.h, FCP_RENDERER=0 = off).
  void OnRegisterModules(std::vector<rex::IModule*>& modules) override {
    TitleApp::OnRegisterModules(modules);
    fcp::render::RegisterModule(modules);
  }

  rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override {
    no_dialogs_ = fcp::env::Flag("FCP_NO_DIALOGS");

    // All SDK messages go through the asynchronous log; the SDK's own synchronous file, console and
    // debugger sinks are off (log.flush_level=off: no flush on a warning in a guest thread).
    fcp::log::AttachSdkLogger();
    using L = rex::SettingLayer;
    Set(settings, L::kTitle, "log.file", "false", "the boot log replaces the SDK file");
    Set(settings, L::kTitle, "log.flush_level", "off", "no synchronous flush in guest threads");
    if (!fcp::env::Flag("FCP_LOG_DEBUGGER")) {
      Set(settings, L::kTitle, "log.debugger", "false", "FCP_LOG_DEBUGGER=1 keeps it");
    }
    // No GPU emulation (decision "direct D3D HLE"): the null Xenos driver runs PM4 for fences,
    // interrupts and the vblank, and skips draws. FCP_GPU_DRIVER=<name> overrides.
    const std::string gpu = fcp::env::Get("FCP_GPU_DRIVER");
    Set(settings, L::kTitle, "gpu.driver", gpu.empty() ? "null" : gpu, "no Xenos emulation");
    // Saves and profiles in the user folder; the SDK's own settings file (optional, advanced) next to predator.toml.
    Set(settings, L::kTitle, "app.user_root", fcp::paths::Utf8(fcp::paths::Saves()), "user folder");
    Set(settings, L::kTitle, "app.config_file", fcp::paths::Utf8(fcp::paths::Root() / "rexglue.toml"),
        "SDK settings, read only when present (predator.toml is the player's file)");
    // Window or fullscreen, size, monitor: predator.toml / FCP_DISPLAY_MODE, FCP_WINDOW_WIDTH/HEIGHT, FCP_MONITOR.
    Set(settings, L::kTitle, "ui.fullscreen", "false", "default without a settings value");
    fcp::settings::Apply(settings);
    // The disc image itself as game root: FCP_GAME_ISO or [game] image in predator.toml (wins over
    // title.game_root of rexglue.toml).
    if (const std::string iso = fcp::env::Get("FCP_GAME_ISO"); !iso.empty()) {
      Set(settings, L::kEnvironment, "title.game_root", iso, "FCP_GAME_ISO");
    }
    if (no_dialogs_) {
      Set(settings, L::kTitle, "kernel.headless", "true", "FCP_NO_DIALOGS: system dialogs answer at once");
    }
    // Command line the title sees (ExLoadedCommandLine; Game::Run parses -map <level>, -lpc<n> ...).
    if (const std::string cmd = fcp::env::Get("FCP_COMMAND_LINE"); !cmd.empty()) {
      Set(settings, L::kTitle, "kernel.command_line", cmd, "FCP_COMMAND_LINE");
    }
    if (fcp::env::Flag("FCP_MUTE")) {
      Set(settings, L::kTitle, "apu.mute", "true", "FCP_MUTE");
    }
    // Relaunch: the new window opens where the previous one was, in its mode (window, maximized, fullscreen on
    // its monitor), before it shows.
    fcp::relaunch::ApplyWindowHandover(settings);
    return rex::Ok();
  }

  rex::Result<void> OnTitleLaunch(rex::kernel::TitleRuntime& title) override {
    fcp::relaunch::OnTitleLaunch();
    fcp::devstart::Configure();
    fcp::devstart::SetRuntime(title);
    fcp::gametime::Configure();  // test clock first: the input script reads it
    fcp::level_test::Configure();  // FCP_TEST_TELEPORT
    fcp::pc_input::Configure(&GetInput());  // keyboard/mouse, pad connection (before the import flags)
    fcp::imports::Configure();
    fcp::virtual_pads::Start();  // FCP_VIRTUAL_PADS
    fcp::d3d::Configure();
    fcp::watch::Start(title);
    fcp::profile::Start(title);  // FCP_PROFILE
    fcp::cpu::Start(title);         // FCP_CPU_STATS
    fcp::settings::AttachUI(GetUI());  // Alt+Enter
    if (auto root = REX_SETTINGS().Find("title.game_root"); root) {
      FCPLOG("title launch: game root {}", (*root)->GetText());
    }
    if (auto user = REX_SETTINGS().Find("app.user_root"); user) {
      FCPLOG("title launch: user root (saves, content) {}", (*user)->GetText());
    }
    return rex::Ok();
  }

  void OnTitleExit(rex::kernel::TitleExitReason reason) override {
    FCPLOG("title exit: {} (swaps={})", ExitReasonName(reason), fcp::watch::SwapCount());
    if (fcp::relaunch::Active()) {
      // The relaunch decides: hand-over (exit), failure (error, exit) or cancel (window closed).
      FCPLOG("title exit: part of a relaunch, the window stays until the new process takes over");
      return;
    }
    if (fcp::relaunch::DashboardRequested() && !fcp::env::Flag("FCP_DASHBOARD_PROMPT")) {
      FCPLOG("title exit: dashboard requested, clean shutdown (FCP_DASHBOARD_PROMPT=1: SDK prompt)");
      rex::app::RequestShutdown(0);
      return;
    }
    if (fcp::devstart::TitleQuit()) {
      FCPLOG("title exit: the title quit (Quit-Yes), clean shutdown");
      rex::app::RequestShutdown(0);
      return;
    }
    if (no_dialogs_) {
      rex::app::RequestShutdown(0);
      return;
    }
    TitleApp::OnTitleExit(reason);
  }

  void ReportError(std::string_view message) override {
    FCPLOG("error: {}", message);
    if (no_dialogs_) {
      App::ReportError(message);
      return;
    }
    TitleApp::ReportError(message);
  }

  rex::platform::CrashReporterOptions GetCrashOptions() override {
    rex::platform::CrashReporterOptions options = TitleApp::GetCrashOptions();
    if (fcp::env::Flag("FCP_NO_DIALOGS")) {
      options.show_dialog = false;
    }
    return options;
  }

  // First step of the shutdown (window still there): a close request cancels a pending relaunch.
  void OnShutdownBegin() override {
    fcp::relaunch::CancelIfPending("shutdown requested before the hand-over");
    fcp::settings::DetachUI();
  }

  void OnShutdown() override {
    FCPLOG("shutdown: swaps={}", fcp::watch::SwapCount());
    fcp::watch::Stop();
    fcp::profile::Stop();
    fcp::cpu::Stop();
    fcp::gametime::Shutdown();
    fcp::file_patch::WriteSummary();
    fcp::fp_stats::WriteSummary();
    fcp::virtual_pads::Stop();
    fcp::imports::WriteSummary();
    fcp::pc_input::WriteSummary();
    fcp::render::Shutdown();
    fcp::d3d::WriteSummary();
    fcp::engine::WriteSummary();
    FCPLOG("log: {} lines written, {} dropped", fcp::log::LinesWritten(), fcp::log::LinesDropped());
    fcp::log::Drain(std::chrono::milliseconds(2000));
  }

 private:
  bool no_dialogs_ = false;
};

namespace {

// REX_TITLE_APP with the game code step first; `predator.exe --fcp-build-game-code <settings>` is the build of
// predator_code.dll the development build runs (src/fcp/game_code_build.h).
int PredatorEntry(const rex::CLIArgList& args) {
  if (int code = 0; fcp::game_code_build::RunCommand(code)) {
    return code;
  }
  StartLog();
  const rex::ppc::PPCImage* image = fcp::game_code::Prepare();
  if (!image) {
    FCPLOG("game code: not available, exit");
    fcp::log::Drain(std::chrono::milliseconds(2000));
    return 1;
  }
  PredatorApp app(*image);
  if (app.Init(args) && app.LaunchTitle()) {
    app.Run();
  }
  return app.Shutdown();
}

}  // namespace

REX_DETAIL_ENTRY_POINTS(PredatorEntry)
