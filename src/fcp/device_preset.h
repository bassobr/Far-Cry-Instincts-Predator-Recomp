// Device detection and device defaults (Steam Deck under Proton).
//
// The Steam Deck is recognised by its firmware: board vendor Valve, product Jupiter (LCD) or Galileo (OLED), read
// from the registry key Windows (and Wine/Proton, filled from DMI) keeps the SMBIOS values in, else from the Linux DMI
// files through drive Z:, else SteamDeck=1 (Steam sets it for games it starts on the Deck).
// FCP_DEVICE=deck|pc forces the choice, FCP_DEVICE=pc is the way back to the PC defaults on a Deck.
//
// A device default only replaces the built-in default of a predator.toml key: a value in the file or a switch in the
// environment always wins. A new predator.toml is written with the device defaults (marked in a comment).
// Idea and detection order after FCI's src/app/settings.cpp (MIT, same author).
#pragma once

#include <string>
#include <string_view>

namespace fcp::device {

/** "Steam Deck OLED", "Steam Deck LCD", "Steam Deck" or "" (a PC). Detected once; logs the result. */
const std::string& Name();

/** The device's default for a predator.toml switch (e.g. "FCP_FPS"), as file text; empty when it has none. */
std::string Default(std::string_view env_name);

}  // namespace fcp::device
