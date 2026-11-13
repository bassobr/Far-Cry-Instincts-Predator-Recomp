// Keyboard and mouse as player 1's pad, direct mouse look, and the pad connection state the title sees
// PC comfort only: the pad path and the game's look code run unchanged.
//
// Keyboard: the bindings (predator_input.toml next to the EXE, written with the defaults when missing) press
// pad buttons, triggers and stick directions of user 0, merged with a real pad (both work at once, as in
// FCI/FCE). Mouse look: not through the right stick. The hook on the game's look input (Player_LookInput,
// config/predator/default.toml) runs the original unchanged and then adds the mouse's degrees to the frame's
// yaw/pitch command, scaled by the game's own zoom factor (field of view / 90 degrees); only for the first
// local player. The pointer is locked while the look input runs and the window has the focus, free otherwise
// (menus, pause, other windows).
//
// Connection (deviation fix): the SDK gives user 0 a neutral "idle pad" when nothing is connected, so the title
// never saw "not connected". Now XamInputGetState/XamInputGetCapabilities for user 0 answer
// ERROR_DEVICE_NOT_CONNECTED when no real pad is there and keyboard/mouse are off, or when a real pad was
// unplugged and no key, mouse button or mouse motion was used since (as the console: the title shows its own
// reaction). With keyboard/mouse on and no pad ever seen, keyboard/mouse is a connected pad 0.
//
// Switches (read once at title launch):
//   FCP_KEYBOARD=0            keyboard off (bindings ignored)
//   FCP_MOUSE=0               mouse off (no look, no buttons, no pointer lock)
//   FCP_IDLE_PAD=1            SDK behaviour back: user 0 always connected (idle pad)
//   FCP_INPUT_CONFIG=<path>   another bindings file (default <exe folder>\predator_input.toml)
//   FCP_PAD_BACKGROUND=1      SDL joystick events also without window focus (SDL hint)
//   FCP_TRACE_LOOK=<n>        log the first n look inputs of player 1 (stick, mouse, command), then every 300th
//   FCP_PAD_BACKEND=sdl|xinput|off, FCP_PAD_DEAD_ZONE=<0..0.9>, FCP_PAD_RUMBLE=<percent>,
//   FCP_MOUSE_ACCEL=<gain per count/ms>, FCP_MOUSE_ACCEL_CAP=<factor>, FCP_MOUSE_SMOOTHING=<ms>,
//   FCP_MOUSE_ZOOM_SCALE=fov|tan|off, FCP_MOUSE_ZOOM_SENSITIVITY=<factor>: win over predator_input.toml
//   FCP_PAUSE_ON_FOCUS_LOSS=1 (predator.toml [game] pause_on_focus_loss): XN_SYS_UI, mute, motors off while the
//                             window has no focus
//   FCP_TEST_FOCUS=<t>:<s>,... test: a focus loss at t (script clock) for s wall seconds
//   FCP_TRACE_RUMBLE=<n>      log the first n rumble changes (default 20)
//   FCP_KEY_SCRIPT=<entry>,...  test input as from the devices (works without focus and pointer lock):
//       <t>[-<t_end>]:<key>[+<key>...]       keys held from t to t_end (default FCP_INPUT_HOLD_MS, 150 ms)
//       <t>[-<t_end>]:mx=<counts> / my=<counts>   mouse counts, spread evenly over [t, t_end] (else at once)
//     t on the clock of FCP_INPUT_SCRIPT_CLOCK (wall: seconds since title launch; level: game time since the
//     level start). Key names as in the bindings file. FCP_KEY_SCRIPT_GENS like FCP_INPUT_SCRIPT_GENS.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rex/ppc/context.h>

namespace rex {
class SettingsRegistry;
}
namespace rex::hid {
class IInputSystem;
}
namespace rex::ui {
class IUISystem;
}

namespace fcp::pc_input {

/** OnConfigure (before the modules start): hid.drivers from [pad] backend / FCP_PAD_BACKEND. */
void ApplyStartupSettings(rex::SettingsRegistry& settings);

/** Reads the switches and the bindings file (title launch). */
void Configure(rex::hid::IInputSystem* input);

/** Window focus events for the focus pause (title launch / shutdown begin). */
void AttachUI(rex::ui::IUISystem& ui);
void DetachUI();

/** Before XamInputSetState(user, unk, vibration) reaches the SDK: strength and focus pause. */
void BeforeSetState(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t vibration);

/** After XamInputSetState: the title's own values back into its structure. */
void AfterSetState(uint8_t* base);

/** True when the XamInputGetState/GetCapabilities wrappers must call OnGetState/OnGetCapabilities. */
bool Active();

/** After XamInputGetState(user, flags, state) returned, before the input script (ctx.r3 = result). */
void OnGetState(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t state);

/** After XamInputGetCapabilities(user, flags, caps) returned, before the input script. */
void OnGetCapabilities(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t caps);

/** Writes the counters into the log (shutdown). */
void WriteSummary();

// ---- settings menu (fcp/settings_menu.h) ----

/** The input settings in force, in the units of predator_input.toml. */
struct Live {
  bool keyboard = true, mouse = true;
  double sensitivity_x = 1, sensitivity_y = 1;
  bool invert_y = false;
  std::string zoom_scale;
  double zoom_sensitivity = 1, acceleration = 0, acceleration_cap = 2, smoothing_ms = 0, dead_zone = 0;
  int rumble = 100;
  std::string backend;
};
Live GetLive();

/** predator_input.toml (the file in use). */
std::filesystem::path ConfigFile();

/** "FCP_X=value" when an environment switch wins over this key of the file, else empty. */
std::string EnvironmentOverride(std::string_view section, std::string_view key);

/**
 * Writes `key = literal` into [section] of predator_input.toml (comments stay) and reads the whole file again, so
 * every setting holds at once ([pad] backend: from the next start). Returns an error text or "".
 */
std::string Store(std::string_view section, std::string_view key, const std::string& literal);

/** The keyboard/mouse bindings ([bindings]): count, target name, keys in force, default keys. */
int BindingCount();
std::string BindingName(int index);
std::string BindingKeys(int index);
std::string DefaultBindingKeys(int index);
/** Name of a key code (Windows virtual key, mouse buttons as VK_LBUTTON...) as the file writes it; "" if none. */
std::string KeyName(int code);
std::string WheelName(bool down);

// ---- button prompts (fcp/button_prompts.h) ----

/** The key codes of a binding in force (Windows virtual keys, VK_LBUTTON.., WheelCode); empty when unbound. */
std::vector<int> BindingCodes(int index);
/** The code the bindings use for a wheel notch up / down. */
int WheelCode(bool down);
/** The controller button behind pad target `name` (a b x y lb rb lt rt ls rs start back up down left right) as
 *  its Xbox name ("a", "lt", ...), the first of [pad] pad_bind_<name>; "" when none. */
std::string PadSourceName(std::string_view name);
/** Changes whenever predator_input.toml was read again (bindings may have changed). */
uint32_t SettingsGeneration();

/** The settings menu holds the game: XN_SYS_UI on (the title's own pause, as the console's guide). */
void SetMenuPause(bool open);

/** Reads predator_input.toml and the switches again (after a predator.toml key the input reads changed). */
void Reload();

/** The SDK's input system (null before the title launch). */
rex::hid::IInputSystem* Input();

}  // namespace fcp::pc_input
