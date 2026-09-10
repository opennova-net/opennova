#pragma once

// The friendly-tags gather (D-HUD-20): which entities get an overhead name
// label this frame, with the per-entity facts the HUD compiler's element
// consumes (engine/runtime/hud hud_frame.h HudFriendlyTag). The projection,
// view distance, and fog feed are the presenter's; every selection gate here
// is the witnessed pass. [orig: HUD_DrawFriendlyTagsPass @0x5a4480 +
// the HUD_DrawEntityLabel entry bails @0x5a39eb..0x5a39ff]
// Witness record: docs/interface/hud-re.md (D-HUD-20).

#include <runtime/world/entity.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova::world {

// World is a CLASS (world.h): the class-key must match, MSVC mangles it into
// the symbol and a struct/class mismatch fails the GDExtension link.
class World;

struct FriendlyTagSource {
    EntityHandle entity;
    uint16_t net_id = 0; // the fallback-name index [orig: (pool<<12)|slot]
    Vec3 position;       // raw entity position; the presenter lifts + projects
    std::string name;    // authored display name; empty -> the compiled-in table
    int32_t health_ratio_fp16 = 0x10000;
    // The entity's eye-offset z (entity+0x74, 16.16): the anchor is
    // position.z + this + 0x4000 [orig: HUD_DrawEntityLabel @0x5a3a84..0x5a3a98].
    int32_t eye_offset_z = 0;
    bool player = false;
    // The class's charattr Medic attribute — the red-cross plate feed
    // [orig: AnimMap_IsSlotActive(entity+0x294 playerClass, 8) @0x4125e0,
    //  read by HUD_DrawEntityLabel for the plate @0x5a4309].
    bool medic = false;
    // The DOWNED legs of the bad tier [orig: HUD_DrawEntityLabel — the dead
    // latch `Flags & 2` @0x5a3c1c..0x5a3c27, the slot walk's slot pointer
    // (NULL for the pool-0 organics @0x5a44fa, the slot for players @0x5a457f),
    // slot+0x10 = the 1 Hz revive countdown, slot+0x2C = the medic-request
    // latch @0x5a3ddd..0x5a3df5].
    bool dead = false;
    bool has_slot = false;
    uint8_t revive_seconds = 0;
    bool medic_request = false;
};

// The facts a player's connection slot contributes to its tag [orig: the
// PlayerSlot bytes +0x10 (revive seconds) and +0x2C (medic request), written
// by PlayerSlot_SetDownedState @0x4348d0 from S2C 0x54 / 0x46 bit 0x0008].
struct PlayerSlotFacts {
    uint8_t revive_seconds = 0;
    bool medic_request = false;
};

// Resolves the connection slot driving a pool-0 player entity. Returns false
// when no slot owns the entity (retail's walk only visits slots WITH an
// entity, so an unowned player never reaches the drawer).
using PlayerSlotLookup =
    std::function<bool(EntityHandle entity, PlayerSlotFacts &out)>;

// The pass-level facts both walks read [orig: g_death_screen_active
// @0x5a44df/@0x5a4564, g_GameType @0x5a44e8/@0x5a456d].
struct FriendlyTagPassContext {
    const PlayerSlotLookup *slot_lookup = nullptr;
    bool death_screen = false;
    uint32_t game_type = 0;
    // The session's rules word bit 0x400 (host option FriendlyTag 0): the
    // whole pass returns before either walk [orig: `test g_rules_flags,400h;
    // jnz locret` @0x5a4480..0x5a448a — g_rules_flags @0x24D1E34 is the
    // session descriptor's +44 word: the host's mp_attributes, a joiner's
    // S2C 0x64 fixed block]. The same bit nulls Entity_GetDisplayName
    // @0x59BF70 for the map labels.
    bool rules_no_friendly_tags = false;
};

// The two walks of the tags pass [orig: HUD_DrawFriendlyTagsPass @0x5a4480]:
//   1. pool-0 entities WITHOUT the Player class bit — the AI organics —
//      team 0 / local team / death screen, `g_GameType || death screen`
//      [orig: @0x5a44b0..0x5a4505, slot = NULL];
//   2. the player-slot table: every active slot with an entity, the same team
//      and game-type gates [orig: @0x5a4507..0x5a4597, slot passed].
// The drawer's entry bails apply to both: never the local player, never a
// CARRIED (Flags & 1) entity, only with a resolved item def
// [orig: @0x5a39df/@0x5a39eb/@0x5a39fb] — a DEAD entity is still labelled
// (the bad tier's downed legs read the dead bit @0x5a3dc9).
// The enemy-visibility grant and the squad/channel legs are the record's
// documented residues.
void collect_friendly_tags(World &world, const Entity &local,
                           std::vector<FriendlyTagSource> &out,
                           const FriendlyTagPassContext &ctx = {});

} // namespace opennova::world
