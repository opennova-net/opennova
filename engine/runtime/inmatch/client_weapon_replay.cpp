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
// The record's route legs run on the remote's parent when it is a mountable gun
// with the G capability, whatever the seat, byte 2 only onto a groundEntity that
// is a type-1 EWeap (D-NET-860). The slots are the ones the guns' class
// inits bound at their spawn; no leg here binds or seeds one.
// [orig: Entity_AttachToUseGunSlot @0x546B80; Entity_AttachToVehicleSlot @0x4946D0;
//  NetPacket_SerializePlayerState @0x4C09C0 - the parent @0x4C1346, the gates
//  @0x4C1367..0x4C137E, byte 1 @0x4C1390 / @0x4C139D, byte 2's groundEntity gates
//  @0x4C13C1..0x4C13E5 and @0x4C13EB / @0x4C13F8; WeaponSlot_InitFromEntityDef @0x5466C0
//  runs from the class inits alone]
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
        if (local_slot(world, *mount)) continue;
        mount->primary_weapon_owner = world::EntityHandle{row.handle};
        if (world::mount_is_designated_g_gun(*mount)) {
            auto *parent = world.registry.get(mount->ground_target);
            const bool parent_valid = world::designated_g_route_carrier(parent);
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
// every motion tick, as retail's person update does. Any other row's bit is its
// entity's Flags word, as the round stamp reads it: the 0x0D landed it, and the
// entity pass rebuilds it, which on a joiner is the row's native twin's (the
// materializer seeds the twin's Flags from the 0x0D word). The World's pass visits
// a pool-1 twin behind its +0x2AC think clock with the class re-arms
// (World::update_pool1_slot) and the brain and stagger the mission start's class
// init gave a row the load stream brought (JoinerRole's pool-1 class init), and a
// pool-2 twin on its tick & 7 cohort (tick_item_event_pool), each visit's BB walk
// (CollisionWorld::refresh_blink) over the twin's own candidate slice, behind the
// frame's gates (World::run_entity_pass); the 0x2F's receipt rebuild is the
// materializer's. A row with no twin keeps its received word.
// [orig: RoundData_SpawnRound @0x4EC0D0 - the shooter's Flags @0x4EC830;
//  NapiNPClientMsg_0x00D @0x432C40 - the memset @0x432D1B..0x432D23, the Flags store
//  @0x432D7D; Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C21B9..0x4C21E9,
//  the pool-2 cohort @0x4C225A, its call @0x4C229C; Entity_UpdatePool1Slot - the +0x2AC
//  test @0x4B8E1B, the call @0x4B8E25; Entity_BuildProximityList @0x4B3DC0;
//  NapiNPClientMsg_0x02F @0x4310EC]
bool replica_shooter_indoors(const replication::ClientEntityState &row,
        const world::Entity *twin) {
    const world::EntityHandle h{row.handle};
    if (h.pool() == 0) return (row.rm_entity_flags & world::kEntityFlagIndoors) != 0;
    if (twin != nullptr) return (twin->flags & world::kEntityFlagIndoors) != 0;
    return ((row.spawn_entity_flags | row.rm_entity_flags) & world::kEntityFlagIndoors) != 0;
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
    // walked_z is the Z of the entity the walk hands the frame and `walk` that entity as
    // the overheat leg reads it: the borrowing body for a borrowed slot, the gun itself
    // for an unoccupied one.
    // [orig: WeaponAction_ProcessAllEntities @0x542690 - the pool-0 push @0x5426BF, the
    //  pool-1 push @0x542714]
    const auto pump = [&](world::Entity &entity, int32_t walked_z,
                          const world::HeatGlowWalk &walk) {
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
            owner.position.set_q16({row->x, row->y, row->z});
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
        // The heat window's water gate compares the walked entity's Z with the plane raw
        // (no dry sentinel), never the slot owner's [orig: WeaponAction_ProcessFrame
        // @0x540E60 - the walked entity's Z @0x541004, `cmp eax, g_EnvWaterHeightFixed;
        // jg` @0x541007..0x54100D].
        input.submerged = walked_z <= world.env.water_z;
        input.last_tick_of_batch = world.rules.last_tick_of_batch;
        world::WeaponFsmEvents events;
        world::weapon_fsm_tick(def->action_fsm, slot, input, events);
        world::weapon_heat_glow_update(world, walk, *def, slot, events);
        world::weapon_sound_publish(world, owner, def->action_fsm, events);
    };
    for (const auto handle : borrowers) {
        const auto *row = state.find(handle);
        auto *mount = world.registry.get(world::EntityHandle{row->weapon_slot_handle});
        if (!mount || std::find(pumped.begin(), pumped.end(), mount->handle.packed) != pumped.end())
            continue;
        pumped.push_back(mount->handle.packed);
        world::HeatGlowWalk walk; // the body's parentEntity is the carrier it rides
        walk.carrier = world.registry.get(world::EntityHandle{row->carrier_handle});
        walk.usegun_seat = walk.carrier != nullptr &&
                seat_type(*row, *walk.carrier) == world::SeatType::Gunner;
        pump(*mount, row->z, walk);
    }
    // The world's pool-1 rows to its used count, which every twin's slotted spawn raises
    // [orig: WeaponAction_ProcessAllEntities @0x542690 - @0x5426D1].
    const size_t pool1_used = world.registry.pool_used(1);
    for (size_t i = 0; i < pool1_used; ++i) {
        auto *mount = world.registry.get(world::EntityHandle::make(1, static_cast<int>(i)));
        if (!mount || !mount->has_item_def || !(mount->item_attrib & world::kItemAttribEweap) ||
                mount->primary_occupant.valid() || mount->primary_weapon_slot.heat_window_end_tick == 0 ||
                std::find(pumped.begin(), pumped.end(), mount->handle.packed) != pumped.end()) continue;
        world::HeatGlowWalk walk;
        walk.gun = mount;
        pump(*mount, mount->position.z.q16, walk);
    }
}

void apply_weapon_pickup(JoinerRole &role, ClientRuntime &rt, world::World &world,
        world::LocalPlayer &lp, const WeaponPickupNotice &notice) {
    // Only the picker's own client acts [orig: Player_MountPickedUpWeapon @0x4E03DC]
    if (!rt.has_self_handle() || notice.picker_handle != rt.self_handle()) return;
    world::EntityHandle row_handle;
    row_handle.packed = notice.powerup_handle;
    // The row is the one the world already holds: the after-receive runs this ahead of the
    // frame's replica materialization, so a powerup row born in this same receive is not
    // found. Its weapon word +0x2B0 has one writer, the pickup's contact pass in a world
    // tick on a row materialized earlier, so such a row carries weapon 0 and the grant
    // refuses it either way [orig: PowerupAction_Pickup @0x4428A0 - the store @0x4428FB].
    // Should a fresh row ever take its weapon word from the wire, this lookup moves behind
    // the materialization.
    world::powerup_weapon_grant_received(world, lp,
            role.replica_world_entity(world, row_handle), rt.local_player_dead());
}
} // namespace opennova::inmatch
