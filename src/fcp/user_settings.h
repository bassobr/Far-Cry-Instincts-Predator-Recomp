// The player's settings file predator.toml in the user folder.
//
// Every key maps to an environment switch the modules already read (FCP_FPS, FCP_RENDER_BACKEND, ...). A switch
// set in the environment wins over the file (tests); a key the environment leaves open is written into this
// process's environment, so every module sees one value. Display keys go to the SDK's ui.* settings (file layer).
// FCP_SETTINGS_FILE=<path> uses another file, FCP_SETTINGS_FILE=none none at all (built-in defaults).
// Alt+Enter switches between the window and fullscreen and stores the mode in the file.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace rex {
class SettingsRegistry;
}
namespace rex::ui {
class IUISystem;
}

namespace fcp::settings {

/** Reads (or creates) the file and fills the environment. After fcp::paths::Configure and the log start. */
void Load();

/** Display keys to the SDK (ui.fullscreen, ui.fullscreen_exclusive, ui.window_width/height, ui.monitor). */
void Apply(rex::SettingsRegistry& settings);

/** Alt+Enter: window <-> fullscreen. */
void AttachUI(rex::ui::IUISystem& ui);
void DetachUI();

/** True when this process set the variable from the file (a relaunch child reads the file itself). */
bool FromFile(std::string_view env_name);

// ---- for the settings menu (fcp/settings_menu.h) ----

/** The value as the file has it (or the default), as text without quotes. */
std::string Value(std::string_view section, std::string_view key);

/** "FCP_X=value" when a variable from outside the file decides this key (it wins over the file), else empty. */
std::string EnvironmentOverride(std::string_view section, std::string_view key);

/**
 * Sets a key: the file (comments stay), the remembered value and this process's environment variable (unless
 * one from outside wins). The caller applies what the modules can change at once. Returns an error text or "".
 */
std::string Store(std::string_view section, std::string_view key, const std::string& text);

/** The settings file (empty: none). */
std::filesystem::path File();

}  // namespace fcp::settings
