// The settings menu (F10, on a controller Back + Start) and the frame rate display, drawn with the SDK's Dear ImGui
// over the game picture (renderer overlay). PC comfort only. Idea and layout after
// FCI's F10 menu (Far Cry Instincts Recomp, src/app/menu.cpp, MIT, same author): apply at once where
// the module can, else mark "restart required", store into predator.toml / predator_input.toml.
//
// While the menu is open the game gets no input (pc_input: idle pads, no keyboard/mouse, pointer free) and is held
// by the console guide's XN_SYS_UI (its own pause menu in a level); closing lets it go on.
//
// Switches:
//   FCP_MENU=0                 no menu (F10 and Back + Start reach the game as before)
//   FCP_MENU_PAUSE=0           the open menu does not hold the game (no XN_SYS_UI)
//   FCP_FPS_COUNTER=1          frame rate display ([game] fps_counter)
//   FCP_TEST_MENU=<t>:<action>,...  test, wall seconds since the title launch, actions:
//       open | close | toggle | tab=<display|graphics|input|game|about> | dump=<file.png> (next presented picture,
//       relative = log folder) | set=<section>.<key>=<value> (predator.toml, as the menu does it) |
//       input=<section>.<key>=<literal> (predator_input.toml) | post=<key> (key down/up as window messages, needs
//       the focus: f10, escape, ...) | pad=<buttons> (combo, a, b, up, down, ...: held for 0.2 s as from a pad) |
//       exit | apply (as a click on Exit Game / Apply) | yes | no (answer the question) | quit | restart (without
//       asking); <t>@<g>:<action> only in relaunch generation g.
//     FCP_TEST_MENU_GENS like FCP_INPUT_SCRIPT_GENS.
//   FCP_MENU_RESTART=host      Apply always starts the game afresh (default: the game's ReturnToMenu once a profile
//                              is selected, a fresh start on the title screen)
//
// Exit Game (bottom left, asks first, as in FCI) ends the game as the window's close button. Apply (restarts the
// game) appears while a "restart required" setting differs from its value at start: in the menus at once, in a
// level after a question (progress since the last checkpoint is lost).
#pragma once

#include <cstdint>

namespace rex::ui {
class IUISystem;
}

namespace fcp::menu {

/** Title launch, after pc_input::Configure: adds the dialog, the hotkeys and the test script. */
void Attach(rex::ui::IUISystem& ui);
void Detach();

/** True while the menu is open (any thread): the game's input is held. */
bool BlocksInput();

/** Pad buttons of a pad state the game must not see (Back and Start of the menu chord). Any thread. */
uint16_t PadButtonsToHide(uint16_t buttons);

}  // namespace fcp::menu
