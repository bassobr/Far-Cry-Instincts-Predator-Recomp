#include "fcp/state_trace.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>

#include <rex/ppc/hook.h>
#include <rex/ppc/recompiled_code.h>

#include "fcp/engine_hooks.h"
#include "fcp/env.h"
#include "fcp/gametime.h"
#include "fcp/guest.h"
#include "fcp/guest_call.h"
#include "fcp/log.h"
#include "fcp/relaunch.h"
#include "fcp_addresses.h"

REX_EXTERN(__imp__EntitySystem_GetEntity);

namespace fcp::state {
namespace {

// Layout, read from the image:
//  - game object ([fcp::addr::game_ptr]): local player slots at +0x30 (+4 per player, count +0x6C),
//    Lua Game:GetLocalPlayer prefers +0x40, else +0x30 (0x820F3BA4).
//  - local player slot: +0x34 entity id (0x820F3BC0).
//  - player object (entity +0x254, below): +0xE8 "health", +0xF0 "armor" (Lua properties of the Player
//    class, 0x820FBEFC ff., type 3); +0x39F IsInWater, +0x39C IsUnderWater (Player bindings 0x82108E00 /
//    0x82108EB8).
//  - entity = EntitySystem_GetEntity([fcp::addr::entity_system_ptr], id) (0x820D19B8, as GetLocalPlayer);
//    virtual +0x10 GetPos(1) and +0x18 GetAngles(0) return a pointer to three floats (Entity bindings
//    GetPos 0x82320380, GetAngles 0x82322CE8).
constexpr uint32_t kSlotPreferred = 0x40;
constexpr uint32_t kSlotFirst = 0x30;
constexpr uint32_t kPlayerEntityId = 0x34;
constexpr uint32_t kPlayerHealth = 0xE8;
constexpr uint32_t kPlayerArmor = 0xF0;
constexpr uint32_t kPlayerUnderWater = 0x39C;
constexpr uint32_t kPlayerInWater = 0x39F;
// More Player properties (same table, 0x820FBF70 ff.): "breath" +0xF8, "firing" +0x13C, "reloading" +0x144,
// "jumping" +0x162. Weapon ammo: Player.GetCurrentAmmoInClip (0x82106248) -> 0x82245588 -> 0x82280710 reads
// [player +0x2CC] + (3 + slot) * 4 for ammo type 1 and + (5 + slot) * 4 for type 2; the trace writes the four
// words +0xC..+0x18 of that block (clip counters, used to count shots per second).
constexpr uint32_t kPlayerBreath = 0xF8;
constexpr uint32_t kPlayerFiring = 0x13C;
constexpr uint32_t kPlayerReloading = 0x144;
constexpr uint32_t kPlayerJumping = 0x162;
constexpr uint32_t kPlayerAmmoBlock = 0x2CC;
constexpr uint32_t kEntityGetPos = 0x10;
constexpr uint32_t kEntityGetAngles = 0x18;
// Entity +0x254 = the player object (container) the Player bindings work on: its vtable 0x82062488 holds the
// p_waterbob method 0x8226F5E8 (slot 0x8206256C, engine-abgleich.md); measured with FCP_TRACE_STATE_SCAN: it
// points back to the entity and holds 100.0 at +0xE8 (health) after a level start.
constexpr uint32_t kEntityContainer = 0x254;

int g_every = 0;
std::FILE* g_csv = nullptr;
// Shot parameters of Weapon_Fire (0x82283B00 builds them on its stack): +0x1C = shooter, the player object
// (r6 of 0x82283B00, stored at 0x82283E68; Weapon_Fire reads +0x1C at 0x82284018).
constexpr uint32_t kShotShooter = 0x1C;
bool g_discovered = false;
uint32_t g_last_entity = 0;
bool g_have_prev = false;
float g_prev[3] = {};
double g_prev_game = 0.0;
uint64_t g_rows = 0;
uint32_t g_player = 0;          // player object (entity +0x254) of the last traced frame
uint64_t g_player_shots = 0;    // Weapon_Fire calls with that shooter

float F32(uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(fcp::guest::U32(base, address));
}

// Calls a virtual function of a guest object (vtable at +0) with r4, returns r3.
uint32_t CallVirtual(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t slot, uint32_t r4) {
  const uint32_t vtable = fcp::guest::U32(base, object);
  if (!fcp::guest::Readable(base, vtable + slot, 4)) {
    return 0;
  }
  PPCFunc* f = rex::ppc::ResolveFunction(fcp::guest::U32(base, vtable + slot));
  if (!f) {
    return 0;
  }
  return fcp::guest::Call(ctx, base, *f, {.r3 = object, .r4 = r4});
}

std::string Floats(uint8_t* base, uint32_t p) {
  if (!fcp::guest::Readable(base, p, 12)) {
    return ",,";
  }
  return std::format("{:.3f},{:.3f},{:.3f}", F32(base, p), F32(base, p + 4), F32(base, p + 8));
}

}  // namespace

void Configure() {
  if (fcp::env::Get("FCP_TRACE_STATE").empty()) {
    return;
  }
  g_every = static_cast<int>(std::max<int64_t>(1, fcp::env::Int("FCP_TRACE_STATE", 1)));
  std::error_code ec;
  std::filesystem::create_directories(fcp::log::Folder(), ec);
  const int gen = fcp::relaunch::Generation();
  const auto path = fcp::log::Folder() / (gen > 0 ? std::format("state.gen{}.csv", gen) : std::string("state.csv"));
  g_csv = std::fopen(path.string().c_str(), "w");
  if (!g_csv) {
    FCPLOG("FCP_TRACE_STATE: cannot write {}", path.string());
    return;
  }
  std::setvbuf(g_csv, nullptr, _IOFBF, 1 << 16);
  std::fprintf(g_csv,
               "frame,wall_s,game_s,level_s,dt_ms,x,y,z,ang_x,ang_y,ang_z,speed_mps,hspeed_mps,health,armor,"
               "in_water,under_water,breath,firing,reloading,jumping,clip0,clip1,clip2,clip3,shots\n");
  FCPLOG("FCP_TRACE_STATE: every {} frame(s) to {}", g_every, path.string());
}

void OnFrame(PPCContext& ctx, uint8_t* base) {
  if (!g_csv || !fcp::gametime::InLevel()) {
    return;
  }
  const uint64_t frame = fcp::gametime::Frame();
  if (frame % uint64_t(g_every) != 0) {
    return;
  }
  const uint32_t game = fcp::guest::U32(base, fcp::addr::game_ptr);
  if (!fcp::guest::Readable(base, game, 0x70)) {
    return;
  }
  uint32_t slot = fcp::guest::U32(base, game + kSlotPreferred);
  if (!slot) {
    slot = fcp::guest::U32(base, game + kSlotFirst);
  }
  if (!fcp::guest::Readable(base, slot, kPlayerEntityId + 4)) {
    return;
  }
  const uint32_t id = fcp::guest::U32(base, slot + kPlayerEntityId);
  const uint32_t system = fcp::guest::U32(base, fcp::addr::entity_system_ptr);
  if (!fcp::guest::Readable(base, system, 4)) {
    return;
  }
  const uint32_t entity = fcp::guest::Call(ctx, base, __imp__EntitySystem_GetEntity, {.r3 = system, .r4 = id});
  if (!fcp::guest::Readable(base, entity, 0x140)) {
    return;
  }
  const uint32_t pos = CallVirtual(ctx, base, entity, kEntityGetPos, 1);
  const uint32_t ang = CallVirtual(ctx, base, entity, kEntityGetAngles, 0);
  uint32_t player = fcp::guest::U32(base, entity + kEntityContainer);
  const bool have_player = fcp::guest::Readable(base, player, kPlayerInWater + 1);
  static uint8_t zeros[0x400] = {};
  uint8_t* pl = have_player ? fcp::guest::Ptr(base, player) : zeros;
  if (!have_player) {
    player = 0;
  }
  g_player = player;
  auto raw = [&](uint32_t off) {
    uint32_t v;
    std::memcpy(&v, pl + off, 4);
    return __builtin_bswap32(v);
  };
  auto pf = [&](uint32_t off) { return std::bit_cast<float>(raw(off)); };
  if (!g_discovered || entity != g_last_entity) {
    g_discovered = true;
    g_last_entity = entity;
    g_have_prev = false;
    FCPLOG("state: game {:08X} slot {:08X} entity id {} -> entity {:08X} (vtable {:08X}); player {:08X} (vtable "
           "{:08X}); pos at {:08X} (entity {:+#x}) = {}; angles at {:08X} (entity {:+#x}) = {}; health {:.2f} "
           "armor {:.2f} in water {} under water {}",
           game, slot, id, entity, fcp::guest::U32(base, entity), player, raw(0), pos,
           int64_t(pos) - int64_t(entity), Floats(base, pos), ang, int64_t(ang) - int64_t(entity), Floats(base, ang),
           pf(kPlayerHealth), pf(kPlayerArmor), pl[kPlayerInWater], pl[kPlayerUnderWater]);
    if (player && fcp::env::Flag("FCP_TRACE_STATE_SCAN")) {
      // Diagnosis: words that look like a full health (100.0f, 100, 255) in the player object.
      std::string hits;
      for (uint32_t off = 0; off < 0x1000; off += 4) {
        if (!fcp::guest::Readable(base, player + off, 4)) {
          break;
        }
        const uint32_t v = fcp::guest::U32(base, player + off);
        if (v == 0x42C80000u || v == 100u || v == 255u || v == 0x437F0000u || v == entity) {
          hits += std::format(" +{:#x}={:08X}", off, v);
        }
      }
      FCPLOG("state scan: player {:08X}:{}", player, hits);
      hits.clear();
      for (uint32_t off = 0; off < 0x600; off += 4) {
        const uint32_t v = fcp::guest::U32(base, entity + off);
        if (v == player || v == 0x42C80000u || v == 100u) {
          hits += std::format(" +{:#x}={:08X}", off, v);
        }
        // Objects the entity points to that point back to it (container candidates) and hold 100.0f.
        if (fcp::guest::Plausible(v) && v != entity && fcp::guest::Readable(base, v, 0x800)) {
          bool back = false;
          std::string full;
          for (uint32_t o2 = 0; o2 < 0x800; o2 += 4) {
            const uint32_t w = fcp::guest::U32(base, v + o2);
            back = back || w == entity;
            if (w == 0x42C80000u) {
              full += std::format(" {:#x}", o2);
            }
          }
          if (back && !full.empty()) {
            hits += std::format(" [+{:#x} -> {:08X} vt {:08X} back, 100.0f at{}]", off, v, fcp::guest::U32(base, v),
                                full);
          }
        }
      }
      FCPLOG("state scan: entity {:08X}:{}", entity, hits);
    }
  }
  const double game_s = fcp::gametime::GameSeconds();
  double speed = 0.0, hspeed = 0.0;
  float p[3] = {};
  const bool have_pos = fcp::guest::Readable(base, pos, 12);
  if (have_pos) {
    p[0] = F32(base, pos);
    p[1] = F32(base, pos + 4);
    p[2] = F32(base, pos + 8);
    const double dt = game_s - g_prev_game;
    if (g_have_prev && dt > 1e-6) {
      const double dx = p[0] - g_prev[0], dy = p[1] - g_prev[1], dz = p[2] - g_prev[2];
      hspeed = std::sqrt(dx * dx + dy * dy) / dt;  // CryEngine: z is up
      speed = std::sqrt(dx * dx + dy * dy + dz * dz) / dt;
    }
    if (!g_have_prev || dt > 1e-6) {
      std::memcpy(g_prev, p, sizeof(p));
      g_prev_game = game_s;
      g_have_prev = true;
    }
  }
  uint32_t clips[4] = {};
  if (player) {
    const uint32_t ammo = raw(kPlayerAmmoBlock);
    if (fcp::guest::Plausible(ammo) && fcp::guest::Readable(base, ammo + 0xC, 16)) {
      for (int i = 0; i < 4; ++i) clips[i] = fcp::guest::U32(base, ammo + 0xC + 4 * i);
    }
  }
  std::fprintf(g_csv, "%llu,%.4f,%.4f,%.4f,%.2f,%s,%s,%.3f,%.3f,%.2f,%.2f,%u,%u,%.2f,%u,%u,%u,%d,%d,%d,%d,%llu\n",
               static_cast<unsigned long long>(frame), fcp::gametime::WallSeconds(), game_s,
               fcp::gametime::LevelSeconds(), fcp::gametime::FrameDt() * 1000.0,
               have_pos ? std::format("{:.3f},{:.3f},{:.3f}", p[0], p[1], p[2]).c_str() : ",,",
               Floats(base, ang).c_str(), speed, hspeed, pf(kPlayerHealth), pf(kPlayerArmor),
               unsigned(pl[kPlayerInWater]), unsigned(pl[kPlayerUnderWater]), pf(kPlayerBreath), unsigned(pl[kPlayerFiring]),
               unsigned(pl[kPlayerReloading]), unsigned(pl[kPlayerJumping]), int32_t(clips[0]), int32_t(clips[1]),
               int32_t(clips[2]), int32_t(clips[3]),
               static_cast<unsigned long long>(g_player_shots));
  if (++g_rows % 300 == 0) {
    std::fflush(g_csv);
  }
}

uint32_t PlayerObject() {
  return g_player;
}

bool PlayerPosition(float out[3]) {
  if (!g_have_prev) {
    return false;
  }
  std::memcpy(out, g_prev, sizeof(g_prev));
  return true;
}

void OnWeaponFire(uint8_t* base, uint32_t shot) {
  if (g_csv && g_player && fcp::guest::Plausible(shot) && fcp::guest::U32(base, shot + kShotShooter) == g_player) {
    ++g_player_shots;
  }
}

void Shutdown() {
  if (g_csv) {
    std::fclose(g_csv);
    g_csv = nullptr;
    FCPLOG("FCP_TRACE_STATE: {} rows", g_rows);
  }
}

}  // namespace fcp::state
