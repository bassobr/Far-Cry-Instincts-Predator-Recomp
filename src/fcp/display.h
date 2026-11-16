// Display helpers: DPI awareness and scale per monitor, the monitor's video modes (exclusive fullscreen).
//
// DPI (docs/en/CONFIGURATION.md "Display scaling"): the process declares per-monitor DPI awareness v2 as its first
// step (before the first-start progress window; FCP_DPI_EARLY=0: only when SDL starts its video, as before). The
// built-in window size (1280x720) is meant at 100 %: at 125/150/200 % it is scaled by the monitor's scale, and a
// window moved to a monitor with another scale keeps its size on the screen (FCP_DPI_WINDOW=0: always 1280x720
// pixels, the size in pixels kept on a scale change, as before). Sizes set in predator.toml stay exact pixels.
// Test aid: FCP_TEST_DPI=<percent>[,<t>:<percent>...] stands for the monitor's scale (at start, and at t seconds
// after the title launch as if the window had moved to such a monitor); see docs/en/DEBUG-SWITCHES.md.
#pragma once

#include <string>
#include <vector>

namespace rex::ui {
class IUISystem;
}

namespace fcp::display {

/** First call of the process: per-monitor DPI awareness v2 (unless FCP_DPI_EARLY=0). No log yet. */
void DeclareDpiAware();

/** After the log is up: the awareness in force, the monitors and their scale. */
void LogStart();

/** Scale of the monitor the window opens on ([display] monitor: 0 = primary), 1.0 = 100 %. */
double StartScale(int monitor);

/** The built-in window client size (1280x720 at 100 %) for that monitor: scaled with its scale, at most the work
 *  area; exactly 1280x720 at 100 % or with FCP_DPI_WINDOW=0. */
void BuiltInWindowSize(int monitor, int& width, int& height);

/** A video mode of the monitor the window is on ("WxH@Hz"). */
struct Mode {
  int width = 0, height = 0, hz = 0;
  std::string Value() const;  // "2560x1440@120"
  std::string Label() const;  // "2560 x 1440 @ 120 Hz"
};

/** The modes of the window's monitor (else the primary), largest first, 48 Hz and more, 32-bit only. */
std::vector<Mode> MonitorModes(rex::ui::IUISystem* ui);

/** The monitor's desktop mode (the registry's: the one exclusive fullscreen returns to). */
Mode DesktopMode(rex::ui::IUISystem* ui);

/** DPI change listener (window keeps its size on the screen), FCP_TEST_DPI timeline. UI thread. */
void AttachUI(rex::ui::IUISystem& ui);
void DetachUI();

}  // namespace fcp::display
