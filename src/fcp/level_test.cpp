#include "fcp/level_test.h"

#include <bit>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <rex/ppc/hook.h>
#include <rex/ppc/recompiled_code.h>

#include "fcp/devstart.h"
#include "fcp/env.h"
#include "fcp/gametime.h"
#include "fcp/guest.h"
#include "fcp/guest_call.h"
#include "fcp/log.h"
#include "fcp/relaunch.h"
#include "fcp_addresses.h"

REX_EXTERN(__imp__EntitySystem_GetEntity);

namespace fcp::level_test {
namespace {

// Layout as in fcp/state_trace.cpp (read from the image): game object local player slot +0x40 (else +0x30),
// slot +0x34 = entity id, EntitySystem_GetEntity(system, id). Entity virtuals: +0x10 GetPos(1) returns a
// pointer to three floats (binding 0x82320380), +0x14 SetPos(&vec3, 0) (binding Entity:SetPos 0x82320740,
// call at 0x82320810 with r5 = 0).
constexpr uint32_t kSlotPreferred = 0x40;
constexpr uint32_t kSlotFirst = 0x30;
constexpr uint32_t kPlayerEntityId = 0x34;
constexpr uint32_t kEntityGetPos = 0x10;
constexpr uint32_t kEntitySetPos = 0x14;

struct Teleport {
  std::string level;  // empty: any level (generation list applies)
  double at = 0;
  float pos[3] = {};
  bool done = false;
  int tries = 0;
  std::string text;
};

std::vector<Teleport> g_teleports;

std::string Lower(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

std::vector<std::string> Split(const std::string& text, char sep) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find(sep, pos);
    if (end == std::string::npos) {
      end = text.size();
    }
    out.push_back(text.substr(pos, end - pos));
    pos = end + 1;
  }
  return out;
}

float F32(uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(fcp::guest::U32(base, address));
}

PPCFunc* Virtual(uint8_t* base, uint32_t object, uint32_t slot) {
  const uint32_t vtable = fcp::guest::U32(base, object);
  if (!fcp::guest::Readable(base, vtable + slot, 4)) {
    return nullptr;
  }
  return rex::ppc::ResolveFunction(fcp::guest::U32(base, vtable + slot));
}

uint32_t LocalPlayerEntity(PPCContext& ctx, uint8_t* base) {
  const uint32_t game = fcp::guest::U32(base, fcp::addr::game_ptr);
  if (!fcp::guest::Readable(base, game, 0x70)) {
    return 0;
  }
  uint32_t slot = fcp::guest::U32(base, game + kSlotPreferred);
  if (!slot) {
    slot = fcp::guest::U32(base, game + kSlotFirst);
  }
  if (!fcp::guest::Readable(base, slot, kPlayerEntityId + 4)) {
    return 0;
  }
  const uint32_t id = fcp::guest::U32(base, slot + kPlayerEntityId);
  const uint32_t system = fcp::guest::U32(base, fcp::addr::entity_system_ptr);
  if (!fcp::guest::Readable(base, system, 4)) {
    return 0;
  }
  const uint32_t entity = fcp::guest::Call(ctx, base, __imp__EntitySystem_GetEntity, {.r3 = system, .r4 = id});
  return fcp::guest::Readable(base, entity, 0x140) ? entity : 0;
}

std::string Position(PPCContext& ctx, uint8_t* base, uint32_t entity, float* out = nullptr) {
  PPCFunc* get = Virtual(base, entity, kEntityGetPos);
  if (!get) {
    return "?";
  }
  const uint32_t p = fcp::guest::Call(ctx, base, *get, {.r3 = entity, .r4 = 1});
  if (!fcp::guest::Readable(base, p, 12)) {
    return "?";
  }
  if (out) {
    for (int i = 0; i < 3; ++i) out[i] = F32(base, p + 4 * i);
  }
  return std::format("({:.3f} | {:.3f} | {:.3f})", F32(base, p), F32(base, p + 4), F32(base, p + 8));
}

// A SetPos that does not take (player bound to a vehicle or a scripted sequence, e.g. the helicopter in
// 13_black_forest or the boat MG in 21_dunea) is repeated every kRetrySeconds, at most kMaxTries times.
constexpr double kRetrySeconds = 5.0;
constexpr int kMaxTries = 36;

}  // namespace

void Configure() {
  const std::string value = fcp::env::Get("FCP_TEST_TELEPORT");
  if (value.empty()) {
    return;
  }
  const bool unnamed_active = fcp::devstart::ActiveInGeneration("FCP_TEST_TELEPORT_GENS");
  for (const std::string& item : Split(value, '|')) {
    if (item.empty()) {
      continue;
    }
    Teleport t;
    t.text = item;
    std::string rest = item;
    if (const size_t at = rest.find('@'); at != std::string::npos) {
      t.level = Lower(rest.substr(0, at));
      rest = rest.substr(at + 1);
    }
    const size_t colon = rest.find(':');
    const std::vector<std::string> xyz = colon == std::string::npos ? std::vector<std::string>{}
                                                                     : Split(rest.substr(colon + 1), ',');
    if (xyz.size() != 3) {
      FCPLOG("FCP_TEST_TELEPORT: entry '{}' ignored ([<level>@]<s>:<x>,<y>,<z>)", item);
      continue;
    }
    t.at = std::strtod(rest.c_str(), nullptr);
    for (int i = 0; i < 3; ++i) {
      t.pos[i] = std::strtof(xyz[i].c_str(), nullptr);
    }
    if (t.level.empty() && !unnamed_active) {
      continue;
    }
    g_teleports.push_back(t);
  }
  FCPLOG("FCP_TEST_TELEPORT: {} entr{} (test switch: changes the player position; generation {})",
         g_teleports.size(), g_teleports.size() == 1 ? "y" : "ies", fcp::relaunch::Generation());
}

void OnMainLoop(PPCContext& ctx, uint8_t* base) {
  if (g_teleports.empty() || !fcp::gametime::InLevel()) {
    return;
  }
  const double level_s = fcp::gametime::LevelSeconds();
  std::string level;
  for (Teleport& t : g_teleports) {
    if (t.done || level_s < t.at) {
      continue;
    }
    if (!t.level.empty()) {
      if (level.empty()) {
        level = Lower(fcp::gametime::LevelName());
      }
      if (t.level != level) {
        continue;
      }
    }
    const uint32_t entity = LocalPlayerEntity(ctx, base);
    if (!entity) {
      continue;  // no local player yet: try again next frame
    }
    PPCFunc* set = Virtual(base, entity, kEntitySetPos);
    if (!set) {
      FCPLOG("FCP_TEST_TELEPORT: entity {:08X} has no SetPos slot, '{}' dropped", entity, t.text);
      t.done = true;
      continue;
    }
    const std::string before = Position(ctx, base, entity);
    uint32_t be[3];
    for (int i = 0; i < 3; ++i) {
      be[i] = __builtin_bswap32(std::bit_cast<uint32_t>(t.pos[i]));
    }
    // The vector goes into the scratch frame of the call (string_r4 copies raw bytes, r4 points at them).
    fcp::guest::Call(ctx, base, *set,
                     {.r3 = entity, .r5 = 0,
                      .string_r4 = std::string_view(reinterpret_cast<const char*>(be), sizeof(be))});
    float now[3] = {};
    const std::string after = Position(ctx, base, entity, now);
    const float dx = now[0] - t.pos[0], dy = now[1] - t.pos[1], dz = now[2] - t.pos[2];
    const bool taken = dx * dx + dy * dy + dz * dz < 4.0f;
    ++t.tries;
    t.done = taken || t.tries >= kMaxTries;
    if (!taken) {
      t.at = level_s + kRetrySeconds;
    }
    if (taken || t.tries == 1 || t.done || t.tries % 6 == 0) {
      FCPLOG("FCP_TEST_TELEPORT: level '{}' at {:.2f} s (wall {:.2f} s): player entity {:08X} {} -> SetPos "
             "({:.3f} | {:.3f} | {:.3f}) -> {} {} (try {})",
             fcp::gametime::LevelName(), level_s, fcp::gametime::WallSeconds(), entity, before, t.pos[0], t.pos[1],
             t.pos[2], after,
             taken ? "taken" : t.done ? "NOT taken, given up" : "not taken (player bound?), retry in 5 s", t.tries);
    }
  }
}

}  // namespace fcp::level_test
