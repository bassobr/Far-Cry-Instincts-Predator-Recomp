// Device detection and device defaults (Steam Deck under Proton, ROG Xbox Ally X under Windows).
//
// The Steam Deck is recognised by its firmware: board vendor Valve, product Jupiter (LCD) or Galileo (OLED), read
// from the registry key Windows (and Wine/Proton, filled from DMI) keeps the SMBIOS values in, else from the Linux DMI
// files through drive Z:, else SteamDeck=1 (Steam sets it for games it starts on the Deck).
// The ROG Ally family is recognised by ASUS's model number in the SMBIOS product name from the same places: RC73XA =
// ROG Xbox Ally X (measured, has defaults); RC72LA (ROG Ally X), RC73YA (ROG Xbox Ally), RC71L (ROG Ally) are named only.
// FCP_DEVICE=deck|ally|pc forces the choice, FCP_DEVICE=pc is the way back to the PC defaults on a Deck or an Ally.
//
// A device default only replaces the built-in default of a predator.toml key: a value in the file or a switch in the
// environment always wins. A new predator.toml is written with the device defaults (marked in a comment).
// Idea and detection order after FCI's src/app/settings.cpp (MIT, same author).
#pragma once

#include <string>
#include <string_view>

namespace fcp::device {

/** "Steam Deck OLED", "Steam Deck LCD", "Steam Deck", "ROG Xbox Ally X", "ROG Ally X", "ROG Xbox Ally", "ROG Ally" or ""`r`n * (a PC). Detected once; logs the result. */
const std::string& Name();

/** The device's default for a predator.toml switch (e.g. "FCP_FPS"), as file text; empty when it has none. */
std::string Default(std::string_view env_name);

}  // namespace fcp::device
