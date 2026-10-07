#include "weapon_fire_gate.h"

#include <base/io/bam.h>
#include <algorithm>
#include <cmath>
#include <runtime/world/world.h>

namespace opennova::world {
void weapon_fire_environment_inputs(const World &world, const Entity &owner,
                                    WeaponFsmInputs &inputs) {
    // [orig: WeaponSlot_CanFire @0x541bfa..0x541c39]
    inputs.head_submerged = io::bam_add(to_fixed(owner.position.z), owner.eye_offset_z) <
            world.env.water_z;
    inputs.drowning = ((owner.flags | owner.engine_flags) & 0x8000u) != 0;
    inputs.ignore_ammo_cost = world.rules.ignore_weapon_ammo_cost;
    // The outer loop's catch-up flag the fire-loop emitter reads
    // [orig: dword_24E0E80 @0x5412a7; Game_MainLoop @0x52ba32..0x52ba3a].
    inputs.last_tick_of_batch = world.rules.last_tick_of_batch;
    inputs.protected_carrier = false;
    // First type-1 carrier in the 19-link groundEntity walk.
    // [orig: Entity_FindChildByDefType @0x43bea0]
    const Entity *carrier = nullptr;
    const Entity *candidate = world.registry.get(owner.ground_target);
    for (int depth = 1; candidate != nullptr && depth < 20; ++depth) {
        if (!candidate->has_item_def) return;
        if (candidate->item_type == 1) { carrier = candidate; break; }
        candidate = world.registry.get(candidate->ground_target);
    }
    if (carrier == nullptr || carrier->item_type != 1 ||
            (carrier->carry_flags & 0x40u) == 0) return;
    const Entity *zone = world.registry.get(carrier->ground_target);
    if (zone == nullptr || !zone->has_item_def || (zone->item_attrib2 & 0x2000u) == 0)
        return;
    // CF contact within a friendly FARP blocks firing. Unnumbered FARPs use
    // team equality in team modes, and protect everyone in non-team modes.
    // [orig: sub_44A2B0 @0x44a2b0..0x44a334]
    if (zone->zone_number != 0) {
        if (carrier->team < ZoneChain::kTeamCount)
            inputs.protected_carrier = (world.zones.chain.owned_mask[carrier->team] &
                    (1u << (zone->zone_number & 31))) != 0;
    } else {
        inputs.protected_carrier = (world.match.rules().game_type & 0x10000u) == 0 ||
                zone->team == carrier->team;
    }
}
// The owner/definition rejects precede ammo spending and either spawn arm.
// [orig: Server_ClientFiredRound @0x50bad7..0x50bb06, @0x50c069..0x50c0b9]
int weapon_fire_owner_status(const World &world, const Entity &owner,
                             const WeaponTableEntry *weapon, bool alternate) {
    const uint32_t flags = owner.flags | owner.engine_flags;
    if ((flags & 0x102u) == 0x102u) return -10;
    if (alternate) return 0;
    if (weapon == nullptr) return -3;
    if ((weapon->flags & weapon_flag::kEmplaced) != 0 &&
            world.registry.get(owner.mount_target) == nullptr)
        return (flags & 2u) != 0 ? -18 : -12;
    // The special-ammo global A2ECF0 is zero and has only this reader in the
    // retail image. Its misleading CharAttr_ClassHasAttribute callee tests CHARATTR
    // Medic, not animation state. Preserve the actual zero-index gate.
    if (weapon->ammo_index == 0 &&
            !world.tables.class_has_attribute(owner.player_class, MissionTables::kCharAttrMedic))
        return -14;
    return 0;
}

// Direct round submission checks horizontal and vertical reach separately on
// foot; a gunner uses full 3-D distance and the mount's doubled bound radius.
// The replay arm returns before this block. Carriers whose saved pose moved
// and WeaponDef flag 0x200 also bypass it.
// [orig: Server_ClientFiredRound @0x50c172..0x50c6fe]
int weapon_fire_origin_status(const World &world, const Entity &owner,
                              const WeaponTableEntry &weapon, const FixedVec3 &origin,
                              bool replay_client_action) {
    if (replay_client_action || (weapon.flags & 0x200) != 0 ||
            ((owner.flags | owner.engine_flags) & 0x100u) == 0) return 0;
    const Entity *carrier = world.registry.get(owner.ground_target);
    if (carrier != nullptr && carrier->saved_live_valid &&
            (to_fixed(carrier->position.x) != carrier->saved_live_pos[0] ||
             to_fixed(carrier->position.y) != carrier->saved_live_pos[1] ||
             to_fixed(carrier->position.z) != carrier->saved_live_pos[2] ||
             static_cast<int32_t>(uint32_t(carrier->yaw) << 16) != carrier->saved_live_yaw ||
             static_cast<int32_t>(uint32_t(carrier->pitch) << 16) != carrier->saved_live_pitch ||
             static_cast<int32_t>(uint32_t(carrier->roll) << 16) != carrier->saved_live_roll)) return 0;
    const int32_t dx = io::bam_sub(to_fixed(owner.position.x), origin.x);
    const int32_t dy = io::bam_sub(to_fixed(owner.position.y), origin.y);
    const int32_t dz = io::bam_sub(to_fixed(owner.position.z), origin.z);
    int32_t limit = 0x20000;
    double squared = double(dx) * dx + double(dy) * dy;
    if (owner.mount_type == SeatType::Gunner) {
        squared += double(dz) * dz;
        const Entity *mount = world.registry.get(owner.mount_target);
        limit = mount != nullptr ? to_fixed(2.0f * mount->bound_radius) : 0x30000;
        // A gunner on an attached, parent-routed mountable gun
        // (Entity_IsMountableGun: itemdef && type != 1 && attrib & 0x20; the
        // attachment flag +0x326 & 2; the MountSlot+0x5E redirect bit at
        // entity+0x312 & 8) measures against its type-1 EWEAP hull instead.
        // [orig: Server_ClientFiredRound @0x50c43e..0x50c475;
        //  Entity_IsMountableGun @0x434240]
        if (mount != nullptr && mount->has_item_def && mount->item_type != 1 &&
                (mount->item_attrib & kItemAttribEweap) != 0 &&
                (mount->emplacement_attachment_flags & 2u) != 0 &&
                mount->primary_weapon_slot.redirect_to_parent_slot) {
            const Entity *parent = world.registry.get(mount->ground_target);
            if (parent != nullptr && parent->has_item_def && parent->item_type == 1 &&
                    (parent->item_attrib & 0x20u) != 0)
                limit = to_fixed(2.0f * parent->bound_radius);
        }
    }
    const int32_t distance = static_cast<int32_t>(std::min(std::sqrt(squared), 2147418112.0));
    return distance > limit || io::bam_abs(dz) > limit ? -17 : 0;
}

}
