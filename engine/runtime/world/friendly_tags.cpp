#include <runtime/world/friendly_tags.h>
#include <runtime/world/friendly_tag_gates.h>

#include <algorithm>
#include <cstdint>

#include <runtime/world/world.h>

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
    src.medic = world.tables.class_has_attribute(e.player_class, MissionTables::kCharAttrMedic);
    // The dead latch the bad tier's downed legs read [orig: `Flags & 2`
    // @0x5a3c1c..0x5a3c27]; our registry mirrors the kill's `|= 6` in both
    // the flag word and `alive` [orig: @0x43fbf6].
    src.dead = !e.alive || (e.flags & kEntityFlagDead) != 0;
    // The radio-request icon's per-tag fold: the +885 latch, then the
    // carrier walk clears it [orig: `cmp byte ptr [ebx+375h], 0; jz`
    // @0x5a3bfe; Entity_FindChildByDefType(entity, 1, 1) @0x5a3c0e;
    // `xor ebp, ebp` @0x5a3c1a].
    src.radio_request = e.radio_request != 0 &&
            !friendly_tag_aboard_vehicle(world, e.ground_target);
    if (slot != nullptr) {
        src.has_slot = true;
        src.revive_seconds = slot->revive_seconds;
        src.medic_request = slot->medic_request;
    }
    out.push_back(src);
}

// The drawer's entry bails and the pass gates are the shared predicates in
// world/friendly_tag_gates.h (the joiner's roster walk applies the same
// ones): never the local player, never a CARRIED entity, only with a
// resolved item def [orig: @0x5a39df / @0x5a39eb / @0x5a39fb]; team 0 / the
// local team / the death screen, then `g_GameType || death screen`
// [orig: @0x5a44c7..0x5a44f8, @0x5a4552..0x5a457d], then the drawer's
// equal-team test outside the death screen [orig: @0x5a3c6b..0x5a3c95].
bool entry_bails(const Entity &e, const Entity &local) {
    return friendly_tag_entry_bails(e.handle == local.handle, e.flags,
                                    e.has_item_def);
}

bool pass_gates(uint8_t team, const Entity &local,
                const FriendlyTagPassContext &ctx) {
    return friendly_tag_pass_gates(team, local.team, ctx.death_screen,
                                   ctx.game_type);
}

} // namespace

bool friendly_tag_aboard_vehicle(const World &world, EntityHandle first) {
    // [orig: Entity_FindChildByDefType @0x43bea0 — child = groundEntity
    //  @0x43bea4; the itemDef NULL stop @0x43bec5 precedes the iteration
    //  bound @0x43beca; `def->type == 1` @0x43becf (findFirst: the first hit
    //  returns); child = child->groundEntity @0x43bed7]
    const Entity *link = world.registry.get(first);
    for (int iteration = 1; link != nullptr; ++iteration) {
        if (!link->has_item_def) return false;
        if (iteration >= 20) return false;
        if (link->item_type == 1) return true;
        link = world.registry.get(link->ground_target);
    }
    return false;
}

bool friendly_tag_radio_request_viewer(const Entity &local) {
    // [orig: HUD_DrawEntityLabel @0x5a3bba..0x5a3be8 — var_DC = 1 when
    //  playerEntity+0x168 is 2 (@0x5a3bd1) or 5 (@0x5a3bd6), or its own
    //  +885 latch is set (@0x5a3bdf); the mount word is our SeatType]
    return is_vehicle_control_seat(local.mount_type) || local.radio_request != 0;
}

void collect_friendly_tags(World &world, const Entity &local,
                           std::vector<FriendlyTagSource> &out,
                           const FriendlyTagPassContext &ctx) {
    // The session rule FriendlyTag 0 (rules word bit 0x400) returns before
    // either walk [orig: `test g_RulesFlags,400h; jnz locret` @0x5a4480..0x5a448a].
    if (ctx.rules_no_friendly_tags) return;
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
