// Device detection and device defaults (fcp/device_preset.h). Own code of this project; the detection order
// (registry, DMI files through Z:, SteamDeck=1, a forcing switch) and the ROG Ally X model numbers follow FCI's
// src/app/settings.cpp (MIT, same author).
#include "fcp/device_preset.h"

#include <windows.h>

#include <cctype>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "fcp/env.h"
#include "fcp/log.h"

namespace fcp::device {
namespace {

std::string Trim(std::string s) {
  const size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The firmware's system information as Windows keeps it; Wine fills the same key from DMI.
std::string BiosValue(const char* name) {
  char value[128] = {};
  DWORD size = sizeof value;
  if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", name, RRF_RT_REG_SZ, nullptr, value,
                   &size) != ERROR_SUCCESS) {
    return {};
  }
  return Trim(value);
}

// First line of a host file (under Wine/Proton the Linux root is drive Z:), "" when unreadable.
std::string ReadLine(const char* path) {
  std::ifstream in(path);
  std::string line;
  if (in) std::getline(in, line);
  return Trim(line);
}

struct Preset {
  std::string name;
  std::vector<std::pair<std::string, std::string>> defaults;  // switch -> file text
};

// Steam Deck defaults, measured on a Steam Deck OLED under Proton 10 (gamescope, 1280x800, both backends).
// FCP_FPS: 30 holds in every measured level at about 9 W APU power; a 60 cap holds in lighter levels but averages
// about 56 fps with 1% lows below 20 fps in the heaviest one, at the 15 W limit, and 90 is out of reach (60-75 fps
// uncapped). Listed although it equals the built-in value, so predator.toml names it as the device's choice.
// The display mode is not set here: only the built-in windowed mode was measured (gamescope shows it at 1280x800).
// FCP_FPS_PACING display (docs/phase1/deck-ruckeln.md): the display runs at 89.6 Hz; with the game's own pacing
// (60 Hz vblank clock of the null driver, or a host limiter) 30 fps leaves 10-31 % of the frames 2 or 4 refreshes
// instead of 3 and 45 fps 9-21 % 1 or 3 instead of 2, as the two clocks drift. Paced on the display 30 and 45 keep
// 97-100 % of the frames at 3 or 2 refreshes in 21_dunea, 00_training, 06_cliffs and 01_beach, 45 at 8-10 W.
// Resolution: a 1280x800 window (the display's size) with the native render resolution, so the picture fills the
// 16:10 display (Vert+) instead of 1280x720 with bars; [display] width/height = 0 in an older file takes these too.
// FCP_GAME_CODE_LTO 0 (docs/phase1/deck-erststart-rumble.md): on the Deck ThinLTO brought nothing measurable in the
// game (21_dunea 74.1 against 73.8 frames/s at a 90 cap) and the first-start build without it took 29-56 s less in
// three of three pairs measured in the same time window; FCP_GAME_CODE_LTO=1 builds with ThinLTO.
const std::vector<std::pair<std::string, std::string>> kDeckDefaults = {
    {"FCP_FPS", "30"},
    {"FCP_FPS_PACING", "display"},
    {"FCP_WINDOW_WIDTH", "1280"},
    {"FCP_WINDOW_HEIGHT", "800"},
    {"FCP_RENDER_RESOLUTION", "native"},
    {"FCP_GAME_CODE_LTO", "0"},
};

// ROG Xbox Ally X defaults, measured on a ROG Xbox Ally X (RC73XA, Ryzen AI Z2 Extreme, Radeon 890M, Windows 11,
// 1920x1080 at 120 Hz, mains power), borderless at the native resolution, both backends:
// FCP_FPS 120 holds in 21_dunea, 06_cliffs and 00_training (119.3-120.0 fps from the second run of a level on,
// p99 9-15 ms, main thread 5-7 ms per frame; one D3D12 run at 105.5 did not repeat in four more); uncapped Vulkan reaches 147-169. The display runs at 120 Hz, so 120 is the
// smooth cap. Borderless is the measured mode (the built-in default is a window). scale stays 1: scale=auto (1080
// lines) costs about 2 fps in 21_dunea (117.7) and is left to the player.
const std::vector<std::pair<std::string, std::string>> kAllyXDefaults = {
    {"FCP_FPS", "120"},
    {"FCP_DISPLAY_MODE", "borderless"},
};

// ASUS's model number in the SMBIOS product name ("ROG Xbox Ally X RC73XA_RC73XA" on the Windows firmware). Only the
// measured model gets defaults: the ROG Xbox Ally X (RC73XA, Ryzen AI Z2 Extreme). The ROG Ally X (RC72LA, Z1 Extreme,
// Radeon 780M), the ROG Xbox Ally (RC73YA) and the ROG Ally (RC71L) have slower GPUs and were not measured: they are
// named in the log and keep the PC defaults. FCP_DEVICE=ally forces the RC73XA defaults.
struct Ally {
  const char* model;
  const char* name;
  bool defaults;
};
constexpr Ally kAllies[] = {
    {"rc73xa", "ROG Xbox Ally X", true},
    {"rc72la", "ROG Ally X", false},
    {"rc73ya", "ROG Xbox Ally", false},
    {"rc71l", "ROG Ally", false},
};

const Preset& Get() {
  static const Preset preset = [] {
    Preset p;
    const std::string forced = Lower(fcp::env::Get("FCP_DEVICE"));
    std::string vendor = BiosValue("BaseBoardManufacturer"), product = BiosValue("SystemProductName");
    std::string how = "registry";
    if (vendor.empty() && product.empty()) {
      vendor = ReadLine("Z:\\sys\\class\\dmi\\id\\board_vendor");
      product = ReadLine("Z:\\sys\\class\\dmi\\id\\product_name");
      how = "DMI";
    }
    const bool steam_deck_env = fcp::env::Get("SteamDeck") == "1";
    const std::string model = Lower(product);
    bool deck = (Lower(vendor) == "valve" && (model == "jupiter" || model == "galileo")) || steam_deck_env;
    // the vendor string differs between firmwares ("ASUSTeK COMPUTER INC." on the board): the model number decides
    const Ally* ally = nullptr;
    if (!deck) {
      for (const Ally& a : kAllies) {
        if (model.find(a.model) != std::string::npos) {
          ally = &a;
          break;
        }
      }
    }
    if (forced == "deck") deck = true, ally = nullptr;
    if (forced == "ally" && (!ally || !ally->defaults)) ally = &kAllies[0], deck = false;
    if (forced == "pc") deck = false, ally = nullptr;
    if (!forced.empty() && forced != "deck" && forced != "ally" && forced != "pc") {
      FCPLOG("device: FCP_DEVICE={} unknown (deck, ally, pc), detection decides", forced);
    }
    if (deck) {
      p.name = model == "galileo" ? "Steam Deck OLED" : model == "jupiter" ? "Steam Deck LCD" : "Steam Deck";
      p.defaults = kDeckDefaults;
      // Steam Deck OLED (90 Hz): 45 fps paced on the display, measured in docs/phase1/deck-v0.1.2.md (96-100 % of
      // the frames exactly 2 refreshes at 7.8-8.6 W, 1280x800) after the 45 fps corrections of
      // docs/phase1/bildraten-45.md. The LCD model (60 Hz) and an unknown model keep 30: 45 cannot be paced on 60 Hz.
      // FCP_DECK_FPS45=0 keeps 30 on the OLED too.
      const bool oled = model == "galileo" || (forced == "deck" && model != "jupiter");
      if (oled && fcp::env::Get("FCP_DECK_FPS45") != "0") {
        for (auto& [k, v] : p.defaults) {
          if (k == "FCP_FPS") v = "45";
        }
      }
    } else if (ally) {
      p.name = ally->name;
      if (ally->defaults) p.defaults = kAllyXDefaults;
    }
    FCPLOG("device: {} ({}: vendor '{}', product '{}', SteamDeck={}{}{}){}",
           p.name.empty() ? std::string("PC (not a Steam Deck or ROG Ally X)") : p.name, how, vendor, product,
           steam_deck_env ? "1" : "-", forced.empty() ? "" : ", FCP_DEVICE=", forced,
           ally && !ally->defaults ? ", not measured: PC defaults" : "");
    for (const auto& [k, v] : p.defaults) FCPLOG("device: {} default {} = {}", p.name, k, v);
    return p;
  }();
  return preset;
}
}  // namespace

const std::string& Name() { return Get().name; }

std::string Default(std::string_view env_name) {
  for (const auto& [k, v] : Get().defaults) {
    if (k == env_name) return v;
  }
  return {};
}

}  // namespace fcp::device
