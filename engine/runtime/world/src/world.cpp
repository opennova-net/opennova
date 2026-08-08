#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "world/angle.h"
#include "world/collision.h"
#include "world/vehicle_attach.h"
#include "world/vehicle_sound.h"

#include "world/ai.h" // AiSystem / AiEntity / ai_apply_command — the AI-change command target

namespace opennova::world {

// Max distance (mission units) for mount_best's nearest-emplacement search — the proximity
// proxy for the occupant-model+144 vehicle link the original resolves through the entity
// hierarchy. A manned-gun soldier is placed on/next to its gun, so this is generous.
static constexpr double kMountRadius = 20.0;

static void emit_vehicle_control(World &world, const char *kind, uint16_t target_net_id,
                                 int32_t target_bms_id, uint32_t target_spawn_origin,
                                 uint16_t target_wire_handle) {
    Effect effect;
    effect.kind = kind;
    effect.a = static_cast<int32_t>(target_net_id);
    effect.b = target_bms_id;
    effect.c = static_cast<int32_t>(target_spawn_origin);
    effect.d = static_cast<int32_t>(target_wire_handle);
    world.effects.push(std::move(effect));
}

void emit_vehicle_control_started(World &world, const Entity &vehicle) {
    emit_vehicle_control(world, "vehicle_control_started", vehicle.net_id, vehicle.bms_id,
                         vehicle.spawn_origin, vehicle.handle.packed);
}

void emit_vehicle_control_stopped(World &world, const Entity &vehicle) {
    emit_vehicle_control_stopped(world, vehicle.net_id, vehicle.bms_id, vehicle.spawn_origin,
                                 vehicle.handle.packed);
}

void emit_vehicle_control_stopped(World &world, uint16_t target_net_id,
                                  int32_t target_bms_id, uint32_t target_spawn_origin,
                                  uint16_t target_wire_handle) {
    emit_vehicle_control(world, "vehicle_control_stopped", target_net_id, target_bms_id,
                         target_spawn_origin, target_wire_handle);
}

bool vehicle_claim_primary_occupant(World &world, Entity &vehicle, EntityHandle occupant,
                                    SeatType seat) {
    // [orig: Entity_AttachToVehicleSlot @0x4946d0] ctrlx(2)/drvrx(5) claim +368 when it is
    // empty or already theirs (@0x4947b3..0x4947d2 / @0x4948b9..0x4948d8); UseGun(3) claims
    // only when empty (@0x494944..0x49495e); sitex passengers never touch +368.
    const bool was_empty = !vehicle.primary_occupant.valid();
    switch (seat) {
        case SeatType::Controller:
        case SeatType::Driver:
            if (!was_empty && vehicle.primary_occupant != occupant) return false;
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
    if (was_empty) emit_vehicle_control_started(world, vehicle);
    return true;
}

bool vehicle_release_primary_occupant(World &world, Entity &vehicle, EntityHandle occupant) {
    // [orig: Entity_DetachFromVehicle @0x4355f0] the stop leg runs ONLY when the detaching
    // entity IS the claimant (@0x4356e9); anyone else leaving — including a second control
    // occupant — leaves the latch untouched.
    if (!vehicle.primary_occupant.valid() || vehicle.primary_occupant != occupant)
        return false;
    stop_ground_vehicle_sound(world, vehicle);
    vehicle.primary_occupant = EntityHandle{};
    emit_vehicle_control_stopped(world, vehicle);
    return true;
}

bool vehicle_prepare_weapon_slot(World &world, Entity &vehicle) {
    const int weapon_index = world.weapons.index_of(vehicle.primary_weapon.c_str());
    if (weapon_index < 0 || weapon_index > 0xFF) return false;
    const uint8_t adm = static_cast<uint8_t>(weapon_index);
    const WeaponTableEntry *weapon = world.weapons.by_index(adm);
    if (weapon == nullptr) return false;
    if (vehicle.primary_weapon_slot_adm != adm) {
        vehicle.primary_weapon_slot = WeaponSlotState{};
        vehicle.primary_weapon_slot_adm = adm;
        if (weapon->clipsize < 0) {
            vehicle.primary_weapon_slot.clip = -1;
        } else {
            vehicle.primary_weapon_slot.clip = weapon->clipsize;
            vehicle.primary_weapon_slot.reserve = std::max<int32_t>(
                    0, static_cast<int32_t>(weapon->startrounds) - weapon->clipsize);
        }
    }
    return true;
}

bool vehicle_bind_use_gun_slot(World &world, Entity &occupant, Entity &vehicle) {
    if (!occupant.use_gun_slot_swapped) {
        occupant.pre_use_gun_equipped_adm_index = occupant.equipped_adm_index;
        occupant.use_gun_slot_swapped = true;
    }
    vehicle.primary_weapon_owner = occupant.handle;
    if (!vehicle_prepare_weapon_slot(world, vehicle)) {
        occupant.equipped_adm_index = 0xFF;
        return false;
    }
    occupant.equipped_adm_index = vehicle.primary_weapon_slot_adm;
    return true;
}

const WeaponSlotState *resolve_mounted_ammo_slot(
        const World &world, const Entity &mount) {
    // Retail proves the item definition and the EWeap attrib before resolving
    // ANY slot: the shared helper bails to NULL and the phase-8 writer emits
    // the zero-word form when either is missing [orig: shared helper @0x5460E0
    // (!itemDef -> 0; !(attrib & 0x20) -> 0); writer gate @0x4FFE0B]. A tool
    // world that installs authored seat specs before the item database
    // therefore resolves no slot until traits arrive.
    if (!mount.has_item_def ||
        (mount.item_attrib & kItemAttribEweap) == 0u)
        return nullptr;
    // The unredirected route is the entity's already-bound embedded MountSlot.
    // Only following the mutable route bit to another entity needs the
    // cross-entity relationship proof below.
    if (!mount.primary_weapon_slot.redirect_to_parent_slot)
        return &mount.primary_weapon_slot;
    // Stand-in note: the shared helper routes vehicles via the def+84 attrib
    // bit 0x40 [orig: Entity_GetWeaponSlots @ 0x5460E0] while the phase-8
    // writer keys def+92 type==1 [orig: @ 0x4FFE3F]; the shipped corpus stamps
    // both together on every EWeap vehicle, so type==1 serves both sites.
    if (mount.item_type == 1u)
        return &mount.primary_weapon_slot;

    // Retail follows entity+0x28 (groundEntity), not the addeweap metadata
    // pointer. Promotion/materialization capture the same relationship's live
    // generation so packed-handle reuse cannot redirect into an unrelated row.
    if (!mount.ground_target.valid() ||
        mount.emplacement_parent != mount.ground_target ||
        mount.emplacement_parent_spawn_id == 0)
        return nullptr;
    const Entity *parent = world.registry.get(mount.ground_target);
    if (parent == nullptr ||
        parent->registry_spawn_id != mount.emplacement_parent_spawn_id ||
        !parent->has_item_def || parent->item_type != 1u ||
        (parent->item_attrib & kItemAttribEweap) == 0u)
        return nullptr;
    return &parent->primary_weapon_slot;
}

WeaponSlotState *resolve_mounted_ammo_slot(World &world, Entity &mount) {
    return const_cast<WeaponSlotState *>(resolve_mounted_ammo_slot(
            static_cast<const World &>(world),
            static_cast<const Entity &>(mount)));
}

void vehicle_release_use_gun_slot(Entity &occupant, Entity *vehicle) {
    if (vehicle != nullptr && vehicle->primary_weapon_owner == occupant.handle)
        vehicle->primary_weapon_owner = EntityHandle{};
    if (!occupant.use_gun_slot_swapped) return;
    const bool is_player =
            ((occupant.flags | occupant.engine_flags) & 0x100u) != 0;
    occupant.equipped_adm_index =
            is_player ? occupant.pre_use_gun_equipped_adm_index : 0xFF;
    occupant.pre_use_gun_equipped_adm_index = 0xFF;
    occupant.use_gun_slot_swapped = false;
}

bool vehicle_has_valid_control_occupant(const World &world, const Entity &vehicle) {
    for (const Seat &seat : vehicle.seats) {
        if (!is_vehicle_control_seat(seat.type) || !seat.occupant.valid()) continue;
        const Entity *occupant = world.registry.get(seat.occupant);
        if (occupant != nullptr && occupant->mounted &&
            occupant->mount_target == vehicle.handle &&
            is_vehicle_control_seat(occupant->mount_type)) {
            return true;
        }
    }
    return false;
}

bool seat_allowed_for_mode(SeatType type, SeatSelectionMode mode) {
    switch (mode) {
        case SeatSelectionMode::PassengerOnly:
            return type == SeatType::Passenger;
        case SeatSelectionMode::RejectController:
            return type != SeatType::Controller;
        case SeatSelectionMode::Any:
        default:
            return true;
    }
}

static int16_t mounted_pose_yaw(const Entity &vehicle, const Seat &seat) {
    if (seat.attachment_frame)
        return static_cast<int16_t>(vehicle.yaw + seat.yaw_offset);
    if (seat.type == SeatType::Gunner)
        return static_cast<int16_t>(vehicle.yaw - seat.yaw_offset);
    return static_cast<int16_t>(vehicle.yaw + seat.yaw_offset);
}

void presnap_vehicle_attach_heading(World &world, Entity &occupant,
                                    const Entity &vehicle, const Seat &seat) {
    const int16_t seat_yaw = mounted_pose_yaw(vehicle, seat);
    occupant.yaw = seat_yaw;
    if (world.ai == nullptr) return;
    AiEntity *body = world.ai->for_handle(occupant.handle);
    if (body == nullptr) return;

    const int32_t seat_heading =
            bam_heading_from_mission_yaw_deg(static_cast<double>(seat_yaw));
    body->heading = seat_heading;
    // Retail has one entity Yaw. OpenNova separates the local input-owned look
    // target from the render heading, so both must receive the same attach snap.
    if (body->inf.is_local_player)
        body->inf.target_heading = seat_heading;
}

void pose_mounted_occupant(World &world, Entity &occ, const Entity &vehicle,
                           const Seat &seat) {
    MountedPose live;
    if (world.mounted_pose_provider != nullptr &&
        world.mounted_pose_provider->resolve_mounted_pose(world, vehicle, seat, live)) {
        occ.position = live.position;
        occ.yaw = live.yaw;
        occ.pitch = live.pitch;
        occ.roll = live.roll;
        return;
    }
    // Rotate the seat-local offset by the entity orientation frame, then translate by the vehicle
    // origin. In our stored mission-yaw convention this is -vehicle.yaw; this matches the retail
    // seat bone path through Entity_GetBoneTransformAndOrientation @0x4b0c50.
    constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
    const double a = static_cast<double>(-vehicle.yaw) * kDeg2Rad;
    const double ca = std::cos(a), sa = std::sin(a);
    const Vec3 &L = seat.seat_local;
    occ.position.x = vehicle.position.x + static_cast<float>(L.x * ca - L.y * sa);
    occ.position.y = vehicle.position.y + static_cast<float>(L.x * sa + L.y * ca);
    occ.position.z = vehicle.position.z + L.z;
    occ.yaw = mounted_pose_yaw(vehicle, seat);
    occ.pitch = vehicle.pitch;
    occ.roll = vehicle.roll;
}

// Attached emplacement children are allocated breadth-first after their carrier,
// so pool/slot iteration is parent-before-child even for turret-on-vehicle chains.
// Reuse the mounted-pose provider: a resolved USRP bone follows live PANM; bone
// zero takes pose_mounted_occupant's parent-root/local fallback.
static void pose_emplacement_attachments(World &world) {
    // Parent ownership ends when the carrier dies, even though ordinary item
    // destruction keeps that carrier resident as a husk. Peel orphan chains
    // without mutating registry slots during traversal.
    for (int depth = 0; depth < 8; ++depth) {
        std::vector<EntityHandle> orphans;
        world.registry.for_each([&](const Entity &candidate) {
            if (!candidate.emplacement_parent.valid()) return;
            const Entity *parent =
                    world.registry.get(candidate.emplacement_parent);
            if (parent == nullptr ||
                parent->registry_spawn_id !=
                        candidate.emplacement_parent_spawn_id ||
                !parent->alive || parent->health <= 0)
                orphans.push_back(candidate.handle);
        });
        if (orphans.empty()) break;
        for (EntityHandle orphan : orphans) {
            std::vector<EntityHandle> occupants;
            world.registry.for_each([&](const Entity &candidate) {
                if (candidate.mounted && candidate.mount_target == orphan)
                    occupants.push_back(candidate.handle);
            });
            for (EntityHandle occupant : occupants)
                entity_detach_from_vehicle(world, occupant);
            world.registry.despawn(orphan);
        }
    }
    world.registry.for_each([&](const Entity &snapshot) {
        if (!snapshot.emplacement_parent.valid()) return;
        // A stock streamed child carries an exact absolute spawn pose, but its
        // parent/type pair can map to multiple authored addeweap slots. Only a
        // resolved attachment row may replace that wire pose with a userpoint
        // pose. Orphan ownership and mounted-rider refresh remain independent.
        if (!snapshot.emplacement_pose_metadata_resolved) return;
        Entity *child = world.registry.get(snapshot.handle);
        const Entity *parent =
                world.registry.get(snapshot.emplacement_parent);
        if (child == nullptr || parent == nullptr ||
            parent->registry_spawn_id !=
                    snapshot.emplacement_parent_spawn_id)
            return;
        Seat anchor;
        anchor.type = SeatType::Gunner;
        anchor.bone_index = child->emplacement_bone;
        anchor.seat_local = child->emplacement_local;
        anchor.yaw_offset = child->emplacement_yaw_offset;
        anchor.attachment_frame = true;
        pose_mounted_occupant(world, *child, *parent, anchor);
    });

    // A gunner riding an attached child was posed earlier in the AI system loop,
    // before the carrier moved. Refresh those occupants from the child's fresh pose.
    world.registry.for_each([&](const Entity &snapshot) {
        if (!snapshot.mounted) return;
        Entity *occupant = world.registry.get(snapshot.handle);
        const Entity *target = world.registry.get(snapshot.mount_target);
        if (occupant == nullptr || target == nullptr ||
            !target->emplacement_parent.valid() ||
            snapshot.mount_seat < 0 ||
            snapshot.mount_seat >= static_cast<int>(target->seats.size()))
            return;
        if (world.ai != nullptr) {
            if (AiEntity *body = world.ai->for_handle(snapshot.handle)) {
                world.ai->pose_if_mounted(*body, world);
                return;
            }
        }
        pose_mounted_occupant(
                world, *occupant, *target, target->seats[snapshot.mount_seat]);
    });
}

// ----------------------------------------------------------------------------
// EntityCommands — the shared Entity_* primitive layer.
// In this foundational core, commands mutate the clean Entity model directly.
// (Replication routing through World::net is the deferred MP seam; LocalSink
// makes single-player run everything locally.)
// ----------------------------------------------------------------------------

// Script SSN -> entity handle, with the retail PLAYER mapping. Mission scripts
// (WAC SSN* commands + BMS Single triggers/actions) address the local player as
// SSN 10000 — retail player entities carry 10000+slot as their net id, so
// EntityPool_FindByNetId @0x4f0a20 resolves them like any SSN. Our player
// entities deliberately carry net_id 0 (the wire is handle-based), so this
// script seam restores the mapping; MP joiner SSNs (10001+) wait on the net
// track. [orig: dfx2med player-slot SSN convention; EntityPool_FindByNetId]
EntityHandle EntityCommands::resolve_ssn(uint16_t ssn) const {
    if (ssn == kLocalPlayerSsn && world_.cached.local_player.valid())
        return world_.cached.local_player;
    return world_.registry.find_by_net_id(ssn);
}

bool EntityCommands::kill_ssn(uint16_t ssn) {
    Entity *e = world_.registry.get(resolve_ssn(ssn));
    if (!e) return false;
    e->alive = false;
    e->health = 0;
    return true;
}

bool EntityCommands::remove_ssn(uint16_t ssn) {
    EntityHandle h = resolve_ssn(ssn);
    if (!world_.registry.get(h)) return false;
    world_.registry.despawn(h);
    return true;
}

bool EntityCommands::set_ssn_hp(uint16_t ssn, int32_t hp) {
    Entity *e = world_.registry.get(resolve_ssn(ssn));
    if (!e) return false;
    e->health = hp;
    e->alive = hp > 0;
    return true;
}

bool EntityCommands::add_ssn_hp(uint16_t ssn, int32_t delta) {
    Entity *e = world_.registry.get(resolve_ssn(ssn));
    if (!e) return false;
    e->health += delta;
    if (e->health < 0) e->health = 0;
    e->alive = e->health > 0;
    return true;
}

namespace {

// The per-entity leg of a waypoint REDIRECT [orig: Entity_SetWaypointByTeam @0x43cdb4]:
// a mounted NON-player auto-detaches [orig: Entity_DetachFromVehicleIfServer @0x4359d0],
// the entity route fields update, and the brain (when the entity carries one) takes the
// mode/list/node order + the turn-budget seed.
void apply_waypoint_order(World &world, Entity &e, int32_t list, int32_t node) {
    const bool is_player = e.handle.pool() == 0 && e.player_class != 0;
    if (e.mounted && !is_player) world.commands.dismount(e.net_id);
    e.waypoint_id = static_cast<uint8_t>(list);
    // Retail keeps the authored node, resolving only the -1 sentinel to nearest.
    // [orig: Entity_SetWaypointByTeam @0x43cdb4 ->
    // Entity_FindNearestTriggerByType @0x407ea0]
    e.wp_number = node >= 0 ? node : 0;
    if (world.ai != nullptr) {
        if (AiEntity *ae = world.ai->for_handle(e.handle)) {
            world.ai->apply_route_order(*ae, list, node);
            // Mirror the resolved nearest/clamped node into the registry entity,
            // which is the script/debug-facing route state.
            if (ae->brain.f[AiBrain::kWpType] == 1 &&
                ae->brain.f[AiBrain::kWpChannel] == list)
                e.wp_number = ae->brain.f[AiBrain::kWpNode];
        }
    }
}

} // namespace

bool EntityCommands::set_ssn_waypoint(uint16_t ssn, int32_t wp, int32_t node) {
    Entity *e = world_.registry.get(resolve_ssn(ssn));
    if (!e) return false;
    apply_waypoint_order(world_, *e, wp, node);
    return true;
}

bool EntityCommands::set_ssn_alert(uint16_t ssn, int32_t state) {
    Entity *e = world_.registry.get(resolve_ssn(ssn));
    if (!e) return false;
    e->alert_state = static_cast<uint8_t>(state);
    return true;
}

bool EntityCommands::set_ssn_target(uint16_t ssn, uint16_t target) {
    Entity *e = world_.registry.get(resolve_ssn(ssn));
    if (!e) return false;
    e->ai_target = target;
    return true;
}

bool EntityCommands::set_ssn_move_speed(uint16_t ssn, int32_t kph) {
    Entity *e = world_.registry.get(resolve_ssn(ssn));
    if (!e) return false;
    e->move_speed_kph = kph;
    return true;
}

bool EntityCommands::set_ssn_engage_min(uint16_t ssn, int32_t v) {
    Entity *e = world_.registry.get(resolve_ssn(ssn));
    if (!e) return false;
    e->engage_min = v;
    return true;
}

bool EntityCommands::set_ssn_engage_max(uint16_t ssn, int32_t v) {
    Entity *e = world_.registry.get(resolve_ssn(ssn));
    if (!e) return false;
    e->engage_max = v;
    return true;
}

bool EntityCommands::set_ssn_attack_max(uint16_t ssn, int32_t v) {
    Entity *e = world_.registry.get(resolve_ssn(ssn));
    if (!e) return false;
    e->attack_max = v;
    return true;
}

bool EntityCommands::set_ssn_anim(uint16_t ssn, int32_t anim_slot) {
    Entity *e = world_.registry.get(resolve_ssn(ssn));
    if (!e) return false;
    e->body_anim_slot = anim_slot; // the present-pass clip channel (not the +0x374 selector)
    return true;
}

bool EntityCommands::set_ssn_hidden(uint16_t ssn, bool hidden) {
    Entity *e = world_.registry.get(resolve_ssn(ssn));
    if (!e) return false;
    e->hidden = hidden;
    return true;
}

bool EntityCommands::set_ssn_held(uint16_t ssn, bool held) {
    Entity *e = world_.registry.get(resolve_ssn(ssn));
    if (!e) return false;
    e->held = held;
    return true;
}

bool EntityCommands::set_ssn_disabled(uint16_t ssn, bool disabled) {
    Entity *e = world_.registry.get(resolve_ssn(ssn));
    if (!e) return false;
    e->disabled = disabled;
    return true;
}

bool EntityCommands::ssn_exists(uint16_t ssn) const {
    return world_.registry.get(resolve_ssn(ssn)) != nullptr;
}

bool EntityCommands::ssn_alive(uint16_t ssn) const {
    const Entity *e = world_.registry.get(resolve_ssn(ssn));
    return e != nullptr && e->alive;
}

bool EntityCommands::ssn_dead(uint16_t ssn) const {
    const Entity *e = world_.registry.get(resolve_ssn(ssn));
    return e != nullptr && !e->alive;
}

bool EntityCommands::ssn_in_area(uint16_t ssn, int area_id) const {
    const Entity *e = world_.registry.get(resolve_ssn(ssn));
    const Area *a = world_.registry.area(area_id);
    if (!e || !a) return false;
    return a->bounds.contains(e->position);
}

bool EntityCommands::local_player_out_of_bounds() const {
    // [orig: Entity_IsLocalPlayerOutOfBounds @0x439d40] At least one area-trigger
    // record with the Active flag (bit0), and the local player's X/Y inside NONE
    // of the active zones' X/Y AABBs — the Z axis is ignored. No local player
    // (a serve-only host) reads as in-bounds.
    const Entity *p = world_.registry.get(world_.cached.local_player);
    if (!p) return false;
    bool any_active = false;
    for (int id = 0;; ++id) {
        const Area *a = world_.registry.area(id);
        if (!a) break;
        if (!a->active) continue;
        any_active = true;
        if (p->position.x >= a->bounds.min.x && p->position.x <= a->bounds.max.x &&
            p->position.y >= a->bounds.min.y && p->position.y <= a->bounds.max.y)
            return false;
    }
    return any_active;
}

int EntityCommands::kill_group(int group) {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (e) { e->alive = false; e->health = 0; ++n; }
    }
    return n;
}

int EntityCommands::group_to_waypoint(int group, int32_t wp, int32_t node) {
    // [orig: Entity_SetWaypointByTeam @0x43cdb4 — commandGroup match over pools 0..1]
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (e) { apply_waypoint_order(world_, *e, wp, node); ++n; }
    }
    return n;
}

int EntityCommands::set_group_hp(int group, int32_t hp) {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (e) { e->health = hp; e->alive = hp > 0; ++n; }
    }
    return n;
}

int EntityCommands::set_group_engage_min(int group, int32_t v) {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (e) { e->engage_min = v; ++n; }
    }
    return n;
}

int EntityCommands::set_group_engage_max(int group, int32_t v) {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (e) { e->engage_max = v; ++n; }
    }
    return n;
}

int EntityCommands::set_group_attack_max(int group, int32_t v) {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (e) { e->attack_max = v; ++n; }
    }
    return n;
}

bool EntityCommands::group_alive(int group) const {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    for (EntityHandle h : members) {
        const Entity *e = world_.registry.get(h);
        if (e && e->alive) return true;
    }
    return false;
}

bool EntityCommands::group_dead(int group) const {
    return !group_alive(group);
}

// --- mount / emplacement (AttachToEmplaced) ---

int EntityCommands::find_best_seat(const Entity &target, EntityHandle occupant,
                                   SeatSelectionMode mode) const {
    // [orig: Entity_FindBestSeatSlot @0x4351f0] lowest weight wins; skip None/taken seats.
    int best = -1;
    int32_t best_weight = 65536000; // [orig: bestWeight init sentinel]
    for (int i = 0; i < static_cast<int>(target.seats.size()); ++i) {
        const Seat &s = target.seats[i];
        if (s.type == SeatType::None) continue;           // [orig: boneIdx != 0]
        if (!seat_allowed_for_mode(s.type, mode)) continue;
        if (s.occupant.valid() && s.occupant != occupant) // [orig: owner==0xFFFF || owner==self]
            continue;
        int32_t w;
        switch (s.type) {
            case SeatType::Controller:
            case SeatType::Driver:    w = 0x2000;   break; // [orig: case 2/5]
            case SeatType::Passenger: w = 0x200000; break; // [orig: case 1, on-vehicle]
            case SeatType::Gunner:
            default:                  w = 0x20000;  break; // [orig: default (UseGun)]
        }
        if (w < best_weight) { best = i; best_weight = w; }
    }
    return best;
}

bool EntityCommands::mount(uint16_t occupant_ssn, uint16_t target_ssn, SeatSelectionMode mode) {
    // [orig: WacScript_TryMountEntityToVehicle @0x4f70f0] resolve both; reject already-mounted /
    // seatless; pick the best seat; write both sides; pose now.
    EntityHandle oh = resolve_ssn(occupant_ssn);
    EntityHandle th = resolve_ssn(target_ssn);
    Entity *occ = world_.registry.get(oh);
    Entity *tgt = world_.registry.get(th);
    if (!occ || !tgt) return false;
    if (occ->mounted) return false;       // [orig: entity->pad8[8] set -> return 0]
    if (tgt->seats.empty()) return false; // [orig: no model+144 vehicle / no seats]
    const int seat_idx = find_best_seat(*tgt, oh, mode);
    if (seat_idx < 0) return false;
    Seat &s = tgt->seats[seat_idx];
    presnap_vehicle_attach_heading(world_, *occ, *tgt, s);
    s.occupant = oh;                                       // [orig: vehicle[400+2*slot] = handle]
    occ->mount_target = th;                                // [orig: occupant+364]
    occ->mount_target_net_id = tgt->net_id;
    occ->mount_target_bms_id = tgt->bms_id;
    occ->mount_target_spawn_origin = tgt->spawn_origin;
    occ->mount_seat = static_cast<int8_t>(seat_idx);       // [orig: occupant+360]
    occ->mount_type = s.type;
    occ->mount_bone = s.bone_index;                        // [orig: occupant+0x157]
    occ->mounted = true;
    if (s.type == SeatType::Gunner) {
        // UseGun clears the transient drowning/in-air pair but does not set the
        // generic carried/vehicle flag. [orig: Entity_AttachToUseGunSlot
        // @0x546c56-0x546c7c clears 0xA000]
        occ->flags &= ~(kEntityFlagDrowning | kEntityFlagInAir);
        occ->engine_flags &= ~(kEntityFlagDrowning | kEntityFlagInAir);
    } else {
        // Ordinary vehicle slots clear the pair and mark the occupant carried.
        // [orig: Entity_AttachToVehicleSlot @0x494752-0x494775, the
        // `& 0xFFFF5FBF | 0x40` form — the masks are static_asserted at the
        // vehicle_attach.cpp twin]
        occ->flags = (occ->flags & ~(kEntityFlagDrowning | kEntityFlagInAir | kEntityFlagMounted)) |
                     kEntityFlagMounted;
        occ->engine_flags =
                (occ->engine_flags & ~(kEntityFlagDrowning | kEntityFlagInAir | kEntityFlagMounted)) |
                kEntityFlagMounted;
    }
    occ->mounted_config_valid = tgt->emplaced_config_valid;
    occ->mounted_config = tgt->emplaced_config_valid ? tgt->emplaced_config : 0;
    if (s.type == SeatType::Gunner)
        vehicle_bind_use_gun_slot(world_, *occ, *tgt);
    pose_mounted_occupant(world_, *occ, *tgt, s);
    vehicle_claim_primary_occupant(world_, *tgt, oh, s.type); // [orig: +368 claim @0x4946d0]
    return true;
}

bool EntityCommands::mount_boarding_command(uint16_t occupant_ssn, uint16_t target_ssn,
                                            uint8_t command_id) {
    SeatSelectionMode mode = SeatSelectionMode::Any;
    switch (command_id) {
        case 123:
            mode = SeatSelectionMode::PassengerOnly;
            break;
        case 124:
            mode = SeatSelectionMode::RejectController;
            break;
        case 125:
            break;
        default:
            return false;
    }
    return mount(occupant_ssn, target_ssn, mode);
}

bool EntityCommands::mount_best(uint16_t occupant_ssn) {
    // [orig: EventAction_Dispatch case 0x25 @0x4542e0 -> the vehicle is occupant-model+144.]
    // Proximity proxy: the nearest entity offering a free seat within kMountRadius.
    EntityHandle oh = resolve_ssn(occupant_ssn);
    const Entity *occ = world_.registry.get(oh);
    if (!occ || occ->mounted) return false;
    const Vec3 p = occ->position;
    EntityHandle best;
    double best_d2 = kMountRadius * kMountRadius + 1.0;
    world_.registry.for_each([&](const Entity &e) {
        if (e.handle == oh || e.seats.empty()) return;
        if (find_best_seat(e, oh) < 0) return; // no free seat for this occupant
        const double dx = e.position.x - p.x, dy = e.position.y - p.y, dz = e.position.z - p.z;
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= kMountRadius * kMountRadius && d2 < best_d2) { best_d2 = d2; best = e.handle; }
    });
    const Entity *tgt = world_.registry.get(best);
    if (!tgt) return false;
    return mount(occupant_ssn, tgt->net_id);
}

bool EntityCommands::dismount(uint16_t occupant_ssn) {
    // [orig: Entity_DetachFromVehicle @0x4355f0] free the seat + clear the occupant's mount ref.
    Entity *occ = world_.registry.get(resolve_ssn(occupant_ssn));
    if (!occ || !occ->mounted) return false;
    const bool claim_capable_seat = occ->mount_type != SeatType::Passenger &&
                                    occ->mount_type != SeatType::None;
    const uint16_t target_net_id = occ->mount_target_net_id;
    const int32_t target_bms_id = occ->mount_target_bms_id;
    const uint32_t target_spawn_origin = occ->mount_target_spawn_origin;
    const uint16_t target_wire_handle = occ->mount_target.packed;
    const EntityHandle oh = occ->handle;
    Entity *tgt = world_.registry.get(occ->mount_target);
    if (tgt && occ->mount_seat >= 0 && occ->mount_seat < static_cast<int>(tgt->seats.size()))
        tgt->seats[occ->mount_seat].occupant = EntityHandle{}; // [orig: vehicle[400+2*slot]=0xFFFF]
    vehicle_release_use_gun_slot(*occ, tgt);
    occ->mounted = false;
    occ->flags &= ~kEntityFlagMounted;
    occ->engine_flags &= ~kEntityFlagMounted;
    occ->mount_target = EntityHandle{};
    occ->mount_target_net_id = 0;
    occ->mount_target_bms_id = 0;
    occ->mount_target_spawn_origin = 0;
    occ->mount_seat = -1;
    occ->mount_type = SeatType::None;
    occ->mount_bone = 0;
    occ->mounted_config_valid = false;
    occ->mounted_config = 0;
    if (tgt != nullptr) {
        vehicle_release_primary_occupant(world_, *tgt, oh); // [orig: +368 leg @0x4356e9]
    } else if (claim_capable_seat) {
        // The vehicle entity is already gone; its stored identity carries the stop so the
        // host tears the presentation down (host cleanup — the claimant check is
        // unavailable, and a spurious stop is idempotent downstream).
        emit_vehicle_control_stopped(world_, target_net_id, target_bms_id,
                                     target_spawn_origin, target_wire_handle);
    }
    return true;
}

uint16_t EntityCommands::find_mounted_on(uint16_t target_ssn) const {
    // [orig: Vehicle_HasEnemyOccupant @0x4359f0] first occupant riding target_ssn, else 0.
    EntityHandle th = resolve_ssn(target_ssn);
    if (!th.valid()) return 0;
    uint16_t result = 0;
    world_.registry.for_each([&](const Entity &e) {
        if (result == 0 && e.mounted && e.mount_target == th) result = e.net_id;
    });
    return result;
}

namespace {

// The shared frame of the four Player mount triggers: resolve the SSN entity + a live
// local player. [orig: the common head of @0x4f10d0/0x4f1260/0x4f1150/0x4f11e0 — handle
// resolve, ItemTypeIndex != 0, local player set, !(Flags & 2)]
const Entity *mount_trigger_ssn(const World &w, const EntityCommands &cmds, uint16_t ssn,
                                const Entity **local_out) {
    const Entity *local = w.registry.get(w.cached.local_player);
    if (local == nullptr || !local->alive || local->health <= 0 || (local->flags & 2u) != 0)
        return nullptr;
    const Entity *target = w.registry.get(cmds.resolve_ssn(ssn));
    if (target == nullptr) return nullptr;
    *local_out = local;
    return target;
}

// entity == candidate OR entity's standing-carrier == candidate (one link deep).
// [orig: `vehicle == entity || vehicle->groundEntity == entity` @0x4f113d]
bool is_or_carried_by(const World &w, EntityHandle chain_head, const Entity &candidate) {
    const Entity *head = w.registry.get(chain_head);
    if (head == nullptr) return false;
    if (head->handle == candidate.handle) return true;
    return head->ground_target == candidate.handle;
}

} // namespace

bool EntityCommands::local_player_attached_to_ssn(uint16_t ssn) const {
    // [orig: Entity_IsLocalPlayerSeatedOnSsn @0x4f10d0 — parentEntity chain, any seat]
    const Entity *local = nullptr;
    const Entity *target = mount_trigger_ssn(world_, *this, ssn, &local);
    if (target == nullptr) return false;
    return local->mounted && is_or_carried_by(world_, local->mount_target, *target);
}

bool EntityCommands::local_player_standing_on_ssn(uint16_t ssn) const {
    // [orig: Entity_IsLocalPlayerStandingOnSsn @0x4f1260 — groundEntity chain]
    const Entity *local = nullptr;
    const Entity *target = mount_trigger_ssn(world_, *this, ssn, &local);
    if (target == nullptr) return false;
    return is_or_carried_by(world_, local->ground_target, *target);
}

bool EntityCommands::local_player_driving_ssn(uint16_t ssn) const {
    // [orig: Entity_IsLocalPlayerDrivingSsn @0x4f1150 — the seat chain + parentSlot 2/5]
    if (!local_player_attached_to_ssn(ssn)) return false;
    const Entity *local = world_.registry.get(world_.cached.local_player);
    return local != nullptr && is_vehicle_control_seat(local->mount_type);
}

bool EntityCommands::local_player_on_gun_of_ssn(uint16_t ssn) const {
    // [orig: Entity_IsLocalPlayerOnGunOfSsn @0x4f11e0 — the seat chain + parentSlot 3]
    if (!local_player_attached_to_ssn(ssn)) return false;
    const Entity *local = world_.registry.get(world_.cached.local_player);
    return local != nullptr && local->mount_type == SeatType::Gunner;
}

// --- AI command (the AI-change action family) ---
// [orig: Entity_ApplyCommand @0x43ab60.] Resolve the target's brain through World::ai and
// apply the sub-type command in-engine. No AI system / no brain -> no-op.

bool EntityCommands::apply_ai_command(uint16_t ssn, int sub_type, int32_t p2, int32_t p3, int32_t p4) {
    if (!world_.ai) return false;
    AiEntity *ae = world_.ai->for_handle(resolve_ssn(ssn));
    if (!ae) return false;
    ai_apply_command(ae->brain, sub_type, p2, p3, p4);
    return true;
}

int EntityCommands::apply_group_ai_command(int group, int sub_type, int32_t p2, int32_t p3, int32_t p4) {
    // The alert-change subs also stamp the per-group alert record the cat-1
    // triggers read, independent of any AI brains [orig: Entity_HandleAlertCommand
    // @ 0x43cff7 maps sub 5 -> red, 6 -> green, 22 -> yellow via
    // TriggerGroup_SetAlertRed/Green/Yellow @ 0x40d630/0x40d5f0/0x40d610].
    if (group > 0 && group < TriggerRelations::kGroups) {
        if (sub_type == 5) world_.relations.group(group).alert = TriggerRelations::kAlertRed;
        else if (sub_type == 6) world_.relations.group(group).alert = TriggerRelations::kAlertGreen;
        else if (sub_type == 22) world_.relations.group(group).alert = TriggerRelations::kAlertYellow;
    }
    if (!world_.ai) return 0;
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        AiEntity *ae = world_.ai->for_handle(h);
        if (ae) { ai_apply_command(ae->brain, sub_type, p2, p3, p4); ++n; }
    }
    return n;
}

int EntityCommands::apply_area_ai_command(int zone_area_id, int team, int sub_type,
                                          int32_t p2, int32_t p3, int32_t p4) {
    // AREA_AI_RED/BLUE: apply to the team's units inside a zone. [target = zone area id,
    // team filter: blue=1/red=2; the exact BMS zone->area mapping is grill-gated (P5).]
    if (!world_.ai) return 0;
    const Area *a = world_.registry.area(zone_area_id);
    if (!a) return 0;
    std::vector<EntityHandle> in;
    world_.registry.in_area(a->bounds, in);
    int n = 0;
    for (EntityHandle h : in) {
        const Entity *e = world_.registry.get(h);
        if (!e || e->team != static_cast<uint8_t>(team)) continue;
        AiEntity *ae = world_.ai->for_handle(h);
        if (ae) { ai_apply_command(ae->brain, sub_type, p2, p3, p4); ++n; }
    }
    return n;
}

// ----------------------------------------------------------------------------
// World
// ----------------------------------------------------------------------------

namespace {

constexpr int32_t kFogMinimumQ16 = 2 << 16;
constexpr int32_t kFogReferenceQ16 = 1024 << 16;
constexpr uint32_t kTodDayFixed24 = 24u << 24;

int32_t authored_distance_q16(int32_t distance) noexcept {
    const int64_t fixed = static_cast<int64_t>(distance) << 16;
    return static_cast<int32_t>(std::clamp<int64_t>(
            fixed, kFogMinimumQ16, kFogReferenceQ16));
}

uint32_t transition_step(int32_t current, int32_t target, int32_t seconds) noexcept {
    // Rounded per-tick step over seconds*62 ticks — the same rounding the
    // witnessed weather scalar transition uses on the render side
    // [orig: Environment_UpdateWeatherTick @ 0x57ede2 family; engine/formats/env mirror].
    int64_t ticks = static_cast<int64_t>(seconds) * 62;
    if (ticks == 0) ticks = 1;
    if (ticks < 0) ticks = -ticks;
    int64_t centered = static_cast<int64_t>(target) - current + ticks / 2;
    if (centered < 0) centered = -centered;
    return static_cast<uint32_t>(std::min<int64_t>(centered / ticks, 0xFFFFFFFFll));
}

int32_t authored_percent_q16(int32_t percent) noexcept {
    const int64_t fixed = (static_cast<int64_t>(percent) << 16) / 100;
    return static_cast<int32_t>(std::min<int64_t>(fixed, 0x10000));
}

int32_t spring_tick(int32_t current, int32_t target, uint32_t step,
                    int32_t max_abs) noexcept {
    // 1/32 spring toward target with a per-tick step clamp — the witnessed
    // weather spring [orig: Environment_UpdateWeatherTick @ 0x57ede2].
    int64_t delta = (static_cast<int64_t>(target) - current + 31) >> 5;
    const int64_t clamp = std::min<uint64_t>(step, 0x7FFFFFFFu);
    delta = std::clamp<int64_t>(delta, -clamp, clamp);
    return static_cast<int32_t>(std::clamp<int64_t>(
            static_cast<int64_t>(current) + delta, -max_abs, max_abs));
}

} // namespace

void EnvNetworkState::publish_complete(const EnvNetworkSample &sample) noexcept {
    const bool first = !valid;
    if (first || (scripted_channels_ & kScriptedFog) == 0) {
        fog_target_q16 = sample.fog_target_q16;
        fog_current_q16_ = sample.fog_current_q16;
        fog_accel_clamp = sample.fog_accel_clamp;
    }
    if (first) {
        tod_fixed24 = sample.tod_fixed24;
        tod_advance_per_tick_ = sample.tod_advance_per_tick;
    } else if ((scripted_channels_ & kScriptedTod) == 0) {
        tod_fixed24 = sample.tod_fixed24;
    } else if (tod_advance_per_tick_ == 0) {
        // An external 62-Hz owner (Godot) advances the resource clock. Retain
        // the scripted absolute time while applying only that base clock delta.
        const uint32_t previous = last_external_tod_fixed24_ % kTodDayFixed24;
        const uint32_t current = sample.tod_fixed24 % kTodDayFixed24;
        const uint32_t delta = (current + kTodDayFixed24 - previous) % kTodDayFixed24;
        tod_fixed24 = (tod_fixed24 + delta) % kTodDayFixed24;
    }
    last_external_tod_fixed24_ = sample.tod_fixed24;
    if (first || (scripted_channels_ & kScriptedQuake) == 0)
        quake_ticks = sample.quake_ticks;
    if (first || (scripted_channels_ & kScriptedCloud) == 0)
        cloud_scroll_rate_target = sample.cloud_scroll_rate_target;
    if (first || (scripted_channels_ & kScriptedPrecipitation) == 0) {
        rain_pct_current_q16 = sample.rain_pct_current_q16;
        rain_target_q16_ = static_cast<int32_t>(sample.rain_pct_current_q16);
        precipitation_kind = sample.precipitation_kind;
    }
    if (first || (scripted_channels_ & kScriptedOvercast) == 0) {
        overcast_blend_q16 = sample.overcast_blend_q16;
        overcast_target_q16_ = static_cast<int32_t>(sample.overcast_blend_q16);
    }
    valid = true;
    ++generation;
}

void EnvNetworkState::initialize_mission_start() noexcept {
    if (!valid) return;
    fog_current_q16_ = fog_target_q16;
    fog_accel_clamp = 0x00FF0000u;
    fog_reference_q16_ = 1000 << 16;
    rain_pct_current_q16 = static_cast<uint32_t>(rain_target_q16_);
    rain_step_q16_ = 0x1000u;
    overcast_blend_q16 = static_cast<uint32_t>(overcast_target_q16_);
    overcast_step_q16_ = 0x1000u;
    ++generation;
}

void EnvNetworkState::command_fog_distance(int32_t authored_distance) noexcept {
    const int32_t target = authored_distance_q16(authored_distance);
    const int64_t delta = static_cast<int64_t>(target) - fog_current_q16_;
    fog_target_q16 = target;
    fog_accel_clamp = static_cast<uint32_t>(delta < 0 ? -delta : delta);
    scripted_channels_ |= kScriptedFog;
    ++generation;
}

void EnvNetworkState::command_move_fog(int32_t authored_distance, int32_t seconds) noexcept {
    const int32_t target = authored_distance_q16(authored_distance);
    fog_target_q16 = target;
    fog_accel_clamp = transition_step(fog_current_q16_, target, seconds);
    scripted_channels_ |= kScriptedFog;
    ++generation;
}

void EnvNetworkState::command_sky_speed(int32_t authored_rate) noexcept {
    cloud_scroll_rate_target = static_cast<uint32_t>(authored_rate) << 10;
    scripted_channels_ |= kScriptedCloud;
    ++generation;
}

void EnvNetworkState::command_precipitation(int32_t authored_percent, int32_t seconds,
                                            PrecipitationKind kind) noexcept {
    rain_target_q16_ = authored_percent_q16(authored_percent);
    rain_step_q16_ = transition_step(
            static_cast<int32_t>(rain_pct_current_q16), rain_target_q16_, seconds);
    precipitation_kind = static_cast<uint32_t>(kind);
    scripted_channels_ |= kScriptedPrecipitation;
    ++generation;
}

void EnvNetworkState::command_overcast(int32_t authored_percent, int32_t seconds) noexcept {
    overcast_target_q16_ = authored_percent_q16(authored_percent);
    overcast_step_q16_ = transition_step(
            static_cast<int32_t>(overcast_blend_q16), overcast_target_q16_, seconds);
    scripted_channels_ |= kScriptedOvercast;
    ++generation;
}

void EnvNetworkState::command_quake(int32_t authored_duration) noexcept {
    // [orig: sub_4ED4C0 — Env_QuakeTicks = 6 * value]
    quake_ticks = authored_duration <= 0
            ? 0u
            : static_cast<uint32_t>(static_cast<uint64_t>(authored_duration) * 6u);
    scripted_channels_ |= kScriptedQuake;
    ++generation;
}

void EnvNetworkState::command_time_of_day_minutes(int32_t minute_of_day) noexcept {
    // [orig: TOD handler @0x4EDC70] ParamType::Hour resolves to minutes;
    // the handler multiplies directly by 0x44444 into the 8.24 accumulator.
    tod_fixed24 = static_cast<uint32_t>(minute_of_day) * 0x44444u;
    scripted_channels_ |= kScriptedTod;
    ++generation;
}

void EnvNetworkState::advance_tick() noexcept {
    bool changed = false;
    if (tod_advance_per_tick_ != 0) {
        tod_fixed24 = (tod_fixed24 + tod_advance_per_tick_) % kTodDayFixed24;
        changed = true;
    }
    if ((scripted_channels_ & kScriptedQuake) != 0 && quake_ticks != 0) {
        --quake_ticks;
        changed = true;
    }
    if ((scripted_channels_ & kScriptedFog) != 0) {
        const int32_t next = spring_tick(fog_current_q16_, fog_target_q16,
                                        fog_accel_clamp, fog_reference_q16_);
        changed |= next != fog_current_q16_;
        fog_current_q16_ = next;
    }
    if ((scripted_channels_ & kScriptedPrecipitation) != 0) {
        const int32_t current = static_cast<int32_t>(rain_pct_current_q16);
        const int32_t next = spring_tick(current, rain_target_q16_, rain_step_q16_, 0x10000);
        changed |= next != current;
        rain_pct_current_q16 = static_cast<uint32_t>(next);
    }
    if ((scripted_channels_ & kScriptedOvercast) != 0) {
        const int32_t current = static_cast<int32_t>(overcast_blend_q16);
        const int32_t next = spring_tick(
                current, overcast_target_q16_, overcast_step_q16_, 0x10000);
        changed |= next != current;
        overcast_blend_q16 = static_cast<uint32_t>(next);
    }
    if (changed) ++generation;
}

uint16_t World::next_prng16() noexcept {
    // [orig: PRNG_Next16 @0x6130a0 / @0x613140, both over
    // dword_31BFBB0] s = rol4(s + rol11(s)) ^ 1; return low word.
    const uint32_t rol11 = (prng16_state << 11) | (prng16_state >> 21);
    uint32_t next = prng16_state + rol11;
    next = ((next << 4) | (next >> 28)) ^ 1u;
    prng16_state = next;
    return static_cast<uint16_t>(next);
}

void World::add_system(ISystem *sys) {
    if (sys) systems_.push_back(sys);
}

void World::load_systems() {
    for (ISystem *s : systems_) s->on_load(*this);
}

void World::run_logic_tick(bool is_authority, bool pre_mission) {
    // [orig: WacScript_AdvanceTick refreshes the per-tick local-player cache via
    // WacScript_CacheLocalPlayerState @0x4f5780 at the top of the tick, before the
    // script evaluators read it. Deferred: the mission sim has no local-player avatar
    // yet, so `cached` stays host-populated and the WAC near-* builtins read it as-is.]
    TickContext ctx;
    ctx.world = this;
    ctx.logic_tick = logic_tick;
    ctx.is_authority = is_authority;
    ctx.pre_mission = pre_mission;
    // The pending fire-sound countdown, before this tick's spawns: retail
    // drains after the client network frame (whose receive seeds our embedder
    // also applies pre-tick) and before the server/entity updates that seed
    // the rest [orig: Sound_TickPendingSlots @ 0x526697 in
    // Game_ProcessMainFrame, between Client_ProcessNetworkFrame and
    // Server_TickUpdate / Entity_UpdateAllEntities].
    fire_sounds.tick();
    // The presenting-client identity for the spawn-time tracer style select — stamped
    // before the system loop so rounds spawned THIS tick (AI fire, local fire) select
    // against fresh values [orig: g_local_player_entity->Team read @ 0x4ec740].
    round_sim.local_player = cached.local_player;
    if (const Entity *lp = registry.get(cached.local_player))
        round_sim.local_team = static_cast<uint8_t>(lp->team);
    // The system loop runs on BOTH the authoritative host and a non-authority client
    // (the original client also runs a tick): each system self-gates on
    // ctx.is_authority. WacSystem / BmsEventSystem early-out on a client (scripting is
    // host-only; the in-match C2S drain is host-only too, owned by Server_TickUpdate, not an
    // ISystem); AiSystem on a client simulates ONLY the
    // local player (the §5.38 entity==local-player branch) and leaves every other
    // entity to the replicated wire state. [orig: the client tick still steps the
    // local player's infantry motor; Server_TickUpdate / Game_ProcessMainFrame.]
    for (ISystem *s : systems_) s->tick(*this, ctx);
    pose_emplacement_attachments(*this);
    // Entity_UpdateAllEntities walks pool 1 before the projectile pool. That
    // prevents a newly converted charge from losing an arm-delay tick and lets
    // claymore shrapnel fly later in its detonation frame [orig:
    // Entity_UpdatePool1Slot @0x4b8dd0 -> Weapon_UpdateAllProjectiles @0x4ec020].
    // These presentation events describe only the current authoritative tick.
    if (is_authority && !pre_mission) {
        throwables.events.clear();
        throwables.tick(*this, ai != nullptr ? ai->collision : nullptr, terrain);
    }
    // The global weapon-action pump follows the complete entity/system update and
    // precedes projectile stepping. This is where an AI UseGun nextAction write can
    // become a same-frame round.
    // [orig: Entity_UpdateAllEntities @0x52674b, then
    //  WeaponAction_ProcessAllEntities @0x526786]
    if (!pre_mission && ai != nullptr)
        ai->pump_mounted_weapon_slots(*this, logic_tick);
    // Live rounds step on the host and on an explicitly configured MP
    // non-authority client. The latter is the retail tag-2 visual re-sim path;
    // every decoded/predicted round carries VisualOnly through all consequence
    // sites, so only the host can mutate gameplay state. Do not infer a client
    // role from is_authority=false alone -- tests and pre-mission callers use it too.
    // [orig: Weapon_UpdateAllProjectiles @0x4ec020; §5.60]
    if (!pre_mission &&
        (is_authority || (mp_session && !projectile_authority)))
        round_sim.tick(*this, terrain, ai != nullptr ? ai->collision : nullptr);
    if (!pre_mission &&
        (is_authority || (mp_session && !projectile_authority))) {
        // The explosion-queue drain runs once per frame after the projectile
        // update [orig: Projectile_ProcessExplosionQueue @0x4ead80]; entries the
        // damage callbacks push (the kz death chain) land next tick, exactly like
        // the original's post-reset writes. Dead non-AI items then settle
        // [orig: the Entity_UpdateStaticDeathPhysics / _UpdateFallingDeathPhysics
        // update callbacks] and the death-piece pool advances
        // [orig: DeathPiece_TickAll @0x57b900].
        // Retail runs all three UNGATED on every peer — the shared per-frame
        // entity update calls them on clients too, which is how a joiner's
        // 0x13/0x26-triggered death chain detonates its kz blasts and flies its
        // pieces locally. The MP visual client (the round pool's predicate
        // above) therefore drains them as well; its authoritative state keeps
        // arriving over the wire regardless.
        // [orig: Entity_UpdateAllEntities @0x4c2100 — DeathPiece_TickAll
        //  @0x4c221c, Projectile_ProcessExplosionQueue @0x4c223f, and the
        //  pool-2/3 update-callback walk, all unconditional]
        // The water plane: env.water_z (16.16, the #265 sound-profile home) —
        // zero means "no water authored", the same read the wreck gates use
        // [orig: Env_WaterHeightFixed @0x26c6454].
        const float water_z =
                env.water_z != 0 ? static_cast<float>(env.water_z) / 65536.0f : -1.0e9f;
        explosions.process(*this, ai != nullptr ? ai->collision : nullptr, terrain,
                           water_z, destruction);
        destruction_tick_dead_items(*this, terrain, water_z, destruction);
        death_pieces.tick(*this, terrain, water_z, destruction);
    }
    // The waypoint current-selection pass, from the local player's position (the
    // original runs it in the client frame beside the player update; our SP host
    // is that client — the pure-client view is D-HUD-16). Position converts to
    // the original's 16.16 fixed compare space. [orig: Player_UpdatePerFrame
    // @0x4de5f7]
    if (is_authority && !pre_mission && !waypoints.empty()) {
        if (const Entity *lp = registry.get(cached.local_player))
            waypoints.tick_advance(static_cast<int32_t>(lp->position.x * 65536.0f),
                                   static_cast<int32_t>(lp->position.y * 65536.0f));
    }
    if (is_authority) {
        if (pre_mission) {
            // The one-shot initial group recount, ordered right after the pre
            // pass [orig: Game_StartMission @ 0x525b86 -> @ 0x525b8b].
            recount_group_initials();
        } else if (--group_recount_timer_ <= 0) {
            // The 62-tick live rescan [orig: Server_TickUpdate timer
            // @ 0x51db6d, reload 0x3E @ 0x51db93 -> EntityPool_RecountLiveByGroup
            // @ 0x51dc02].
            group_recount_timer_ = 0x3E;
            recount_group_live();
        }
    }
    ++logic_tick; // [orig: current_tick @0x24c1968 advances once per frame tick]
    // Audio-less/headless hosts never drain presentation. Retire their bounded
    // latest-intent rows on the same logic clock so old entity lifetimes cannot
    // occupy mailbox admission indefinitely.
    sound_emitters.prune(logic_tick);
}

// Structural translation of Server_ProcessRoundEnd @0x5164f0 at SP altitude.
void World::process_round_end(int32_t winning_team) {
    if (round_end.ended) return;          // the double-run guard [orig: @0x516502]
    round_end.winner_team = winning_team; // [orig: g_round_winning_team @0x516528]
    // Unmodeled MP score surfaces, in original order: the winner-team scoring pass
    // (GameEvent_ProcessScoring @0x52f550 per winning-team member @0x516530), the
    // end-of-round scoreboard block build (Server_BuildEndOfRoundScoreboard @0x508f30
    // — round_end.winner_team stands for its winner dword @0x24c1970), and the
    // top-scorer bonus on non-team draws. Net-track wire legs, per active slot in
    // state 6: S2C 0x61 round-end marker (4 zero bytes) @0x516790, S2C 0x1D
    // scoreboard header [u8 winner][s16 score0][s16 score1][u8 draw][s8 myEntryIndex]
    // (EndRoundScoreboard_SerializeHeader @0x505280) @0x516839, CNetPlayer_SetGameState(11)
    // @0x516846, slot state 6->7 @0x51685e; then the per-team round-win counters for
    // game types 0x10000/65537/65540 @0x5168a0 and the MP-only 2790-tick linger
    // @0x5166c4 (drained by Server_TickUpdate -> exit reason 3 / the client frame ->
    // reason 4; SP never drains it — the epilog owns the SP exit).
    round_end.ended = true; // [orig: g_spawn_success_gate latch @0x5168e4]
    // The SP tail [orig: @0x51691d..0x51698f]: stop the dialog audio channel
    // (DialogAudio_PlayNextChunkOrStop(0) @0x51694b) + Dialog_ResetAll + park the
    // mission music, then winner==1 -> the WIN epilog (Cine_InitPlayback @0x578390:
    // <mission>.cne if present, else a static camera; the flyaway + jo_Epil.tga
    // score screen) + end music track 1; anything else -> the LOSE cine
    // (Cine_StartPlayback @0x577840 letterbox/fade + the jo_Epil2.tga MISSION FAILED
    // screen) + end track 2 (MusicCtx_SelectEndTrack @0x672fd0). All host
    // presentation: the effect carries the winner, the host selects the flow.
    effects.push({"round_end", winning_team, 0, 0, 0, std::string()});
}

// Count alive members per commandGroup over the actor pools; group 0 is
// forced to zero [orig: EntityPool_RecountByType @ 0x40e7e0 /
// EntityPool_RecountLiveByGroup @ 0x40e8d0 — !(flags & 2) && health > 0].
static void count_groups(const EntityRegistry &registry,
                         int32_t (&counts)[TriggerRelations::kGroups]) {
    std::vector<EntityHandle> members;
    for (int g = 1; g < TriggerRelations::kGroups; ++g) {
        members.clear();
        registry.by_group(static_cast<uint8_t>(g), members);
        int32_t alive = 0;
        for (EntityHandle h : members) {
            const Entity *e = registry.get(h);
            if (e && e->alive && e->health > 0) ++alive;
        }
        counts[g] = alive;
    }
    counts[0] = 0;
}

void World::recount_group_initials() {
    int32_t counts[TriggerRelations::kGroups] = {};
    count_groups(registry, counts);
    for (int g = 0; g < TriggerRelations::kGroups; ++g) {
        relations.group(g).initial_count = counts[g];
        relations.group(g).live_count = counts[g];
    }
}

void World::recount_group_live() {
    int32_t counts[TriggerRelations::kGroups] = {};
    count_groups(registry, counts);
    for (int g = 0; g < TriggerRelations::kGroups; ++g)
        relations.group(g).live_count = counts[g];
}

World::Snapshot World::snapshot() const {
    Snapshot s;
    s.registry = registry;
    s.vars = vars;
    s.wac_values = wac_values;
    s.env = env;
    s.network_env = network_env;
    s.logic_tick = logic_tick;
    s.prng16_state = prng16_state;
    s.local_player = cached.local_player;
    return s;
}

void World::restore(const Snapshot &s) {
    registry.restore_from(s.registry);
    vars = s.vars;
    wac_values = s.wac_values;
    env = s.env;
    network_env = s.network_env;
    logic_tick = s.logic_tick;
    prng16_state = s.prng16_state;
    // Reset per-tick health/proximity counters, then restore only the stable
    // ownership identity captured with the registry. A post-snapshot player may
    // have reused a baseline actor's slot, while a listen baseline may already
    // contain its host player; copying the current cache or clearing ownership
    // unconditionally gets one of those cases wrong.
    cached = CachedFrameState{};
    cached.local_player = s.local_player;
    effects.clear();
    slot_sounds.clear();
    sound_emitters.clear();
    fire_sounds.clear();
    round_sim.reset();
    explosions.reset();
    throwables.reset();
    death_pieces.reset();
    destruction_rng.reset();
    destruction = DestructionEvents{};
    // Round outcome + kill stats reset with the mission [orig: Game_StartMission —
    // gate clear @0x524a1f + the scoreboard-block memset @0x5249df; the stat buckets
    // clear in the round-start state init].
    round_end = RoundEndState{};
    kill_stats = MissionKillStats{};
    load_systems(); // systems re-init their per-mission state
    if (collision != nullptr) collision->refresh_after_registry_change(*this);
    registry.for_each([&](const Entity &vehicle) {
        if (vehicle.primary_occupant.valid())
            emit_vehicle_control_started(*this, vehicle);
    });
}

} // namespace opennova::world
