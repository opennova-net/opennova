#include <runtime/world/vehicle_system.h>

#include <runtime/world/world.h>
#include <runtime/devtools/tick_profile.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/local_player.h> // the local look the request snaps
#include <runtime/world/mount_controls.h> // emplaced_gun_frame_heading, emplaced_word_bam
#include <runtime/world/mounted_pose.h> // seat_bone_pose_on_carrier_matrix
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_part_anim.h> // rotor_spawn_full, the joiner's aircraft init
#include <runtime/world/weapon_fire_gate.h> // entity_in_friendly_farp
#include <runtime/world/weapon_inventory.h> // weapon_slot_initial_zoom, the seat's pool reads

#include <runtime/world/ai.h> // AiSystem / AiEntity / ai_apply_command — the AI-change command target
#include <runtime/renderer/render_slot_shadow.h> // slot_seat_bits / slot_seat_walk: the slot seat facts
#include <base/io/bam.h>

#include "vehicle_motor_detail.h"

namespace opennova::world {

static void emit_vehicle_control(World &world, const char *kind, uint16_t target_net_id,
                                 int32_t target_bms_id, uint32_t target_spawn_origin,
                                 uint16_t target_wire_handle) {
    Effect effect;
    effect.kind = kind;
    effect.a = static_cast<int32_t>(target_net_id);
    effect.b = target_bms_id;
    effect.c = static_cast<int32_t>(target_spawn_origin);
    effect.d = static_cast<int32_t>(target_wire_handle);
    world.out.effects.push(std::move(effect));
}

// The claim edges also run the item's slot-A leg: the occupancy spawner's start and the
// claimant detach's +0x1CC release (ItemEmitterSystem::control_started / control_stopped carry
// the witnesses).
void VehicleSystem::emit_control_started(const Entity &vehicle) {
    World &world = world_;
    emit_vehicle_control(world, "vehicle_control_started", vehicle.net_id, vehicle.bms_id,
                         vehicle.spawn_origin, vehicle.handle.packed);
    world.item_emitters.control_started(world, vehicle);
}

void VehicleSystem::emit_control_stopped(const Entity &vehicle) {
    World &world = world_;
    world.vehicles.emit_control_stopped(vehicle.net_id, vehicle.bms_id, vehicle.spawn_origin,
                                 vehicle.handle.packed);
}

void VehicleSystem::emit_control_stopped(uint16_t target_net_id, int32_t target_bms_id, uint32_t target_spawn_origin, uint16_t target_wire_handle) {
    World &world = world_;
    emit_vehicle_control(world, "vehicle_control_stopped", target_net_id, target_bms_id,
                         target_spawn_origin, target_wire_handle);
    if (const Entity *vehicle = world.registry.get(EntityHandle{target_wire_handle}))
        world.item_emitters.control_stopped(world, *vehicle);
}

bool VehicleSystem::claim_primary_occupant(Entity &vehicle, EntityHandle occupant, SeatType seat) {
    World &world = world_;
    // [orig: Entity_AttachToVehicleSlot @0x4946d0] ctrlx(2)/drvrx(5) claim +368 when it is
    // empty or already theirs and the carrier's def has PlayerControl (0x40)
    // (@0x4947b3..0x4947d2 / @0x4948b9..0x4948d8). UseGun(3) claims through
    // Entity_AttachToUseGunSlot @0x546B80 (the test @0x546bb6..0x546bc6, the store
    // @0x546c6d): the authority's attach refused a held +368 before this
    // (attach_apply) and a client's forced attach stores over a claimant there, so
    // here it claims an empty one or keeps its own. This function's own type-3 leg
    // (@0x494944..0x49495e, empty only) is unreachable: the one caller sends UseGun seats to
    // Entity_AttachToUseGunSlot (@0x435c9f / @0x435caa). Sitex passengers never touch +368.
    const bool was_empty = !vehicle.primary_occupant.valid();
    switch (seat) {
        case SeatType::Controller:
        case SeatType::Driver:
            if (!was_empty && vehicle.primary_occupant != occupant) return false;
            if ((vehicle.item_attrib & kItemAttribPlayerControl) == 0) return false;
            break;
        case SeatType::Gunner:
            if (!was_empty) return vehicle.primary_occupant == occupant;
            break;
        default:
            return false;
    }
    vehicle.primary_occupant = occupant;
    // The empty -> claimed edge is the retail engine-start edge (the per-tick spawner
    // fires once its latch sees +368 set) [orig: @0x48faad..0x48fb0c].
    if (was_empty) world.vehicles.emit_control_started(vehicle);
    return true;
}

bool VehicleSystem::release_primary_occupant(Entity &vehicle, EntityHandle occupant) {
    World &world = world_;
    // [orig: Entity_DetachFromVehicle @0x4355f0] the stop leg runs ONLY when the detaching
    // entity IS the claimant (@0x4356e9); anyone else leaving — including a second control
    // occupant — leaves the latch untouched.
    if (!vehicle.primary_occupant.valid() || vehicle.primary_occupant != occupant)
        return false;
    // A PlayerControl vehicle's claimant leaving cuts the running action of
    // its vehicle weapon slot short: a nonzero MountSlot counter drops to 7.
    // [orig: Def gate @0x4356D0, attrib 0x40 @0x4356EF..0x4356F4, +0x474
    //  counter @0x4356F6..0x4356FF; the slot is Entity_GetWeaponSlots
    //  @0x5460FA's vehicle MountSlot]
    if (vehicle.has_item_def && (vehicle.item_attrib & kItemAttribPlayerControl) != 0 &&
            vehicle.primary_weapon_slot.counter != 0)
        vehicle.primary_weapon_slot.counter = 7;
    // A weapon carrier without PlayerControl (an emplaced gun) cuts its own MountSlot
    // counter (+0x2B4, the word its 0x0C / 0x18 aiState byte carries) the same way
    // when its claimant leaves (D-NET-133).
    // [orig: Entity_DetachFromVehicle @0x4355F0 - the 0x40 test's else @0x4356F4 ->
    //  @0x435765, attrib 0x20 @0x435765..0x435767, the counter @0x435769..0x435772]
    else if (vehicle.has_item_def && (vehicle.item_attrib & kItemAttribPlayerControl) == 0 &&
            (vehicle.item_attrib & kItemAttribEweap) != 0 &&
            vehicle.primary_weapon_slot.counter != 0)
        vehicle.primary_weapon_slot.counter = 7;
    // The PlayerControl leg: the all-zero fold and the stop on the departing
    // occupant, then the +0x1CC smoke emitter release; the next mover tick
    // re-arms the smoke while the damage band still holds.
    // [orig: Entity_DetachFromVehicle @0x4356EF..0x435759]
    world.vehicles.play_claimant_detach_sound(vehicle, world.registry.get(occupant));
    if (const VehicleTraits *traits = world.vehicles.traits.get(vehicle.item_id);
            traits != nullptr && traits->player_control)
        detail::vehicle_smoke_effect(world, vehicle, /*release=*/true);
    vehicle.primary_occupant = EntityHandle{};
    // The local claimant's EquippedSlot takes the held-ready phase (0x40) once the word
    // clears, after the detach's own mount queued its switch back on that slot: the pump's
    // next transition there takes the held variant (the phase re-stamped 0x40 past the
    // entered handler), and the handler after it consumes the byte at the begin-active shim
    // (ACTIVE, the clip played again, no begin leg). The slot is whichever the walk pumps:
    // a borrowed gun's or carrier's, else the personal one (a Driver's, an unarmed
    // Controller's). With no EquippedSlot retail's write would fault; ours writes nothing.
    // [orig: Entity_DetachFromVehicle @0x4355F0 - the word's clear @0x43577C,
    //  g_LocalPlayerEntity == entity @0x43578C, EquippedSlot->phase = 64 @0x435794;
    //  WeaponAction_ProcessFrame @0x540E60 (the held variant @0x541453..0x54148E);
    //  ActionSlot_BeginActivePhase @0x53F830 (@0x53F88B..0x53F8B0)]
    if (occupant == world.cached.local_player && world.local_player_state != nullptr) {
        LocalPlayerWeapon &w = world.local_player_state->weapon;
        if (w.active) active_local_weapon_slot(world, w)->phase = weapon_phase::kHeld;
    }
    world.vehicles.emit_control_stopped(vehicle);
    return true;
}

bool VehicleSystem::init_weapon_slot_from_entity_def(Entity &entity) {
    World &world = world_;
    // The slot's 0x64 bytes cleared, its owner (+0x24) with them, whatever the name.
    // [orig: WeaponSlot_InitFromEntityDef @0x5466C0 - the memset @0x5466D3]
    entity.primary_weapon_slot = WeaponSlotState{};
    entity.primary_weapon_slot_adm = kAdmSlotNone;
    entity.primary_weapon_owner = EntityHandle{};
    // The carrier word is the entity, whatever the name binds [orig: @0x546749 /
    //  @0x546756, both legs].
    entity.primary_weapon_slot.carrier = entity.handle.packed;
    // An empty name binds nothing, nor does one the table does not hold ("null" among them).
    // [orig: the name test @0x5466E1; AdmDef_FindIndexByName @0x53FD80 (the call @0x5466E8);
    //  AdmDef_GetEntryByIndex @0x53FC80 (the call @0x5466F8), the entry test @0x546704]
    if (entity.primary_weapon.empty()) return false;
    const int weapon_index = world.tables.weapons.index_of(entity.primary_weapon.c_str());
    if (weapon_index < 0 || weapon_index > 0xFF) return false;
    const uint8_t adm = static_cast<uint8_t>(weapon_index);
    const WeaponTableEntry *weapon = world.tables.weapons.by_index(adm);
    if (weapon == nullptr) return false;
    entity.primary_weapon_slot_adm = adm;
    // Scope zero seed and elevation under flags & 3 [orig:
    //  WeaponSlot_InitFromDef @0x53EE70 (the call @0x54670A): +0x60 = def+0xA0 / def+0x9C
    //  @0x53EEB2..0x53EECB; +0x04 = def[+0x3B0 + 4*step] under def+8 & 3
    //  @0x53EEDA..0x53EEEC; +0x08 = atan2(def+0x8C, step * def+0x9C << 16
    //  floored at 100 m) outside that gate, never negated here
    //  @0x53EF4F..0x53EF8B].
    entity.primary_weapon_slot.scope_zero = weapon_scope_zero_initial(weapon->action_fsm.scope_zero);
    if ((weapon->flags & 3) != 0)
        entity.primary_weapon_slot.zero_pitch = weapon_scope_zero_pitch(
            weapon->action_fsm.scope_zero, entity.primary_weapon_slot.scope_zero);
    entity.primary_weapon_slot.zero_yaw = weapon_scope_zero_yaw(
        weapon->action_fsm.scope_zero, entity.primary_weapon_slot.scope_zero);
    // The zoom seed (MountSlot+0xC). The emplacement's own slot has no
    // owner entity, so the class-6 sniper lock never applies here: a
    // tank cannon starts at its floor, a JOTAC roof sight at 1x
    // [orig: WeaponSlot_InitFromEntityDef passes entityPtr 0 @0x546706;
    //  WeaponSlot_InitFromDef skips the lock @0x53EEF7, seed
    //  @0x53EF2D..0x53EF44].
    entity.primary_weapon_slot.scope_zoom = weapon_slot_initial_zoom(weapon->scope_max_mag,
            weapon->scope_initial_mag, weapon->scope_min_mag, /*sniper_lock=*/false);
    // The MountSlot's 16-bit words: the clip is the def's clipsize, the
    // reserve its raw startrounds when either word is 0xFFFF, else the
    // unclamped 16-bit difference. [orig: WeaponSlot_InitFromEntityDef
    //  +0x10 = def+0x58 @0x54670F..0x546713; +0x12 = def+0x5C @0x546725;
    //  the 0xFFFF tests @0x546721 / @0x54672B, the difference
    //  @0x546731..0x546733]
    const uint16_t clip_word = static_cast<uint16_t>(weapon->clipsize);
    const uint16_t start_word = static_cast<uint16_t>(weapon->startrounds);
    uint16_t reserve_word = start_word;
    if (clip_word != 0xFFFFu && start_word != 0xFFFFu)
        reserve_word = static_cast<uint16_t>(start_word - clip_word);
    entity.primary_weapon_slot.clip = static_cast<int16_t>(clip_word);
    entity.primary_weapon_slot.reserve = static_cast<int16_t>(reserve_word);
    // Its +0x160 store of the def's round word (@0x54673B) has no reader on a gun or an
    // aircraft (the fire descriptors read their shooter's, a person's), so ours keeps none.
    return true;
}

void VehicleSystem::split_weapon_slot_ammo(Entity &entity, int32_t total) {
    World &world = world_;
    // [orig: WeaponSlot_SplitAmmoIntoClipAndReserve @0x546370 - the slot's def @0x546379, the
    //  -1 tests on startrounds (+0x5C) and clipsize (+0x58) @0x54637F..0x54638B, the total's
    //  cap @0x54638F..0x546391, the clip word and its cap @0x546393..0x5463A5, the reserve
    //  @0x5463B3..0x5463B5; its +0x474 twin WeaponSlot_SplitVehicleAmmoIntoClipAndReserve
    //  @0x546310 runs the same body]
    const WeaponTableEntry *weapon = world.tables.weapons.by_index(entity.primary_weapon_slot_adm);
    if (weapon == nullptr) return;
    const int32_t start = weapon->startrounds;
    if (start == -1 || weapon->clipsize == -1) return;
    const int32_t rounds = total > start ? start : total;
    const int16_t clip_size = weapon->clipsize;
    int16_t clip = clip_size;
    if (clip_size > rounds) clip = static_cast<int16_t>(rounds);
    entity.primary_weapon_slot.clip = clip;
    entity.primary_weapon_slot.reserve = static_cast<int16_t>(rounds - clip);
}

void VehicleSystem::run_weapon_slot_class_init(Entity &entity) {
    World &world = world_;
    if (!entity.has_item_def) return;
    const ItemDeathTraits *row = world.tables.item_death_traits.get(entity.item_id);
    if (row == nullptr) return;
    if (row->death_class == ItemDeathClass::kEwep) {
        // The ewep row's init binds the slot first, then splits the bound weapon's
        // startrounds once the entity's model is loaded.
        // [orig: Entity_InitBoneReferences @0x441470 - WeaponSlot_InitFromEntityDef on
        //  entity+0x2B4 (the call @0x441482), the model test @0x44148D and the slot Def test
        //  @0x441495, WeaponSlot_SplitAmmoIntoClipAndReserve with def+0x5C (the call
        //  @0x4415D8)]
        const bool bound = init_weapon_slot_from_entity_def(entity);
        if (bound && entity.has_graphic_model)
            split_weapon_slot_ammo(entity,
                    world.tables.weapons.by_index(entity.primary_weapon_slot_adm)->startrounds);
        // The init's tail: a row on a ground entity with a def (an addeweap child on its
        // carrier) takes its anchor index from that def's slot table (emplacement_bone, the
        // spawn's stamp), then runs its class update once, whose matrix leg composes it on
        // the carrier, behind the slot Def just bound. This is where a child first leaves
        // its spawn record's words.
        // [orig: Entity_InitBoneReferences @0x441470 - groundEntity and its def
        //  @0x4415E1..0x4415EC, the slot byte @0x4415F1..0x4415FF,
        //  Entity_UpdateTransformAndTurret called @0x44164B]
        const Entity *ground = world.registry.get(entity.ground_target);
        if (ground != nullptr && ground->has_item_def)
            tick_emplaced_weapon_class_update(world, entity);
        return;
    }
    if (!row->air_class_init) return;
    if ((entity.item_attrib & kItemAttribEweap) != 0) {
        // The CHel / cpln init's tail, under the def's EWeap bit: the +0x474 slot bound, then
        // its startrounds split with no model test.
        // [orig: Entity_InitHelicopterAIFromDef @0x4683C0 -> Entity_InitAircraftWeaponBones (the
        //  call @0x4686A2, which the no-AI-slot leg's jz @0x468493 and the brain leg's jmp
        //  @0x46869A both reach); Entity_InitAircraftWeaponBones @0x490160 - the
        //  attrib test @0x490169, WeaponSlot_InitFromEntityDef on entity+0x474 (the call
        //  @0x49017B), the slot Def test @0x490180..0x49018B,
        //  WeaponSlot_SplitVehicleAmmoIntoClipAndReserve with def+0x5C (the call @0x490192)]
        if (init_weapon_slot_from_entity_def(entity))
            split_weapon_slot_ammo(entity,
                    world.tables.weapons.by_index(entity.primary_weapon_slot_adm)->startrounds);
    }
    // Then, on a joiner alone (an in-session peer that is not the authority), the row's +0xB0,
    // the 0x0D's copy of the host brain's latch byte, comes back: the brain takes its low two
    // bits (the claimant-engine and movement-direction latches; the byte store clears the
    // other six), and with its bit 7, set while the host's rotor turned, the rotor starts at
    // the title's full rate and at full speed. The init hands over the brain on its brain leg
    // alone; its no-AI-slot leg hands over a stack word it never wrote, which ours, with no
    // brain to hold the bits, writes nowhere. A 0x18 row's +0xB0 is 0, and a respawn drops
    // the rotor words after this (respawn).
    // [orig: Entity_InitAircraftWeaponBones @0x490160 - the authority test @0x4902BB, the
    //  session test @0x4902C4, the brain test @0x4902CD..0x4902D3, +0x318 = +0xB0 & 3
    //  @0x4902D5..0x4902DE, the bit-7 test @0x4902E4, +0x468 = 0x2D82D @0x4902ED, +0x460 =
    //  0x0CCCCCC0 @0x4902F7; Entity_InitHelicopterAIFromDef @0x4683C0 - the brain pushed
    //  @0x4686A0, the unwritten word @0x46869C]
    // [orig: sub_490170 @0x490170 - the same tail @0x4902CB..0x490307, +0x468 = 0x5B05B
    //  @0x4902FD (dfx2.exe)]
    if (!world.rules.mp_session || world.rules.projectile_authority) return;
    Entity::VehicleMotorState &m = entity.veh;
    if (world.ai.for_handle(entity.handle) != nullptr) {
        const uint32_t bits = static_cast<uint32_t>(m.spawn_sound_latch) & 3u;
        m.engine_sound_latched = (bits & 1u) != 0;
        m.reverse_sound_latched = (bits & 2u) != 0;
        m.light_sound_latched = m.flare_latched = false;
        m.skid_sound_latched = false;
        m.collision_sound_latched = false;
        m.pivot_sound_latched = false;
        m.tumble_hard_latched = m.tumble_med_latched = false;
    }
    if ((m.spawn_sound_latch & 0x80) != 0)
        rotor_spawn_full(m.part_spin, world.rules.title().part_spin);
}

void VehicleSystem::run_weapon_slot_class_inits() {
    World &world = world_;
    // [orig: Entity_InitAllFromModels @0x40E460 - the def+0x148 calls over pool 2 @0x40E55B,
    //  pool 3 @0x40E59A, pool 1 @0x40E5D3 and pool 0 @0x40E60B, each pool to its used count
    //  (the loads @0x40E52F, @0x40E568, @0x40E5AC, @0x40E5E0)]
    for (const int pool : { 2, 3, 1, 0 }) {
        for (size_t i = 0; i < world.registry.pool_used(pool); ++i) {
            if (Entity *e = world.registry.get(EntityHandle::make(pool, uint16_t(i))))
                run_weapon_slot_class_init(*e);
        }
    }
}

void VehicleSystem::bind_use_gun_slot(Entity &occupant, Entity &vehicle) {
    // The borrow writes no AdmDef byte (Entity::use_gun_slot_swapped) and nothing in the
    // slot but its owner: the slot is the one the gun's class init bound.
    // [orig: Entity_AttachToUseGunSlot @0x546B80 (the owner store @0x546C73);
    //  Entity_AttachToVehicleSlot @0x4946D0 (the owner stores @0x494840 / @0x494883 / @0x494898)]
    occupant.use_gun_slot_swapped = true;
    vehicle.primary_weapon_owner = occupant.handle;
}

void VehicleSystem::release_despawned_slot_owner(EntityHandle gone) {
    world_.registry.for_each([&](const Entity &row) {
        if (row.primary_weapon_owner == gone)
            world_.registry.get(row.handle)->primary_weapon_owner = EntityHandle{};
    });
}

const WeaponSlotState *VehicleSystem::resolve_mounted_ammo_slot(const Entity &mount) const {
    const World &world = world_;
    // Retail proves the item definition and the EWeap attrib before resolving
    // ANY slot: Entity_GetWeaponSlots bails to NULL and the phase-8 writer emits
    // the zero-word form when either is missing [orig: Entity_GetWeaponSlots @0x5460E0
    // (!itemDef -> 0; !(attrib & 0x20) -> 0); NetPacket_WritePlayerState @0x4FF6B0 (the gate
    // @0x4FFE0B)]. A tool world that installs authored seat specs before the
    // item database therefore resolves no slot until traits arrive.
    if (!mount.has_item_def ||
        (mount.item_attrib & kItemAttribEweap) == 0u)
        return nullptr;
    // The unredirected route is the entity's already-bound embedded MountSlot.
    // Only following the mutable route bit to another entity needs the
    // cross-entity relationship proof below.
    if (!mount.primary_weapon_slot.redirect_to_parent_slot)
        return &mount.primary_weapon_slot;
    // A PlayerControl row (def+0x54 attrib 0x40) keeps its own vehicle slot whatever the route
    // bit says; the phase-8 writer's def+92 type == 1 test is that writer's alone
    // (resolve_phase8_ammo_slot).
    // [orig: Entity_GetWeaponSlots @0x5460E0 - `test al, 40h` @0x5460F6, +0x474 @0x5460FA]
    if ((mount.item_attrib & kItemAttribPlayerControl) != 0u)
        return &mount.primary_weapon_slot;

    // The route bit sends the entity straight to its groundEntity's vehicle slot
    // (+0x28, then +0x474): no def, type, attrib or addeweap identity of that
    // entity is read, whatever entity the ground link names when the slot is
    // asked for. The gates live where the route legs set the bit
    // (designated_g_route_carrier). With no groundEntity retail's helper returns
    // the wild 0x474 its next read faults on; ours resolves no slot there.
    // [orig: Entity_GetWeaponSlots @0x5460E0 - the bit test @0x546101, the
    //  groundEntity read @0x546110, +0x474 @0x546113]
    const Entity *ground = world.registry.get(mount.ground_target);
    return ground != nullptr ? &ground->primary_weapon_slot : nullptr;
}

WeaponSlotState *VehicleSystem::resolve_mounted_ammo_slot(Entity &mount) {
    World &world = world_;
    (void)world;
    return const_cast<WeaponSlotState *>(
            static_cast<const VehicleSystem *>(this)->resolve_mounted_ammo_slot(
                    static_cast<const Entity &>(mount)));
}

const WeaponSlotState *VehicleSystem::resolve_phase8_ammo_slot(const Entity &mount) const {
    // The writer's and the reader's own legs, not the shared helper's: the def and EWeap gate;
    // a type-1 row (no mountable gun) its own vehicle slot whatever the route bit; a mountable
    // gun its own slot with the route bit clear, else its groundEntity's behind the route's four
    // checks (designated_g_route_carrier). Anything else writes the zero words.
    // [orig: NetPacket_WritePlayerState @0x4FF6B0 - the gate @0x4FFE05 / @0x4FFE0B,
    //  Entity_IsMountableGun @0x4FFE0F, the route bit @0x4FFE18 (own @0x4FFE1F), the ground
    //  checks @0x4FFE27..0x4FFE3F, the type-1 leg @0x4FFE5F..0x4FFE65, the zero words
    //  @0x4FFE7F; NapiNPClientMsg_0x00A @0x42FEC0 - @0x4304E5..0x43052E]
    if (!mount.has_item_def || (mount.item_attrib & kItemAttribEweap) == 0u) return nullptr;
    if (mount.item_type == 1u || !mount.primary_weapon_slot.redirect_to_parent_slot)
        return &mount.primary_weapon_slot;
    const Entity *ground = world_.registry.get(mount.ground_target);
    return designated_g_route_carrier(ground) ? &ground->primary_weapon_slot : nullptr;
}

WeaponSlotState *VehicleSystem::resolve_phase8_ammo_slot(Entity &mount) {
    return const_cast<WeaponSlotState *>(
            static_cast<const VehicleSystem *>(this)->resolve_phase8_ammo_slot(
                    static_cast<const Entity &>(mount)));
}

bool VehicleSystem::weapon_slot_full(const Entity &entity) const {
    const World &world = world_;
    if (!entity.has_item_def) return false;
    const WeaponTableEntry *weapon = world.tables.weapons.by_index(entity.primary_weapon_slot_adm);
    if (weapon == nullptr) return false;
    const WeaponSlotState &slot = entity.primary_weapon_slot;
    // The pool: ammo class 1 reads the entity's +0x120 word, an EWeap entity its slot's
    // reserve; another reads the local pools in the game, which no FARP walk reaches with a
    // stock def (0 here) [orig: Entity_GetScoreValueBySlotType @0x540742..0x540763,
    //  @0x540775..0x54078E, @0x5407A1..0x5407CF].
    const bool eweap = (entity.item_attrib & kItemAttribEweap) != 0u;
    const int32_t own_pool = weapon->ammo_class_id == 1
            ? static_cast<int32_t>(entity.mana)
            : eweap ? static_cast<int32_t>(static_cast<int16_t>(slot.reserve)) : 0;
    const WeaponAmmoSeat seat = entity_ammo_seat(world, entity);
    const int32_t pool = weapon_seat_pool(seat, weapon->ammo_class_id, own_pool);
    const int32_t loaded = weapon->ammo_bucket != 0
            ? weapon_seat_bucket(seat, slot.clip)
            : static_cast<int32_t>(static_cast<int16_t>(slot.clip));
    return pool + loaded >= weapon->startrounds;
}

void VehicleSystem::farp_service_second() {
    World &world = world_;
    const int32_t rest = 62 * world.rules.farp_reuse_seconds;
    std::vector<EntityHandle> vehicles;
    world.registry.for_each_in_pool(1, [&](const Entity &e) {
        if (e.item_type_index != 0 && e.has_item_def &&
                ((e.flags | e.engine_flags) & kEntityFlagCarried) == 0 &&
                e.item_type == 1 &&
                static_cast<int32_t>(world.logic_tick - e.farp_stamp) >= rest)
            vehicles.push_back(e.handle);
    });
    for (const EntityHandle handle : vehicles) {
        Entity *vehicle = world.registry.get(handle);
        if (vehicle == nullptr) continue;
        if (!entity_in_friendly_farp(world, *vehicle)) {
            if (vehicle->farp_steps > 5) vehicle->farp_stamp = world.logic_tick;
            vehicle->farp_steps = 0;
            continue;
        }
        ++vehicle->farp_steps;
        if (vehicle->farp_steps > 120) {
            const int16_t health = static_cast<int16_t>(vehicle->critical_hp - 1);
            vehicle->health = health < 0 ? 0 : health;
        }
        bool full = true;
        // The top-up: the weapon's farpinfo rounds on every interval-th step, the slot's
        // words re-split from clip + reserve + rounds (Entity_AddMountSlotAmmo @0x546230 for
        // the vehicle's slot, Entity_AddEWeapSlotAmmo @0x5462A0 for a gun's, each the
        // split's body) [orig: @0x51190B..0x511934, @0x51197A..0x5119A3].
        const auto top_up = [&](Entity &holder) {
            const WeaponTableEntry *weapon =
                    world.tables.weapons.by_index(holder.primary_weapon_slot_adm);
            if (weapon == nullptr || weapon->farp_rounds == 0 || weapon->farp_interval == 0)
                return;
            if (vehicle->farp_steps % weapon->farp_interval != 0) return;
            split_weapon_slot_ammo(holder,
                    static_cast<int16_t>(holder.primary_weapon_slot.clip) +
                    static_cast<int16_t>(holder.primary_weapon_slot.reserve) +
                    weapon->farp_rounds);
        };
        if ((vehicle->item_attrib & kItemAttribEweap) != 0u) {
            full = weapon_slot_full(*vehicle);
            top_up(*vehicle);
        }
        if (vehicle->ref_num != 0) {
            std::vector<EntityHandle> guns;
            world.registry.for_each([&](const Entity &other) {
                if (other.ref_group_member && other.ref_num == vehicle->ref_num)
                    guns.push_back(other.handle);
            });
            for (const EntityHandle gun_handle : guns) {
                Entity *gun = world.registry.get(gun_handle);
                if (gun == nullptr) continue;
                if (!weapon_slot_full(*gun)) full = false;
                // The vehicle's own row in its group tops up the game's +0x2B4 slot, which
                // a vehicle fires from none of; ours has the one slot, topped up above.
                if (gun != vehicle) top_up(*gun);
            }
        }
        vehicle->farp_rearmed = full;
    }
}

uint8_t VehicleSystem::farp_reload_byte(const Entity &rider) const {
    const World &world = world_;
    if (!rider.mounted || !is_vehicle_control_seat(rider.mount_type)) return 0;
    const Entity *vehicle = world.registry.get(rider.mount_target);
    if (vehicle == nullptr) return 0;
    const int32_t left = world.rules.farp_reuse_seconds -
            static_cast<int32_t>(world.logic_tick - vehicle->farp_stamp) / 62;
    if (left > 0) return static_cast<uint8_t>(left > 0xFE ? 0xFE : left);
    if (vehicle->farp_rearmed && (vehicle->carry_flags & 0x40u) != 0u) return 0xFF;
    return 0;
}

bool VehicleSystem::reload_weapon_slot(Entity &mount) {
    World &world = world_;
    const WeaponTableEntry *own_def = world.tables.weapons.by_index(mount.primary_weapon_slot_adm);
    WeaponSlotState *routed = resolve_mounted_ammo_slot(mount);
    if (own_def == nullptr || routed == nullptr) return false;
    // The routed slot's def is the slot's own: the carrier's when the route bit
    // sends the child to it [orig: slot+0x20 read @0x541797 / @0x5417EF].
    const WeaponTableEntry *routed_def = own_def;
    if (routed != &mount.primary_weapon_slot) {
        const Entity *carrier = world.registry.get(mount.ground_target);
        if (carrier == nullptr || routed != &carrier->primary_weapon_slot) return false;
        routed_def = world.tables.weapons.by_index(carrier->primary_weapon_slot_adm);
        if (routed_def == nullptr) return false;
    }
    weapon_mount_slot_reload(*routed_def, *routed, *own_def, mount.primary_weapon_slot);
    return true;
}

void vehicle_release_equipped_slot(Entity &occupant, bool is_local) {
    // The vehicle's MountSlot keeps its owner (+0x24): the detach writes the counter and the
    // EquippedSlot words, never the owner, so the gun's own pump runs its handlers for the
    // gunner who left it (VehicleSystem::release_despawned_slot_owner drops it only with the
    // gunner's entity). [orig: Entity_DetachFromVehicle @0x4355F0 - the slot's stores
    //  @0x4356FF / @0x435772 alone; WeaponAction_ProcessFrame @0x540E60 - the handler calls
    //  with slot->owner @0x5413FF / @0x54142A / @0x541482 / @0x5414A2]
    const bool is_player =
            ((occupant.flags | occupant.engine_flags) & 0x100u) != 0;
    // No AdmDef byte is restored: the detach writes it only below, for a
    // non-player (NEW-L7-3). [orig: Entity_DetachFromVehicle @0x4355F0]
    occupant.use_gun_slot_swapped = false;
    // Any other entity leaving a seat 2 or 3 points its EquippedSlot at +0x308,
    // whether that seat borrowed a slot or not: after an unarmed ctrlx ride it is
    // whatever +0x308 last held, null when nothing saved one (FOUND-4). The local
    // player mounts it instead (local_weapon_detach_mount).
    // [orig: Entity_DetachFromVehicle @0x435671..0x43567F, the store
    //  @0x435681..0x435687]
    if (!is_local && (occupant.mount_type == SeatType::Gunner ||
                      occupant.mount_type == SeatType::Controller))
        occupant.equipped_slot = occupant.selected_slot;
    // Then any non-player leaving its parent, whatever the seat, drops its
    // EquippedSlot and zeroes its AdmDef byte and its ammo-def word.
    // [orig: Entity_DetachFromVehicle Flags 0x100 test @0x43568D, EquippedSlot
    //  @0x435696, the byte @0x43569C, +0x160 @0x4356A3 (edx 0 from @0x435667)]
    if (!is_player) {
        occupant.equipped_slot = {};
        occupant.equipped_adm_index = 0;
        occupant.ammo_def_word = 0;
    }
}

SlotSeatFacts slot_seat_facts(bool player_class, bool pool0, SeatType seat,
        bool parent_ref_num, EntityHandle parent, EntityHandle parent_ground) {
    // The slot record's flag bit 1 (Flags 0x100 >> 7, or a pool-0 entity)
    // [orig: RenderSlot_UpdateEntityLight @0x5d6dbf..0x5d6ddf].
    SlotSeatFacts out;
    out.bits = renderer::slot_seat_bits(player_class || pool0, pool0,
            static_cast<int>(seat), parent_ref_num);
    switch (renderer::slot_seat_walk(out.bits)) {
        case renderer::SlotSeatWalk::kParent: out.walk = parent; break;
        case renderer::SlotSeatWalk::kParentGround: out.walk = parent_ground; break;
        case renderer::SlotSeatWalk::kNone: break;
    }
    return out;
}

SlotSeatFacts slot_seat_facts(const World &world, const Entity &entity) {
    // parentEntity (+0x16C) and its refNum (+0x215) and groundEntity (+0x28)
    // [orig: RenderSlot_SortAndAssign @0x5d65e1..0x5d65eb;
    //  RenderSlot_RenderEntityAndChildren @0x5d794c..0x5d7955].
    const Entity *parent = entity.mounted ? world.registry.get(entity.mount_target) : nullptr;
    const Entity *parent_ground =
            parent != nullptr ? world.registry.get(parent->ground_target) : nullptr;
    return slot_seat_facts(((entity.flags | entity.engine_flags) & kEntityFlagPlayer) != 0,
            entity.handle.pool() == 0, entity.mounted ? entity.mount_type : SeatType::None,
            parent != nullptr && parent->ref_num != 0,
            parent != nullptr ? parent->handle : EntityHandle{},
            parent_ground != nullptr ? parent_ground->handle : EntityHandle{});
}

EntityPosition entity_local_point_world(const Entity &vehicle, const Vec3 &local) {
    // Seat points use the carrier's live BAM frame, as collision/render do;
    // the mission-degree mirrors discard sub-degree flight motion. An unmoved
    // carrier's frame is its placement angles in the spawn form.
    // [orig: Entity_GetBoneTransformAndOrientation @0x4b0c50, point @0x4b0d42;
    //  Math_BuildFixedPointMatrixFromEulerAngles @0x613f40; Entity_SpawnFromBMSRecord
    //  @0x40EB42..0x40EBA6]
    const Vec3 &L = local;
    const int32_t heading = vehicle.veh.yaw_seeded ? vehicle.veh.yaw_bam
            : vehicle.heading;
    const int32_t origin[3] = {0, 0, 0};
    const CollisionMatrix m = collision_matrix_from_euler(
            heading,
            vehicle.veh.yaw_seeded ? vehicle.veh.air_pitch_bam
                    : vehicle.pitch_bam,
            vehicle.veh.yaw_seeded ? vehicle.veh.air_roll_bam
                    : vehicle.roll_bam, origin);
    // seat_local is pre-swizzled ((-y, x, z) over the raw authored ints — a
    // baked-in Rz(90)), while the collision euler matrix with heading
    // bam(90 - yaw) expects RAW model coordinates: un-swizzle first.
    const int32_t lf[3] = {static_cast<int32_t>(L.y * 65536.0f),
                           static_cast<int32_t>(-L.x * 65536.0f),
                           static_cast<int32_t>(L.z * 65536.0f)};
    int32_t wf[3];
    m.rotate_point(lf, wf);
    // The rotated point onto the carrier's words, a word add (D-NET-507)
    // [orig: Math_FixedPointTransformPoint22 @0x4b0d42's translation].
    EntityPosition p;
    p.set_q16({io::bam_add(vehicle.position.x.q16, wf[0]),
               io::bam_add(vehicle.position.y.q16, wf[1]),
               io::bam_add(vehicle.position.z.q16, wf[2])});
    return p;
}

namespace {

// A vehicle's model takes BoneCallback_Simple, and so does a one-object collision
// model on a def without the EWeap bit (the stock chair, Chair01's sitex17); the
// callback hands every bone slot the carrier's own matrix, so its sitex / ctrlx /
// drvrx bone is the userpoint through that matrix (seat_bone_pose_on_carrier_matrix);
// a UseGun bone attaches through the bone attach instead.
// [orig: EntityDef_LoadModelsAndCallbacks @0x439F50 (def+0x5C == 1 @0x43A05A, else the
//  collision block's +0x68 == 1 @0x43A060..0x43A066 and def+0x54 & 0x20 clear
//  @0x43A06B -> BoneCallback_Simple @0x43A072); BoneCallback_Simple @0x4E2600]
bool seat_bone_is_carrier_matrix(const Entity &carrier, const Seat &seat) {
    const bool simple = carrier.item_type == 1 ||
            (carrier.graphic_model_collision_objects == 1 &&
                    (carrier.item_attrib & kItemAttribEweap) == 0u);
    return simple && !seat.attachment_frame && seat.type != SeatType::Gunner;
}

} // namespace

void VehicleSystem::presnap_attach_heading(Entity &occupant, const Entity &vehicle, const Seat &seat) {
    World &world = world_;
    // A UseGun requester faces along the gun: the carrier's own Yaw less its
    // stored gun yaw word (+0x322) shifted up. Every other seat takes the seat
    // bone's yaw: on a vehicle the fpatan of the userpoint's direction through the
    // carrier's matrix; on any other carrier the carrier frame turned by the seat
    // offset, as pose_mounted_occupant composes it.
    // [orig: Entity_RequestVehicleAttach @0x43655F..0x43656E (UseGun),
    //  @0x4365BF..0x4365C3 (the Entity_GetBoneTransformAndOrientation yaw @0x436540)]
    const int32_t carrier = emplaced_gun_frame_heading(vehicle);
    int32_t seat_heading;
    if (seat.type == SeatType::Gunner && !seat.attachment_frame) {
        seat_heading = io::bam_sub(carrier, emplaced_word_bam(vehicle.emplaced_gun_yaw_word));
    } else if (seat_bone_is_carrier_matrix(vehicle, seat)) {
        MountedPose bone;
        seat_bone_pose_on_carrier_matrix(vehicle, seat, bone);
        seat_heading = bone.heading;
    } else {
        seat_heading = io::bam_sub(carrier, bam_from_degrees_wrapped(seat.yaw_offset));
    }
    occupant.heading = seat_heading;
    // The local player's look yaw takes the same value: the request runs outside
    // the tick, so the input-owned look carries it into the next one.
    // [orig: g_LocalPlayerLookYaw @0x436579 / @0x4365CE]
    if (occupant.handle == world.cached.local_player && world.local_player_state != nullptr)
        world.local_player_state->input.look_heading = seat_heading;
    AiEntity *body = world.ai.for_handle(occupant.handle);
    if (body == nullptr) return;

    body->heading = seat_heading;
    // Retail has one entity Yaw. OpenNova separates the local input-owned look
    // target from the render heading, so both must receive the same attach snap.
    if (body->inf.is_local_player)
        body->inf.target_heading = seat_heading;
}

MountedPose VehicleSystem::pose_mounted_occupant(Entity &occ, const Entity &vehicle, const Seat &seat) {
    World &world = world_;
    MountedPose live;
    // An addeweap child with no anchor takes its carrier's root pose only while
    // the carrier's matrix bit is clear: the update then builds the carrier's
    // matrix from its words and copies the root pose. With the bit set (every
    // mover's tail sets it each tick; a decoration, building or powerup carries it
    // from its model init) the child keeps its own pose. The child's bit is set
    // either way. [orig: Entity_UpdateTransformAndTurret @0x440CA0 (the gate
    // `test [edi+24h], 20000h` @0x4410EA..0x4410F1, the build @0x4410F7..0x441151,
    // the copy @0x441153..0x4411BC, the child's bit @0x4411C2); the setters
    // Entity_InitFromModel @0x40E0D4..0x40E105, Entity_UpdateVehiclePhysics
    // @0x48AF00 (the tail @0x48D43E..0x48D451)]
    if (seat.attachment_frame && seat.bone_index == 0 &&
            (vehicle.engine_flags & kEntityFlagMatrixBuilt) != 0) {
        occ.engine_flags |= kEntityFlagMatrixBuilt;
        live.position = occ.position;
        live.heading = occ.veh.yaw_seeded ? occ.veh.yaw_bam : occ.heading;
        live.pitch = occ.veh.yaw_seeded ? occ.veh.air_pitch_bam : occ.pitch_bam;
        live.roll = occ.veh.yaw_seeded ? occ.veh.air_roll_bam : occ.roll_bam;
        return live;
    }
    if (world.pose_provider == nullptr ||
        !world.pose_provider->resolve_mounted_pose(world, vehicle, seat, live)) {
        if (seat_bone_is_carrier_matrix(vehicle, seat)) {
            // A vehicle's seat bone: the userpoint through the carrier's Q22 matrix,
            // the fpatan yaw, the matrix's pitch and roll (D-NET-448, D-INF-35).
            // [orig: Entity_GetBoneTransformAndOrientation @0x4B0C50 (@0x4B0D42,
            //  @0x4B0D64..0x4B0D81, @0x4B0DA1..0x4B0DBB) from the carry @0x4B6510 /
            //  @0x4BED72]
            seat_bone_pose_on_carrier_matrix(vehicle, seat, live);
        } else {
        // The root/local fallback uses the same complete carrier frame as
        // collision. Only an unseeded mission row needs degree-to-BAM conversion.
        // [orig: Entity_GetBoneTransformAndOrientation @0x4b0c50 over @0x613f40]
        live.position = entity_local_point_world(vehicle, seat.seat_local);
        const int offset = !seat.attachment_frame && seat.type == SeatType::Gunner
                ? -seat.yaw_offset : seat.yaw_offset;
        live.heading = vehicle.veh.yaw_seeded
                ? io::bam_sub(vehicle.veh.yaw_bam, bam_from_degrees_wrapped(offset))
                : io::bam_sub(vehicle.heading, bam_from_degrees_wrapped(offset));
        live.pitch = vehicle.veh.yaw_seeded ? vehicle.veh.air_pitch_bam
                : vehicle.pitch_bam;
        live.roll = vehicle.veh.yaw_seeded ? vehicle.veh.air_roll_bam
                : vehicle.roll_bam;
        }
        // An addeweap child with no anchor copies its carrier's root pose (the
        // carrier's matrix bit clear): the position and the Yaw / Pitch / Roll words
        // as they are, no seat offset, then the carrier's savedLivePose words
        // (+0x80..+0x94) as they are, which the priority walk's displacement reads
        // (D-NET-507). [orig: Entity_UpdateTransformAndTurret @0x440CA0 - the six
        // words @0x441151..0x441177, savedLivePose @0x44117A..0x4411B6, the matrix
        // @0x4411A9..0x4411BC]
        if (seat.attachment_frame && seat.bone_index == 0) {
            live.position = vehicle.position;
            live.heading = vehicle.veh.yaw_seeded ? vehicle.veh.yaw_bam : vehicle.heading;
            std::copy_n(vehicle.saved_live_pos, 3, occ.saved_live_pos);
            occ.saved_live_yaw = vehicle.saved_live_yaw;
            occ.saved_live_pitch = vehicle.saved_live_pitch;
            occ.saved_live_roll = vehicle.saved_live_roll;
            occ.saved_live_valid = vehicle.saved_live_valid;
        }
    }
    // An anchored addeweap child's savedLivePose (+0x80..+0x94) takes its own Position and
    // Yaw / Pitch / Roll words as they stand, before the bone frame's euler replaces them:
    // after each compose it holds the previous compose's pose, which the priority walk's
    // displacement and the fire-origin compare read (D-NET-507). [orig:
    //  Entity_UpdateTransformAndTurret @0x440CA0 - Bone_BuildAttachmentMatrix @0x44109D,
    //  savedLivePose @0x4410A2..0x4410D7, Math_FixedPointMatrixToEulerAngles @0x4410DD]
    if (seat.attachment_frame && seat.bone_index != 0) stamp_saved_live_pose(occ);
    // The seat point's words are the occupant's (D-NET-507); a composed addeweap
    // frame's are the child's own.
    // [orig: Math_FixedPointMatrixToEulerAngles @0x613481..0x61348C]
    occ.position = live.position;
    // The Yaw word: an addeweap child takes the bone's (the whole attitude); an
    // organic occupant's stays its look, the seat carry writing the bone yaw into
    // the body frame only (bodyHeading and the legs; apply_resolved_mounted_seat_frame)
    // and the look legs owning +0x10 (a remote player's chase @0x4B4361, the
    // local input, the org1 look chase). A non-organic occupant keeps ours' seat
    // yaw: retail runs no seat mover for one, the seat bone's only readers being the
    // two organic movers, the attach request, the rope renderer and the suspension
    // bounce, and its only non-organic mount the WAC's. [orig:
    //  Entity_UpdateTransformAndTurret @0x4410DD; Entity_UpdateInfantryPlayerBody - the
    //  seat carry @0x4B6510..0x4B6575 (+0x8C / +0x2E8 / +0x2E4 @0x4B654E..0x4B655A, no
    //  +0x10), the remote chase @0x4B4361; Entity_UpdateInfantryAI - @0x4BED72..0x4BEDDA,
    //  the look chase @0x4BEF84; Entity_GetBoneTransformAndOrientation @0x4B0C50's other
    //  callers Entity_RequestVehicleAttach @0x4364A0 (@0x436540),
    //  Entity_RenderRopeTrailGeometry @0x456140 (@0x4561F6), Entity_UpdateSuspensionBounce
    //  @0x456710 (@0x4567F7); WacScript_TryMountEntityToVehicle @0x4F70F0]
    const AiEntity *organic = world.ai.for_handle(occ.handle);
    if (seat.attachment_frame || organic == nullptr || !organic->inf.active)
        occ.heading = live.heading;
    // The Roll word takes the seat bone's roll; an organic occupant's Pitch word
    // stays its look (the carry writes the bone pitch into the body frame only;
    // the look legs write the Pitch word); an addeweap child takes the whole
    // bone attitude (D-NET-923). [orig: Entity_UpdateInfantryPlayerBody
    //  - Roll @0x4B6572, +0x90 @0x4B6575; Entity_UpdateInfantryAI - Roll
    //  @0x4BEDD7, +0x90 @0x4BEDDA; Entity_AttachToBoneAndUpdateTransform
    //  @0x5463D0 - the bone Roll kept @0x546632, the Pitch word restored
    //  @0x546664; Entity_UpdateTransformAndTurret - the bone euler into
    //  +4..+0x18 @0x4410DD]
    if (seat.attachment_frame) occ.pitch_bam = live.pitch;
    occ.roll_bam = live.roll;
    if (seat.attachment_frame) {
        // An addeweap child carries the full bone attitude into its own model,
        // collision, and camera. Ordinary infantry retain independent LOOK.
        // [orig: Entity_UpdateTransformAndTurret @0x440ca0 ->
        //  Bone_BuildAttachmentMatrix @0x56c630]
        occ.veh.yaw_seeded = true;
        occ.veh.yaw_bam = live.heading;
        occ.veh.air_pitch_bam = live.pitch;
        occ.veh.air_roll_bam = live.roll;
        // The composed child's matrix is current [orig: @0x4411C2].
        occ.engine_flags |= kEntityFlagMatrixBuilt;
    }
    return live;
}

} // namespace opennova::world
