// Virtual SDL3 gamepads for self tests without a connected pad (as FCI_VIRTUAL_PADS in Far Cry Instincts Recomp).
// The pads are real SDL joysticks (SDL's virtual joystick driver); the SDK input driver sees them like a
// plugged-in controller, so the whole path SDL -> rexhid -> XamInputGetState is exercised (unlike
// FCP_INPUT_SCRIPT, which writes into the title's buffer after the SDK).
//
//   FCP_VIRTUAL_PADS=<n>                n pads (1..4), attached after title launch
//   FCP_VIRTUAL_PAD_SCRIPT=<s>:<step>[+<step>],...   seconds since title launch; steps:
//       a b x y back guide start ls rs lb rb up down left right   button, held FCP_VIRTUAL_PAD_HOLD_MS (150)
//       lx=<v> ly=<v> rx=<v> ry=<v>    stick -1..1 (y up positive, as XInput), stays until changed
//       lt=<v> rt=<v>                   trigger 0..1, stays until changed
//       unplug / plug                   detach / attach the pad again (hot plug)
//     prefix "p<k>." addresses pad k (0 based), default pad 0, e.g. "40:p1.start"
//   FCP_VIRTUAL_PAD_ID=<vid>:<pid>      USB ids in hex (default 0:0, SDL type "standard")
// Rumble that reaches SDL for a virtual pad is counted and logged (first 50 changes).
// While virtual pads are active, SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS is set, so the test also runs
// when the window has no focus.
#pragma once

namespace fcp::virtual_pads {

void Start();
void Stop();

}  // namespace fcp::virtual_pads
