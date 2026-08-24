#include "world/friendly_tags.h"

#include <algorithm>
#include <cstdint>

#include "world/world.h"

namespace opennova::world {

namespace {

// The per-entity facts the drawer reads once past its entry bails
// [orig: HUD_DrawEntityLabel @0x5a39b0]. `slot` is NULL for the pool-0 walk
// and the connection slot for the player walk.
void push_tag(World &world, const Entity &e, const PlayerSlotFacts *slot,
              std::vector<FriendlyTagSource> &out) {
    FriendlyTagSource src;
    src.entity = e.handle;
    src.net_id = e.net_id;
    src.position = e.position;
    // The anchor's eye lift rides along: z + entity[+116] + 0x4000
    // [orig: HUD_DrawEntityLabel @0x5a3a84..0x5a3a98].
    src.eye_offset_z = e.eye_offset_z;
    src.name = e.display_name;
    src.player = (e.flags & kEntityFlagPlayer) != 0;
    // health<<16 / max, clamped to 1.0 — the difficulty-scaled max
    // (Entity_GetMaxHealthWithDifficulty) is the base def hp here; the
    // difficulty term is the record's residue [orig: @0x5a3b91..0x5a3bb8,
    // clamp mirror of HUD_BuildEntityInfo @0x4b87d1].
    const int64_t max_health = std::max(1, e.health_max);
    const int64_t ratio =
            (static_cast<int64_t>(std::max(0, e.health)) << 16) / max_health;
    src.health_ratio_fp16 =
            static_cast<int32_t>(std::min<int64_t>(ratio, 0x10000));
    // The medic plate keys on the class's charattr ATTRIBUTES & 8
    // [orig: AnimMap_IsSlotActive(playerClass, 8) @0x4125e0].
    src.medic = world.class_has_attribute(e.player_class, World::kCharAttrMedic);
    // The dead latch the bad tier's downed legs read [orig: `Flags & 2`
    // @0x5a3c1c..0x5a3c27]; our registry mirrors the kill's `|= 6` in both
    // the flag word and `alive` [orig: @0x43fbf6].
    src.dead = !e.alive || (e.flags & kEntityFlagDead) != 0;
    if (slot != nullptr) {
        src.has_slot = true;
        src.revive_seconds = slot->revive_seconds;
        src.medic_request = slot->medic_request;
    }
    out.push_back(src);
}

// The drawer's entry bails, shared by both walks: never the local player,
// never a CARRIED entity (Flags & 1 — hidden while attached), and only with
// a resolved item def [orig: @0x5a39df entity == playerEntity; @0x5a39eb
// Flags & 1; @0x5a39fb itemDef == NULL]. Dead entities are NOT bailed: the
// bad tier's downed legs are how a fallen teammate keeps its label.
bool entry_bails(const Entity &e, const Entity &local) {
    if (e.handle == local.handle) return true;
    if ((e.flags & kEntityFlagCarried) != 0) return true;
    if (!e.has_item_def) return true;
    return false;
}

// Team gate: neutral or the local team, or anyone while the death screen is
// up; then the mode gate `g_GameType || death screen`. The enemy magenta leg
// is server-granted spectator state (g_enemyTagsVisible), unported
// [orig: pool-0 @0x5a44c7..0x5a44f8; players @0x5a4552..0x5a457d;
//  the @0x5a3c7d enemy bail].
bool pass_gates(uint8_t team, const Entity &local,
                const FriendlyTagPassContext &ctx) {
    if (team != 0 && team != local.team && !ctx.death_screen) return false;
    if (ctx.game_type == 0 && !ctx.death_screen) return false;
    return true;
}

} // namespace

void collect_friendly_tags(World &world, const Entity &local,
                           std::vector<FriendlyTagSource> &out,
                           const FriendlyTagPassContext &ctx) {
    // Walk 1: the pool-0 organic loop skips the Player class bit
    // [orig: @0x5a44b0, `test [eax+24h], 100h` @0x5a44b9].
    world.registry.for_each([&](const Entity &e) {
        if (e.kind != EntityKind::Organic) return;
        if ((e.flags & kEntityFlagPlayer) != 0) return;
        if (entry_bails(e, local)) return;
        if (!pass_gates(e.team, local, ctx)) return;
        push_tag(world, e, nullptr, out);
    });
    // Walk 2: the player-slot table — every active slot with an entity
    // [orig: @0x5a4507..0x5a4597 — slot+0x0D active, slot+0x24 entity,
    //  entity+0x162 team vs the local team, then HUD_DrawEntityLabel(entity,
    //  slot)]. The slot owner is the embedder's connection table; an entity
    // no slot owns is never visited (retail walks slots, not entities).
    if (ctx.slot_lookup == nullptr || !*ctx.slot_lookup) return;
    world.registry.for_each([&](const Entity &e) {
        if (e.kind != EntityKind::Organic) return;
        if ((e.flags & kEntityFlagPlayer) == 0) return;
        PlayerSlotFacts facts;
        if (!(*ctx.slot_lookup)(e.handle, facts)) return;
        if (entry_bails(e, local)) return;
        if (!pass_gates(e.team, local, ctx)) return;
        push_tag(world, e, &facts, out);
    });
}

} // namespace opennova::world
