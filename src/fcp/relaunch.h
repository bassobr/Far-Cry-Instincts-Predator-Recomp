// Relaunch of the title as a new process (XamLoaderLaunchTitle), design of Far Cry Instincts Recomp:
//  - default.xex: start predator.exe again with the launch data the title set (XamLoaderSetLaunchData);
//    no CREATE_SUSPENDED; the named hand-over event exists before CreateProcess; the old window stays
//    until the new process shows its first frame (first D3DDevice_Swap while there is no image) and
//    the new window takes over the old placement; if the new process dies (or does not show a frame
//    within FCP_RELAUNCH_TIMEOUT seconds, default 120) the old one shows an error and exits.
//  - Closing the old window (any shutdown request) during the hand-over cancels the relaunch first:
//    the new process is terminated, by handle.
//  - game:\ige.xex (map editor, not supported): logged, the new process starts default.xex with the
//    launch data the menu prepared for the editor (profile, controller port), i.e. back to the main
//    menu, and shows a short non-blocking notice in its window title.
//  - NULL path (dashboard): clean shutdown.
// Generation 0 logs to predator.log, generation n to predator.gen<n>.log in the user folder (fcp/user_paths.h; both
// processes write at the same time during the hand-over). The window goes over with its place and mode: windowed,
// maximized or fullscreen/exclusive on its monitor, set before the new window shows (rexglue change 110).
// Values taken from predator.toml are not inherited: the new process reads the file itself.
//
// Switches: FCP_NO_RELAUNCH=1 (SDK behaviour: the title only exits), FCP_RELAUNCH_TIMEOUT=<s>,
// FCP_TEST_RELAUNCH_FAIL=<g> (the process of generation g exits with code 7 before its first frame),
// FCP_TEST_RELAUNCH_DELAY_MS=<ms> (a relaunched process waits before its title starts: longer hand-over),
// FCP_DASHBOARD_PROMPT=1 (the SDK "title exited" prompt instead of a clean shutdown on the dashboard).
// Internal hand-over variables (set by the parent): FCP_RELAUNCH_GEN, FCP_RELAUNCH_EVENT,
// FCP_RELAUNCH_DATA (hex), FCP_RELAUNCH_WINDOW, FCP_RELAUNCH_NOTICE, FCP_RELAUNCH_PARENT.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rex {
class SettingsRegistry;
}

namespace fcp::relaunch {

/** Reads the hand-over variables. First call in OnConfigure, before the log starts. */
void Configure();

/** Relaunch generation of this process (0 = started by the user). */
int Generation();

/** Log file name for this generation. */
std::string LogName();

/** Logs the hand-over state of this process (after the log started). */
void LogStart();

/** Child: the launch data handed over by the parent, null when there is none. */
const std::vector<uint8_t>* HandoverData();

/** Child: window placement in client pixels for ui.window_width/height, 0 when none. */
int HandoverClientWidth();
int HandoverClientHeight();

/** Child, OnConfigure: the previous window as SDK settings before the window opens (environment layer, over
    predator.toml): windowed size, client origin and maximized state (ui.window_position, ui.window_maximized,
    rexglue change 110), or fullscreen/exclusive on the previous monitor. FCP_RELAUNCH_KEEP_WINDOW=0: only the
    size, as before (the window opens centered and moves at title launch). */
void ApplyWindowHandover(rex::SettingsRegistry& settings);

/** Child, title launch: test failure switch, window placement, notice. */
void OnTitleLaunch();

/** Child: first D3DDevice_Swap (from the watchdog). Signals the parent. */
void OnFirstSwap();

/** Import hooks (guest thread): the title's launch data and launch target. */
void OnSetLaunchData(const uint8_t* data, uint32_t size);
void OnLaunchTitle(bool has_path, const std::string& path, uint32_t flags);

/** Parent: a relaunch is running and not yet handed over or failed. */
bool Pending();

/** Parent: true once the relaunch took over this process's role (the exit is part of it). */
bool Active();

/** Shutdown: cancels a pending relaunch (terminates the new process). */
void CancelIfPending(const char* reason);

/** Win32 view of the own window for the log (rectangle, client, style, monitor, FULLSCREEN/WINDOW/MAXIMIZED). */
std::string DescribeOwnWindow();

/** Dashboard target (NULL path) seen. */
bool DashboardRequested();

}  // namespace fcp::relaunch
