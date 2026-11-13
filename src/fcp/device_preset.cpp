// Device detection and device defaults (fcp/device_preset.h). Own code of this project; the detection order
// (registry, DMI files through Z:, SteamDeck=1, a forcing switch) follows FCI's src/app/settings.cpp (MIT, same
// author).
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
const std::vector<std::pair<std::string, std::string>> kDeckDefaults = {
    {"FCP_FPS", "30"},
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
    if (forced == "deck") deck = true;
    if (forced == "pc") deck = false;
    if (!forced.empty() && forced != "deck" && forced != "pc") {
      FCPLOG("device: FCP_DEVICE={} unknown (deck, pc), detection decides", forced);
    }
    if (deck) {
      p.name = model == "galileo" ? "Steam Deck OLED" : model == "jupiter" ? "Steam Deck LCD" : "Steam Deck";
      p.defaults = kDeckDefaults;
    }
    FCPLOG("device: {} ({}: vendor '{}', product '{}', SteamDeck={}{}{})",
           deck ? p.name : std::string("PC (not a Steam Deck)"), how, vendor, product, steam_deck_env ? "1" : "-",
           forced.empty() ? "" : ", FCP_DEVICE=", forced);
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
