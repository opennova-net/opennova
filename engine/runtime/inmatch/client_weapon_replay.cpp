#include "client_weapon_replay.h"
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/world.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/powerup.h>
#include <algorithm>
#include <vector>

namespace opennova::inmatch {
namespace {
world::SeatType seat_type(const replication::ClientEntityState &row, const world::Entity &mount) {
    if (row.mount_bone != 0)
        for (const auto &seat : mount.seats)
            if (seat.bone_index == row.mount_bone) return seat.type;
    return world::SeatType::None;
}
world::Entity *selected_slot_owner(world::World &world, world::Entity &mount) {
    auto *slot = world.vehicles.resolve_mounted_ammo_slot(mount);
    if (slot == &mount.primary_weapon_slot) return &mount;
    auto *parent = world.registry.get(mount.ground_target);
    return parent && slot == &parent->primary_weapon_slot ? parent : nullptr;
}
bool local_slot(world::World &world, const world::Entity &mount) {
    return world.local_player_state &&
            world::active_local_weapon_slot(world, world.local_player_state->weapon) ==
                    &mount.primary_weapon_slot;
}
}

// Replica organics borrow the exact materialized carrier slot. An ordinary
// on-foot remote has no EquippedSlot: its equipped-ADM byte is display state.
// [orig: Entity_AttachToUseGunSlot @0x546B80; Entity_AttachToVehicleSlot @0x4946D0;
//  NetPacket_SerializePlayerState @0x4C09C0]
void sync_replica_weapon_slots(replication::ClientState &state, world::World &world,
        uint16_t self_handle) {
    for (auto &row : state.entities) {
        row.weapon_slot_handle = world::EntityHandle::kInvalid;
        if (row.handle == self_handle ||
                (row.cls != EntityClass::Player && row.cls != EntityClass::Infantry)) continue;
        auto *mount = world.registry.get(world::EntityHandle{row.carrier_handle});
        if (!mount || !mount->has_item_def || !(mount->item_attrib & world::kItemAttribEweap))
            continue;
        const auto type = seat_type(row, *mount);
        if (type != world::SeatType::Gunner && type != world::SeatType::Controller) continue;
        if (!world.vehicles.prepare_weapon_slot(*mount) || local_slot(world, *mount)) continue;
        mount->primary_weapon_owner = world::EntityHandle{row.handle};
        if (type == world::SeatType::Gunner && mount->item_type != 1u &&
                (mount->emplacement_attachment_flags & 2u)) {
            auto *parent = world.registry.get(mount->ground_target);
            const bool parent_valid = parent && mount->emplacement_parent == parent->handle &&
                    mount->emplacement_parent_spawn_id == parent->registry_spawn_id &&
                    parent->has_item_def && parent->item_type == 1u &&
                    (parent->item_attrib & world::kItemAttribEweap) &&
                    world.vehicles.prepare_weapon_slot(*parent);
            if (parent_valid && !local_slot(world, *parent))
                parent->primary_weapon_owner = world::EntityHandle{row.handle};
            if (row.seat_type == 1) mount->primary_weapon_slot.redirect_to_parent_slot = false;
            else if (row.seat_type == 2 && parent_valid)
                mount->primary_weapon_slot.redirect_to_parent_slot = true;
        }
        if (auto *selected = selected_slot_owner(world, *mount))
            row.weapon_slot_handle = selected->handle.packed;
    }
}

// Wire ADM selects the rows; selected-slot def supplies kick/heat. A missing
// muzzle byte skips both actions while leaving the projectile spawn untouched.
// [orig: NetPacket_DeserializeRoundEvent @0x42F270]
void replica_weapon_action_source(const replication::ClientEntityState &shooter,
        world::World &world, uint8_t flags, world::RoundSourceState &source) {
    auto *mount = world.registry.get(world::EntityHandle{shooter.carrier_handle});
    if (!mount) return;
    const auto type = seat_type(shooter, *mount);
    if (type != world::SeatType::Controller && type != world::SeatType::Gunner) return;
    source.mounted_action = true;
    source.action_allowed = false;
    if (type == world::SeatType::Controller && !(mount->item_attrib & world::kItemAttribEweap))
        return;
    if (world::weapon_userpoint_byte(*mount, (flags >> 4) & 3, 1) == 0) return;
    auto *selected = world.registry.get(world::EntityHandle{shooter.weapon_slot_handle});
    if (!selected) return;
    const auto *def = world.tables.weapons.by_index(selected->primary_weapon_slot_adm);
    if (!def) return;
    source.action_slot = &selected->primary_weapon_slot;
    source.action_slot_def = &def->action_fsm;
    source.action_allowed = true;
}

// A person row's mirror is its 0x0C word, which the replica resolver re-derives
// every motion tick, as retail's person update does. A pool-1 row's bit is
// recomputed here, at the round event, by the BB walk retail's client runs on
// the entity on each think visit (+0x2AC nonpositive), which the def's class
// callback re-arms (the lighting feed's walk, role_feeds.cpp): none for a def
// of type 5, the candidate slice (+0x1BC / +0x1C0) for type 1 or 3, the static
// building prefix (which never holds a pool-1 source, so the self-skip is
// moot) otherwise. For a brainless row of the vehicle classes (cveh, chel,
// cbot, cpln, ctrn), one a 0x0D lands after the mission start's class-init
// pass, and for a class with no handler, the visit runs every tick and the
// walk matches it; a vehicle the load stream brought (brained at the mission
// start) and any other class differ (D-COL-13). Another row keeps its received
// word, where retail's staggered pool-2 loop rebuilds the bit (D-COL-13).
// [orig: NapiNPClientMsg_0x00D @0x432D7D; Entity_UpdateAllEntities @0x4C2100,
// the pool-1 walk @0x4C21B9..0x4C21E9 -> Entity_UpdatePool1Slot - the +0x2AC
// test @0x4B8E1B, the call @0x4B8E25; Entity_BuildProximityList @0x4B3DC0 -
// the type-5 test @0x4B3E4A..0x4B3E4D, the candidate walk @0x4B3F3E..0x4B3F93,
// the static prefix @0x4B3E65..0x4B3F39; the pool-2 call @0x4C229C]
bool replica_shooter_indoors(mission::MissionKernel &kernel,
        const replication::ClientEntityState &row) {
    const world::EntityHandle h{row.handle};
    if (h.pool() == 0) return (row.rm_entity_flags & world::kEntityFlagIndoors) != 0;
    if (h.pool() != 1)
        return ((row.spawn_entity_flags | row.rm_entity_flags) & world::kEntityFlagIndoors) != 0;
    const uint8_t def_type = kernel.wire_collision_shape_for_type(row.type_id).item_type;
    if (def_type == 5) return false; // no walk: the cleared bit stays clear
    const int32_t at[3] = {row.x, row.y, row.z};
    world::BlinkAccum blink;
    kernel.collision.query_wire_blink_boxes_at_point(
            kernel.world, row.handle, at, def_type == 1 || def_type == 3, blink);
    return (blink.flags & world::kBlinkIndoorsBit) != 0;
}

// One pump per selected slot: occupied guns via their pool-0 borrower, then
// unoccupied pool-1 guns with a heat window. Detach retains the slot's last owner.
// [orig: WeaponAction_ProcessAllEntities @0x542690; WeaponAction_ProcessFrame @0x540E60]
void tick_replica_weapon_slots(replication::ClientState &state, world::World &world) {
    std::vector<uint16_t> borrowers;
    for (const auto &row : state.entities)
        if (row.type_id && row.weapon_slot_handle != 0xFFFF) borrowers.push_back(row.handle);
    std::sort(borrowers.begin(), borrowers.end());
    std::vector<uint16_t> pumped;
    const auto pump = [&](world::Entity &entity) {
        auto *mount = &entity;
        if (local_slot(world, *mount)) return;
        const auto *def = world.tables.weapons.by_index(mount->primary_weapon_slot_adm);
        if (!def) return;
        auto &slot = mount->primary_weapon_slot;
        world::Entity owner;
        const auto *row = state.find(mount->primary_weapon_owner.packed);
        const auto *native_owner = world.registry.get(mount->primary_weapon_owner);
        if (row) {
            owner.handle = world::EntityHandle{row->handle};
            owner.position = {float(row->x)/65536.0f, float(row->y)/65536.0f, float(row->z)/65536.0f};
            owner.mount_target = world::EntityHandle{row->carrier_handle};
            if (const auto *carrier = world.registry.get(owner.mount_target))
                owner.mount_type = seat_type(*row, *carrier);
        } else if (native_owner) owner = *native_owner;
        world::WeaponFsmInputs input;
        input.owner_present = row || native_owner;
        input.is_local = false;
        input.is_authority = false;
        // The one process global every slot reads [orig: g_AutoReloadEnabled
        // @0x24D2118; WeaponAction_Idle @0x5429a6 tests no owner].
        input.auto_reload = world.rules.auto_reload;
        input.current_tick = static_cast<int32_t>(world.logic_tick);
        input.submerged = world.env.water_z != 0 && world::to_fixed(owner.position.z) <= world.env.water_z;
        input.last_tick_of_batch = world.rules.last_tick_of_batch;
        world::WeaponFsmEvents events;
        world::weapon_fsm_tick(def->action_fsm, slot, input, events);
        world::weapon_sound_publish(world, owner, def->action_fsm, events);
    };
    for (const auto handle : borrowers) {
        const auto *row = state.find(handle);
        auto *mount = world.registry.get(world::EntityHandle{row->weapon_slot_handle});
        if (!mount || std::find(pumped.begin(), pumped.end(), mount->handle.packed) != pumped.end())
            continue;
        pumped.push_back(mount->handle.packed);
        pump(*mount);
    }
    for (size_t i = 0; i < world.registry.pool_capacity(1); ++i) {
        auto *mount = world.registry.get(world::EntityHandle::make(1, static_cast<int>(i)));
        if (!mount || !mount->has_item_def || !(mount->item_attrib & world::kItemAttribEweap) ||
                mount->primary_occupant.valid() || mount->primary_weapon_slot.heat_window_end_tick == 0 ||
                std::find(pumped.begin(), pumped.end(), mount->handle.packed) != pumped.end()) continue;
        pump(*mount);
    }
}

void apply_weapon_pickups(JoinerRole &role, ClientRuntime &rt, world::World &world,
        world::LocalPlayer &lp) {
    for (const WeaponPickupNotice &notice : rt.drain_weapon_pickups()) {
        // Only the picker's own client acts [orig: Player_MountPickedUpWeapon @0x4E03DC]
        if (!rt.has_self_handle() || notice.picker_handle != rt.self_handle()) continue;
        world::EntityHandle row_handle;
        row_handle.packed = notice.powerup_handle;
        world::powerup_weapon_grant_received(world, lp,
                role.replica_world_entity(world, row_handle), rt.local_player_dead());
    }
}
} // namespace opennova::inmatch
