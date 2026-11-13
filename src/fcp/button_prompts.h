// Button prompts that follow the input device. PC comfort; the default draws the
// console's glyphs until another device is used.
//
// How the game shows buttons: its strings carry tokens ({A} {B} {X} {Y} {WH} {BL} {START} {BACK} {TRIGL} {TRIGR}
// {ASL} {ASR} {DPU} ... in common.dat), which the game replaces with characters of its Magma font (token table
// 0x828BB260, characters 0x82053C70, replaced in 0x821F7E10: {A} -> U+03A9 ...). Those characters are glyphs on
// two pages of the font "Geometr706 BdCn BT Bold 20(_hud)": page 0 (512x256 DXT3: A/B/X/Y, LB/RB, sticks, D-pads,
// weapon and vehicle icons) and page 1 (256x256 DXT3: START, BACK, the triggers, the alphabet). The renderer asks
// this module (renderer/texture_replace.h) whether a converted texture is one of these pages and binds, for the
// device in use, our own copy in which each button glyph is drawn anew: the bound key or mouse button with
// keyboard and mouse, PlayStation or Switch buttons for those pads. Place and size of every glyph stay the game's.
//
// The drawing is taken over from FCI (Far Cry Instincts Recomp, src/gpu/prompt_icons.cpp and src/hle/prompts.cpp,
// own MIT project code of the same author): signed distance shapes with one pixel of antialiasing, key names from
// GDI (Arial bold). The glyph boxes, colours and the page detection are Predator's own.
//
// Switches:
//   predator.toml [input] button_prompts = auto | xbox | keyboard | playstation | switch   (FCP_BUTTON_PROMPTS)
//     auto (default) = the device used last (before any use: a connected controller's, else keyboard and mouse).
//   FCP_BUTTON_PROMPTS=xbox   the game's own glyphs always (the console's look; the code that replaces stays idle)
//   FCP_PROMPTS=0             the module is off (no texture is classified or replaced: the state before)
//   FCP_PROMPTS_PS=3|4|5      PlayStation generation when the pad does not tell (OPTIONS/SHARE, PS5 CREATE)
//   FCP_PROMPTS_DUMP=1        every drawn page as prompts_<style>_<kind>.png into the log folder
//   FCP_PROMPTS_LOG=1         every candidate texture (BC2, one level) with its key into the log
#pragma once

#include <cstdint>

namespace fcp::nr {
class TextureReplaceSource;
}

namespace fcp::prompts {

enum class Style : uint8_t { kXbox, kKeyboard, kPlayStation, kSwitch };
enum class PadFamily : uint8_t { kXbox, kPlayStation3, kPlayStation4, kPlayStation5, kSwitch };

/** Reads button_prompts (FCP_BUTTON_PROMPTS) and the switches. Before the renderer starts. */
void Configure();

/** The renderer's texture replacement source (nullptr when FCP_PROMPTS=0). */
nr::TextureReplaceSource* Source();

/** The style shown now; its name ("xbox", "keyboard", "playstation", "switch"). */
Style Current();
const char* Name(Style s);

// ---- what the player used last (auto); cheap, any thread, no locks of the caller taken ----
void NoteKeyboard();                          // a key or mouse button is down (real or FCP_KEY_SCRIPT)
void NoteMouseMotion(double dx, double dy);   // mouse counts (only steady motion switches)
void NotePad(PadFamily family);               // player 1's controller used clearly
void NotePadPresent(PadFamily family);        // player 1 has this controller (decides before anything is used)

/** button_prompts changed (settings menu): read it again. */
void Reload();

/** Shutdown: counters into the log. */
void WriteSummary();

}  // namespace fcp::prompts
