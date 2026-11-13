// FCP_KEYBOARD_TEXT: the text the system keyboard (XamShowKeyboardUI) offers, for tests without a window.
// With FCP_NO_DIALOGS the SDK answers at once with the default text
// the title passes (the save name "Create New Game" proposes); a second save with the same name made the game ask
// again and again (seen in the level run).
//   FCP_KEYBOARD_TEXT=<text>[|<text>...]   call n takes entry n (the last one repeats); {n} = the call number.
// The text becomes the default text: headless it is the answer, with a window the prompt starts with it.
#pragma once

#include <cstdint>

struct PPCContext;

namespace fcp::keyboard_text {

bool Enabled();

/** Before the SDK call: XamShowKeyboardUI(user r3, flags r4, default r5, title r6, description r7, buffer r8,
    length r9, overlapped r10). Writes the text into the title's buffer and passes it as the default. */
void OnShowKeyboard(PPCContext& ctx, uint8_t* base);

}  // namespace fcp::keyboard_text
