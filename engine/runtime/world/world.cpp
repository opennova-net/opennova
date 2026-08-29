#include <runtime/world/world.h>
#include <base/io/perf_clock.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_sound.h>

#include <runtime/world/ai.h> // AiSystem / AiEntity / ai_apply_command — the AI-change command target
#include <base/io/bam.h>

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

Vec3 entity_local_point_world(const Entity &vehicle, const Vec3 &local) {
    // Build the SAME frame collision serves (target_view): heading from the
    // stored mission yaw, pitch/roll BAM-wrapped from degrees, through
    // collision_matrix_from_euler [orig: @0x613f40]. Pure-yaw carriers keep the
    // pre-existing 2D rotate bit-for-bit (the euler matrix reduces to it, but
    // the trig paths differ in rounding; the fast path also skips the matrix).
    const Vec3 &L = local;
    if (vehicle.pitch == 0 && vehicle.roll == 0) {
        constexpr double kDeg2Rad = io::kRadiansPerDegree;
        const double a = static_cast<double>(-vehicle.yaw) * kDeg2Rad;
        const double ca = std::cos(a), sa = std::sin(a);
        Vec3 p;
        p.x = vehicle.position.x + static_cast<float>(L.x * ca - L.y * sa);
        p.y = vehicle.position.y + static_cast<float>(L.x * sa + L.y * ca);
        p.z = vehicle.position.z + L.z;
        return p;
    }
    const int32_t heading =
            bam_heading_from_mission_yaw_deg(static_cast<double>(vehicle.yaw));
    const int32_t origin[3] = {0, 0, 0};
    const CollisionMatrix m = collision_matrix_from_euler(
            heading,
            bam_from_degrees_wrapped(static_cast<double>(vehicle.pitch)),
            bam_from_degrees_wrapped(static_cast<double>(vehicle.roll)), origin);
    // seat_local is pre-swizzled ((-y, x, z) over the raw authored ints — a
    // baked-in Rz(90)), while the collision euler matrix with heading
    // bam(90 - yaw) expects RAW model coordinates: un-swizzle first, so the
    // flat case reduces bit-for-bit to the legacy -yaw rotate above.
    const int32_t lf[3] = {static_cast<int32_t>(L.y * 65536.0f),
                           static_cast<int32_t>(-L.x * 65536.0f),
                           static_cast<int32_t>(L.z * 65536.0f)};
    int32_t wf[3];
    m.rotate_point(lf, wf);
    Vec3 p;
    p.x = vehicle.position.x + static_cast<float>(wf[0]) / 65536.0f;
    p.y = vehicle.position.y + static_cast<float>(wf[1]) / 65536.0f;
    p.z = vehicle.position.z + static_cast<float>(wf[2]) / 65536.0f;
    return p;
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
    // The seat-local offset through the carrier's FULL orientation frame (yaw +
    // pitch + roll). Retail's seat bone path reads the one entity orientation
    // matrix, the same matrix the collision shell is posed with; a yaw-only
    // rotate here left every mounted body (and its dismount start) in an
    // unrolled frame while the collision volumes leaned with the vehicle.
    // [orig: Entity_GetBoneTransformAndOrientation @0x4b0c50 over
    //  Math_BuildFixedPointMatrixFromEulerAngles @0x613f40]
    occ.position = entity_local_point_world(vehicle, seat.seat_local);
    occ.yaw = mounted_pose_yaw(vehicle, seat);
    occ.pitch = vehicle.pitch;
    occ.roll = vehicle.roll;
}

// Attached emplacement children are allocated breadth-first after their carrier,
// so pool/slot iteration is parent-before-child even for turret-on-vehicle chains.
// Reuse the mounted-pose provider: a resolved USRP bone follows live PANM; bone
// zero takes pose_mounted_occupant's parent-root/local fallback.
static void pose_emplacement_attachments(World &world, LogicTickPerf *perf) {
    uint64_t phase_start = perf != nullptr ? io::perf_now_us() : 0;
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
    if (perf != nullptr) {
        const uint64_t now = io::perf_now_us();
        perf->attachment_orphans_us = now - phase_start;
        phase_start = now;
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
    if (perf != nullptr) {
        const uint64_t now = io::perf_now_us();
        perf->attachment_child_pose_us = now - phase_start;
        phase_start = now;
    }

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
    if (perf != nullptr)
        perf->attachment_riders_us = io::perf_now_us() - phase_start;
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

bool EntityCommands::set_entity_health(EntityHandle h, int32_t hp) {
    Entity *e = world_.registry.get(h);
    if (!e) return false;
    e->health = hp;
    e->alive = hp > 0;
    // The AI motor's entity+286 mirror follows, or the next infantry tick
    // hydrates the registry row back [orig: the WAC SETHP op writes entity+286].
    if (world_.ai != nullptr) {
        if (AiEntity *a = world_.ai->for_handle(h)) a->health = static_cast<int16_t>(hp);
    }
    return true;
}

bool EntityCommands::set_entity_position(EntityHandle h, const Vec3 &mission_pos) {
    Entity *e = world_.registry.get(h);
    if (!e) return false;
    e->position = mission_pos;
    if (world_.ai != nullptr) {
        if (AiEntity *a = world_.ai->for_handle(h)) {
            a->pos[0] = static_cast<int32_t>(mission_pos.x * 65536.0f);
            a->pos[1] = static_cast<int32_t>(mission_pos.y * 65536.0f);
            a->pos[2] = static_cast<int32_t>(mission_pos.z * 65536.0f);
        }
    }
    return true;
}

bool EntityCommands::kill_player(EntityHandle victim, EntityHandle killer) {
    Entity *e = world_.registry.get(victim);
    if (e == nullptr || (e->flags & kEntityFlagPlayer) == 0) return false;
    e->health = 0;
    RoundDeath d;
    d.victim = victim;
    d.victim_handle = victim.packed;
    d.killer = killer;
    d.killer_handle = killer.valid() ? killer.packed : 0xFFFFu;
    world_.round_sim.deaths.push_back(d);
    return true;
}

bool EntityCommands::set_entity_weapon_ammo(EntityHandle h, int32_t clip, int32_t reserve) {
    Entity *e = world_.registry.get(h);
    if (e == nullptr) return false;
    e->primary_weapon_slot.clip = retail_signed_i16(clip);
    e->primary_weapon_slot.reserve = retail_signed_i16(reserve);
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

// --- the cat-2 single-state trigger queries (bms-event-runtime-re §3b) -------

namespace {

// Retail's per-helper pool breadth: the alert/health scanners walk pools 0-1,
// the holding walks pool 0 only, the 42-45 family resolves through
// EntityPool_FindByNetId (pools 0-3). Our resolve is registry-wide, so each
// query re-applies its helper's pool gate from the handle's pool bits.
bool in_pools_01(EntityHandle h) { return h.valid() && h.pool() <= 1; }

} // namespace

bool EntityCommands::ssn_at_alert(uint16_t ssn, int level) const {
    // [orig: Entity_IsSsnAtAlertLevel @0x43e780 — SSN 0 -> 0 @0x43e787;
    // pools 0-1; aiRuntime (entity+0x68) null -> 0; byte +0x88 == level]
    if (ssn == 0 || !world_.ai) return false;
    EntityHandle h = resolve_ssn(ssn);
    if (!in_pools_01(h)) return false;
    AiEntity *ae = world_.ai->for_handle(h);
    if (!ae) return false;
    return ae->slot.bytes()[AiSlot::kAlertByte] == level;
}

bool EntityCommands::ssn_damage_taken_at_least(uint16_t ssn, int32_t points) const {
    // [orig: Entity_HasDamageCapacity @0x43e3d0 — SSN 0 -> 0 (the
    // per-helper head guard; the pool finder itself has none); pools 0-1;
    // signed health(+0x11E) <= healthMax(def+0x17C) - points; no null-def
    // guard, no alive gate]
    if (ssn == 0) return false;
    EntityHandle h = resolve_ssn(ssn);
    if (!in_pools_01(h)) return false;
    const Entity *e = world_.registry.get(h);
    if (!e) return false;
    return e->health <= e->health_max - points;
}

bool EntityCommands::ssn_full_health(uint16_t ssn) const {
    // [orig: Entity_HasFullHealth @0x43e470 — SSN 0 -> 0; pools 0-1; null
    // itemDef -> 0 (our health_max == 0 unresolved marker); health >= healthMax]
    if (ssn == 0) return false;
    EntityHandle h = resolve_ssn(ssn);
    if (!in_pools_01(h)) return false;
    const Entity *e = world_.registry.get(h);
    if (!e || e->health_max == 0) return false;
    return e->health >= e->health_max;
}

bool EntityCommands::ssn_health_at_least(uint16_t ssn, int32_t threshold) const {
    // [orig: Entity_HasHealthAboveThreshold @0x43e350 — SSN 0 -> 0;
    // pools 0-1; health >= threshold, def-free]
    if (ssn == 0) return false;
    EntityHandle h = resolve_ssn(ssn);
    if (!in_pools_01(h)) return false;
    const Entity *e = world_.registry.get(h);
    if (!e) return false;
    return e->health >= threshold;
}

bool EntityCommands::ssn_holding_group(uint16_t ssn, int group) const {
    // [orig: Entity_IsSsnHoldingItemGroup @0x43e2f0 — SSN 0 -> 0
    // @0x43e2f7; pool 0 only; mountedChild(+0x268) null -> 0;
    // held->commandGroup(+0x11C) == group]
    if (ssn == 0) return false;
    EntityHandle h = resolve_ssn(ssn);
    if (!h.valid() || h.pool() != 0) return false;
    const Entity *e = world_.registry.get(h);
    if (!e) return false;
    const Entity *held = world_.registry.get(e->mounted_child);
    if (!held) return false;
    return held->group_id == group;
}

bool EntityCommands::group_holding_group(int holder_group, int held_group) const {
    // [orig: TriggerGroup_AnyMemberHoldingItemGroup @0x43c870 — walk pool 0,
    // gate ItemTypeIndex(+0x1C) != 0, first member of holder_group whose
    // mountedChild's commandGroup == held_group]
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(holder_group), members);
    for (EntityHandle h : members) {
        if (h.pool() != 0) continue;
        const Entity *e = world_.registry.get(h);
        if (!e || e->item_id == 0) continue;
        const Entity *held = world_.registry.get(e->mounted_child);
        if (held && held->group_id == held_group) return true;
    }
    return false;
}

bool EntityCommands::ssn_on_chain_of(uint16_t ssn, uint16_t target_ssn) const {
    // [orig: Entity_IsOnTopOfChain @0x4f19a0 — both resolved + ItemTypeIndex
    // gates; A's groundEntity(+0x28) chain, up to 3 hops, == B]
    // Retail's +0x28 is ONE carrier link that covers standing-on, seated-in,
    // and emplacement-child-of alike (a seated gunner's +0x28 is his seat
    // entity, the seat's +0x28 its hull). Our model splits those into
    // mount_target / emplacement_parent / ground_target, so the hop re-folds
    // them — without the fold, "the player rides the Stryker" (the 05TRcoop
    // convoy root trigger, Single/sub42 p1=10000) never evaluated true for a
    // seated player and the chain stayed dead.
    const auto carrier_of = [this](const Entity &e) -> const Entity * {
        if (e.mounted && e.mount_target.valid())
            return world_.registry.get(e.mount_target);
        if (e.emplacement_parent.valid())
            return world_.registry.get(e.emplacement_parent);
        return world_.registry.get(e.ground_target);
    };
    const Entity *a = world_.registry.get(resolve_ssn(ssn));
    const Entity *b_probe = world_.registry.get(resolve_ssn(target_ssn));
    if (!a || !b_probe || a->item_id == 0 || b_probe->item_id == 0) return false;
    const Entity *hop = carrier_of(*a);
    for (int i = 0; i < 3 && hop != nullptr; ++i) {
        if (hop == b_probe) return true;
        hop = carrier_of(*hop);
    }
    return false;
}

namespace {

// Shared resolve + gates + center distance for the 42-45 trigger family
// [orig: both entities resolved via EntityPool_FindByNetId @0x4f0a20, gated
// on ItemTypeIndex(+0x1C) != 0; float euclidean over the 16.16 centers with
// the 0x7FFF0000 overflow clamp — our float positions need no clamp].
bool trigger_pair_distance(const World &w, const EntityCommands &cmds,
                           uint16_t ssn_a, uint16_t ssn_b,
                           const Entity *&a, const Entity *&b, float &dist) {
    a = w.registry.get(cmds.resolve_ssn(ssn_a));
    b = w.registry.get(cmds.resolve_ssn(ssn_b));
    if (a == nullptr || b == nullptr || a->item_id == 0 || b->item_id == 0)
        return false;
    const float dx = a->position.x - b->position.x;
    const float dy = a->position.y - b->position.y;
    const float dz = a->position.z - b->position.z;
    dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    return true;
}

} // namespace

bool EntityCommands::ssn_within_distance(uint16_t ssn, uint16_t target_ssn,
                                         int32_t meters) const {
    // [orig: Entity_CheckProximity @0x4f14c0 — dist <= p3, RAW positive]
    const Entity *a = nullptr;
    const Entity *b = nullptr;
    float dist = 0.0f;
    if (!trigger_pair_distance(world_, *this, ssn, target_ssn, a, b, dist))
        return false;
    return dist <= static_cast<float>(meters);
}

namespace {

// The +0x1FC LOS endpoint: position plus the host-stamped model bbox center,
// added RAW (unrotated) — an unstamped center leaves the raw position, like
// retail's zeroed pool memory. [orig: rayStart = entity[1..3] +
// entity[127..129] @0x4f1880..0x4f18c5 (sub 45) / @0x4f1728..0x4f176f
// (sub 44); the center writer Entity_InitFromModel @0x40df1e..0x40e018]
void los_offset_point(const Entity &e, int32_t out[3]) {
    out[0] = to_fixed(e.position.x + e.bbox_center.x);
    out[1] = to_fixed(e.position.y + e.bbox_center.y);
    out[2] = to_fixed(e.position.z + e.bbox_center.z);
}

} // namespace

bool EntityCommands::ssn_los_clear_within(uint16_t ssn, uint16_t target_ssn,
                                          int32_t meters) const {
    // [orig: Entity_CheckLineOfSightInRange @0x4f15e0 — center distance gate,
    // then a radius-0 ray between the +0x1FC bbox-center offset points;
    // <= 20 u uses the entity-aware walker @0x53b130, above it
    // terrain/sectors @0x539910. Our port rays through the one modeled LOS
    // seam (that walker split stays a tracked stand-in, §3b).]
    const Entity *a = nullptr;
    const Entity *b = nullptr;
    float dist = 0.0f;
    if (!trigger_pair_distance(world_, *this, ssn, target_ssn, a, b, dist))
        return false;
    if (dist > static_cast<float>(meters)) return false;
    if (!world_.ai) return true; // no AI/physics wired: the clear-ray default
    int32_t pa[3];
    los_offset_point(*a, pa);
    int32_t pb[3];
    los_offset_point(*b, pb);
    return world_.ai->line_of_sight_clear(world_, pa, pb,
                                          resolve_ssn(ssn), resolve_ssn(target_ssn));
}

bool EntityCommands::ssn_sees_within(uint16_t ssn, uint16_t target_ssn,
                                     int32_t meters) const {
    // [orig: Entity_CheckLineOfSight @0x4f17c0 — range, ray, AND bearing all
    // computed over the +0x1FC bbox-center offset points (deltas
    // @0x4f18cd..0x4f18dd), then the facing cone:
    // |wrap32(-yaw(+0x10) - int(atan2(dy, dx) * -(2^31/pi)))| <= 0x15555540
    // (30.0 deg), int32 wrap = shortest arc]
    const Entity *a = world_.registry.get(resolve_ssn(ssn));
    const Entity *b_ent = world_.registry.get(resolve_ssn(target_ssn));
    if (a == nullptr || b_ent == nullptr ||
        a->item_id == 0 || b_ent->item_id == 0)
        return false;
    const double fdx =
            (static_cast<double>(b_ent->position.x) + b_ent->bbox_center.x) -
            (static_cast<double>(a->position.x) + a->bbox_center.x);
    const double fdy =
            (static_cast<double>(b_ent->position.y) + b_ent->bbox_center.y) -
            (static_cast<double>(a->position.y) + a->bbox_center.y);
    const double fdz =
            (static_cast<double>(b_ent->position.z) + b_ent->bbox_center.z) -
            (static_cast<double>(a->position.z) + a->bbox_center.z);
    if (std::sqrt(fdx * fdx + fdy * fdy + fdz * fdz) >
        static_cast<double>(meters))
        return false;
    if (world_.ai) {
        int32_t pa[3];
        los_offset_point(*a, pa);
        int32_t pb[3];
        los_offset_point(*b_ent, pb);
        if (!world_.ai->line_of_sight_clear(world_, pa, pb, resolve_ssn(ssn),
                                            resolve_ssn(target_ssn)))
            return false;
    }
    // Retail truncates toward zero (_ftol2_sse) over the NEGATED scale
    // -(2^31/pi); the sign folds out under the cdq-abs below, but the
    // truncation is load-bearing (llround here would drift 1 BAM32 LSB on
    // half of all bearings). [orig: fpatan -> fmul dbl_7C57B8
    // (-683565275.5764316) -> _ftol2_sse @0x4f195f-0x4f196f]
    const int32_t bearing_neg = static_cast<int32_t>(
            std::atan2(fdy, fdx) * -683565275.5764316);
    const int32_t heading = a->veh.yaw_seeded
            ? a->veh.yaw_bam
            : bam_heading_from_mission_yaw_deg(static_cast<double>(a->yaw));
    // diff = wrap32(-yaw - trunc(atan2 * -(2^31/pi))) [orig: neg ecx; sub
    // ecx, eax @0x4f1977-0x4f1979] == wrap32(bearing - yaw); |.| equalizes.
    const int32_t diff = static_cast<int32_t>(
            static_cast<uint32_t>(-heading) - static_cast<uint32_t>(bearing_neg));
    // Retail's cdq/xor/sub abs: INT_MIN stays negative, so a target EXACTLY
    // 180.0 deg astern satisfies the signed <= — a witnessed quirk, carried.
    const uint32_t mask = static_cast<uint32_t>(diff >> 31);
    const int32_t adiff =
            static_cast<int32_t>((static_cast<uint32_t>(diff) ^ mask) - mask);
    return adiff <= 0x15555540; // 30.0000 deg in BAM32
}

// A scripted group kill has to reach the WIRE, not just zero the health. Retail
// never fans deaths from the damage pass: every motor's per-entity update carries
// the edge `Health <= 0 && (Flags & 2) == 0` and calls Entity_CheckAndProcessDeath
// there, so ANY writer of zero health — a bullet, or this action — is noticed and
// notified. The killer rides on the VICTIM (entity+704, read by
// BuildDeathNotifyPayload), which is why retail's own baseline capture shows its
// scripted kills as `killerSource=0`: the script never stamps that field. Ours
// leaves the killer handle unset for the same reason, and the burst matches.
//
// SHAPE NOTE: raising the death here rather than from a health<=0 sweep in the
// motor is narrower than the original — a future health-zeroing path would have
// to remember to do the same. Converging on the sweep is worth doing when the
// death path is next opened up; it needs the killer moved onto the entity first.
// [orig: the edge @0x4bfxxx (org1) / @0x4b73xx (org2) -> Entity_CheckAndProcessDeath
//  @0x51b550 -> BuildDeathNotifyPayload @0x5036e0, send_mask 0x90]
static void raise_scripted_death(World &world, Entity &e, EntityHandle h) {
    RoundDeath d;
    d.victim = h;
    d.victim_handle = h.packed;
    // killer_handle stays at its default: retail's unstamped entity+704.
    d.killer_handle = 0;
    world.round_sim.deaths.push_back(d);
    e.alive = false;
    e.health = 0;
}

int EntityCommands::kill_group(int group) {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (!e) continue;
        // Only the LIVING cross the edge — retail's `(Flags & 2) == 0` half. A
        // group killed twice must not notify twice.
        if (e->health > 0 && (e->flags & kEntityFlagDead) == 0)
            raise_scripted_death(world_, *e, h);
        else { e->alive = false; e->health = 0; }
        ++n;
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
        if (!e) continue;
        // Setting a group to zero health is a kill by another name, and retail's
        // motor edge cannot tell the two apart — it only sees the zero. Same
        // notify, same unstamped killer.
        if (hp <= 0 && e->health > 0 && (e->flags & kEntityFlagDead) == 0) {
            raise_scripted_death(world_, *e, h);
            ++n;
            continue;
        }
        e->health = hp;
        e->alive = hp > 0;
        ++n;
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

// Seat-type filter for a selection mode. #564 retired the shared copy of this
// predicate along with its own seat-selection path; find_best_seat below is the
// child-emplacement scan [orig: Entity_FindBestSeatSlot @0x4351f0] and still
// needs it, so it lives here as a file-local helper.
static bool seat_allowed_for_mode(SeatType type, SeatSelectionMode mode) {
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

int EntityCommands::find_best_seat(const Entity &target, EntityHandle occupant,
                                   SeatSelectionMode mode,
                                   EntityHandle *out_owner) const {
    // [orig: Entity_FindBestSeatSlot @0x4351f0] lowest weight wins; skip None/taken
    // seats. The original walks the vehicle AND ITS CHILDREN ("Searches through bone
    // slots of a vehicle entity and its children"): childCount/childArray come off the
    // carrier, index -1 is the carrier itself and 0..n-1 are its children, and two
    // rules apply only to children — `ctrlx`/`drvrx` are SKIPPED on a child
    // (`if (entityPtr != vehicleEntity) goto ...`), and a child `sitex` is weighted
    // 0x2000000 instead of 0x200000, i.e. worst of all.
    //
    // This retires the "child-entity traversal is deferred" residual. It is the
    // PRECONDITION for the emplaced body state: 00TRg's vehicles carry NO Gunner seat
    // of their own (1302/1303/1305 are [pass N, ctrl 1, GUN 0] with attach 1/3/1) —
    // every gun position is an addeweap CHILD (1892/1902/1988/1991/1993, GUN 1 each).
    // Scanning only the parent could never reach one, so anim 67 kEmplaced sat at 0.0%
    // against retail's 21.3%. With children in scope the Gunner weight (0x20000) beats
    // the parent's passenger seats (0x200000) 16:1, which is how retail fills its guns.
    int best = -1;
    int32_t best_weight = 65536000; // [orig: bestWeight init sentinel]
    if (out_owner != nullptr) *out_owner = target.handle;

    auto scan = [&](const Entity &owner, bool is_child) {
        for (int i = 0; i < static_cast<int>(owner.seats.size()); ++i) {
            const Seat &s = owner.seats[i];
            if (s.type == SeatType::None) continue;           // [orig: boneIdx != 0]
            // A child's control/driver bones are not seats of this vehicle.
            // [orig: the `entityPtr != vehicleEntity` skips on ctrlx and drvrx]
            if (is_child && (s.type == SeatType::Controller || s.type == SeatType::Driver))
                continue;
            if (!seat_allowed_for_mode(s.type, mode)) continue;
            if (s.occupant.valid() && s.occupant != occupant) // [orig: owner==0xFFFF || owner==self]
                continue;
            int32_t w;
            switch (s.type) {
                case SeatType::Controller:
                case SeatType::Driver:    w = 0x2000;   break; // [orig: case 2/5]
                case SeatType::Passenger:
                    w = is_child ? 0x2000000 : 0x200000;       // [orig: case 1 + the child bump]
                    break;
                case SeatType::Gunner:
                default:                  w = 0x20000;  break; // [orig: default (UseGun)]
            }
            if (w < best_weight) {
                best = i;
                best_weight = w;
                if (out_owner != nullptr) *out_owner = owner.handle;
            }
        }
    };

    scan(target, /*is_child=*/false);
    // The children. Retail keeps an explicit child array on the carrier; our link is
    // the child's own emplacement_parent stamped at promote, so the walk is a registry
    // scan rather than an array index. Declared divergence: same set, different
    // traversal. [orig: childArray = carrier->pad_1ba[2], childCount = pad_1ba[6]]
    world_.registry.for_each([&](const Entity &e) {
        if (e.emplacement_parent != target.handle) return;
        scan(e, /*is_child=*/true);
    });
    return best;
}

bool EntityCommands::mount(uint16_t occupant_ssn, uint16_t target_ssn, SeatSelectionMode mode) {
    // [orig: WacScript_TryMountEntityToVehicle @0x4f70f0] resolve both; reject already-mounted /
    // seatless; pick the best seat; write both sides; pose now.
    EntityHandle oh = resolve_ssn(occupant_ssn);
    EntityHandle th = resolve_ssn(target_ssn);
    Entity *occ = world_.registry.get(oh);
    Entity *tgt = world_.registry.get(th);
    if (!occ || !tgt || occ->mounted) return false;
    VehicleSeatSelection selection;
    if (!find_best_vehicle_seat(world_, th, oh, selection, mode)) return false;
    return attach_to_vehicle_seat(world_, oh, selection);
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

// WAC `ssnrelease` -- the RELEASE half of the AI boarding order, and the reason a
// transported squad ever gets out again. [orig: WacCmd_SsnRelease @0x4f7420]
//
//   if ( !v3 || !v3->ItemTypeIndex || !v3->parentEntity ) return 0;
//   Entity_DetachFromVehicleIfServer(v3);
//   if ( v3->aiRuntime ) { aiRuntime[37] = 0; aiRuntime[35] = 0; }
//
// It is the exact twin of the `ssn2ssn` setter (WacCmd_SsnToSsn @0x4f7330, which arms
// aiRuntime[37]=125 + [38]=target + [36]=carrier and zeroes thinkCooldown). Retail
// has NO arrival-driven unload anywhere -- all 20 Entity_DetachFromVehicleIfServer
// call sites are death/damage, a waypoint redirect, destroy, or spawn reset -- so
// THIS script command is how a mission disembarks a transported AI. Without it the
// occupant rides to the destination and then sits at command 125 forever, which is
// exactly what our 00TRg probe showed: 12 permanently-mounted AI, every one at
// wp=125, seven of them having driven ~700 u and then stopped dead.
//
// Clearing [35] (the has-route flag) as well as [37] is witnessed and load-bearing:
// leaving the route flag set would keep the stale board route live after the detach.
bool EntityCommands::release_boarding_command(uint16_t occupant_ssn) {
    const EntityHandle oh = resolve_ssn(occupant_ssn);
    Entity *occ = world_.registry.get(oh);
    // [orig: the !ItemTypeIndex and !parentEntity rejects] -- a release only applies
    // to a real item entity that is actually riding something.
    if (!occ || occ->item_type == 0 || !occ->mounted) return false;
    dismount(occupant_ssn); // [orig: Entity_DetachFromVehicleIfServer]
    if (world_.ai) {
        if (AiEntity *ae = world_.ai->for_handle(oh)) {
            ae->slot.f[37] = 0; // [orig: aiRuntime[37] = 0 — clear the board command]
            ae->slot.f[35] = 0; // [orig: aiRuntime[35] = 0 — clear the has-route flag]
        }
    }
    return true;
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
        VehicleSeatSelection selection;
        if (!find_best_vehicle_seat(world_, e.handle, oh, selection)) return;
        const double dx = e.position.x - p.x, dy = e.position.y - p.y, dz = e.position.z - p.z;
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= kMountRadius * kMountRadius && d2 < best_d2) { best_d2 = d2; best = e.handle; }
    });
    const Entity *tgt = world_.registry.get(best);
    if (!tgt) return false;
    return mount(occupant_ssn, tgt->net_id);
}

bool EntityCommands::dismount(uint16_t occupant_ssn) {
    return entity_detach_from_vehicle(world_, resolve_ssn(occupant_ssn));
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

namespace {

// The ChangeAI command family's per-entity alert-byte arms: sub 5 -> red(2),
// 22 -> yellow(1), 6 -> green(0) on the controller alert byte the
// SingleAtRed/YellowAlert triggers read. Reached per-SSN from ChangeSingleAI
// and per-member from the group/area fans, exactly like the original's
// per-entity dispatch. [orig: Entity_ApplyCommand @0x43ab60 cases 5/22/6
// @0x43ac2d/0x43ac8d/0x43acfd]
void apply_alert_command_byte(AiEntity &ae, int sub_type) {
    switch (sub_type) {
        case 5: ae.slot.bytes()[AiSlot::kAlertByte] = 2; break;
        case 22: ae.slot.bytes()[AiSlot::kAlertByte] = 1; break;
        case 6: ae.slot.bytes()[AiSlot::kAlertByte] = 0; break;
        default: break;
    }
}

// The BRAIN half of the alert subs: retail queues AIEvent {type 6, level}
// beside the controller-byte write whenever the entity carries an AI
// component; the brain applies it at dispatch (ai_handle_command case 6 —
// the forced-2-on-change store + the combat-state push).
// [orig: Entity_ApplyCommand case 5 @0x43ac59..0x43ac77 / case 0x16
//  @0x43acc4..0x43ace2 / case 6 @0x43ad34..0x43ad4e -> AIEvent_QueueEntry
//  @0x455da0 -> AI_HandleCommand case 6 @0x4657a6..0x465816]
void queue_alert_brain_event(AiSystem &sys, AiEntity &ae, int sub_type) {
    int level = 0;
    switch (sub_type) {
        case 5: level = 2; break;
        case 22: level = 1; break;
        case 6: level = 0; break;
        default: return;
    }
    AiEventEntry ev{};
    ev.f[0] = 6;
    ev.f[1] = 9 | (static_cast<int>(&ae - sys.at(0)) << 16);
    ev.set_timer(0.0f);
    ev.f[3] = level;
    sys.events.queue(ev);
}

} // namespace

bool EntityCommands::apply_ai_command(uint16_t ssn, int sub_type, int32_t p2, int32_t p3, int32_t p4) {
    if (!world_.ai) return false;
    AiEntity *ae = world_.ai->for_handle(resolve_ssn(ssn));
    if (!ae) return false;
    apply_alert_command_byte(*ae, sub_type);
    queue_alert_brain_event(*world_.ai, *ae, sub_type);
    ai_apply_command(ae->brain, sub_type, p2, p3, p4);
    return true;
}

// BMS action 27, PARTICLE_EFFECT [orig: EventAction_Dispatch case 0x1B @0x4542e0 ->
// EventAction_SpawnParticleEffect (ex sub_4540E0) @0x4540e0]. Retail walks POOL 3, matches `def type == 6088` and
// `entity[167] == param1`, and spawns one emitter per match at the entity's position,
// caching the handle at entity[115].
//
// entity[167] is the WP_NUMBER, not a team: the kong banner guesses "team", but
// 00TRg's four 6088 markers all have NO team byte (0) while their wp_numbers are
// 1/2/3/4 -- exactly the four params its events 12-15 pass. Matching on team would
// fire nothing.
//
// UNPORTED, declared: the emitter descriptor itself (effect_desc[0..13], the rope-trail
// style, the Entity_ClearOwnerSessionIfMatches owner callback) and the entity[115]
// handle cache. We raise one shell effect per match carrying the marker's position and
// leave the emitter style to the presenter; nothing here invents a particle type.
int EntityCommands::spawn_marker_particle_effects(int32_t wp_number) {
    int fired = 0;
    const size_t capacity = world_.registry.pool_capacity(3);
    for (size_t slot = 0; slot < capacity; ++slot) {
        const Entity *e = world_.registry.get(EntityHandle::make(3, static_cast<int>(slot)));
        if (e == nullptr) continue;
        if (e->item_id != kParticleEffectMarkerTypeId) continue; // def type 6088
        if (e->wp_number != wp_number) continue;
        world_.effects.push({"particle_effect", static_cast<int32_t>(to_fixed(e->position.x)),
                             static_cast<int32_t>(to_fixed(e->position.y)),
                             static_cast<int32_t>(to_fixed(e->position.z)), wp_number,
                             std::string()});
        ++fired;
    }
    return fired;
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
        if (ae) {
            apply_alert_command_byte(*ae, sub_type);
            queue_alert_brain_event(*world_.ai, *ae, sub_type);
            ai_apply_command(ae->brain, sub_type, p2, p3, p4);
            ++n;
        }
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
        if (ae) {
            apply_alert_command_byte(*ae, sub_type);
            queue_alert_brain_event(*world_.ai, *ae, sub_type);
            ai_apply_command(ae->brain, sub_type, p2, p3, p4);
            ++n;
        }
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
    // [orig: WacCmd_Quake (ex sub_4ED4C0) — Env_QuakeTicks = 6 * value]
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

void World::run_logic_tick(bool is_authority, TickPhase phase,
                           LogicTickPerf *perf) {
    if (perf != nullptr) *perf = {};
    uint64_t phase_start = perf != nullptr ? io::perf_now_us() : 0;
    // [orig: WacScript_AdvanceTick refreshes the per-tick local-player cache via
    // WacScript_CacheLocalPlayerState @0x4f5780 at the top of the tick, before the
    // script evaluators read it. Deferred: the mission sim has no local-player avatar
    // yet, so `cached` stays host-populated and the WAC near-* builtins read it as-is.]
    TickContext ctx;
    ctx.world = this;
    ctx.logic_tick = logic_tick;
    ctx.is_authority = is_authority;
    ctx.phase = phase;
    const bool pre_mission = phase == TickPhase::PreMission;
    const bool gameplay = phase == TickPhase::Gameplay;
    logic_authority = is_authority;
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
    if (perf != nullptr) {
        const uint64_t now = io::perf_now_us();
        perf->setup_us = now - phase_start;
        phase_start = now;
    }
    // The system loop runs on BOTH the authoritative host and a non-authority client
    // (the original client also runs a tick): each system self-gates on
    // ctx.is_authority. WacSystem / BmsEventSystem early-out on a client (scripting is
    // host-only; the in-match C2S drain is host-only too, owned by Server_TickUpdate, not an
    // ISystem); AiSystem on a client simulates ONLY the
    // local player (the §5.38 entity==local-player branch) and leaves every other
    // entity to the replicated wire state. [orig: the client tick still steps the
    // local player's infantry motor; Server_TickUpdate / Game_ProcessMainFrame.]
    if (phase != TickPhase::PreRound) {
        for (ISystem *s : systems_) {
            const uint64_t system_start =
                    perf != nullptr ? io::perf_now_us() : 0;
            AiTickPerf ai_perf;
            if (perf != nullptr && s == ai)
                ai->tick_profiled(*this, ctx, &ai_perf);
            else
                s->tick(*this, ctx);
            if (perf != nullptr) {
                const uint64_t elapsed = io::perf_now_us() - system_start;
                if (s == ai) {
                    perf->ai_us += elapsed;
                    perf->ai_reactions_us += ai_perf.reactions_us;
                    perf->ai_collision_tables_us += ai_perf.collision_tables_us;
                    perf->ai_entities_us += ai_perf.entities_us;
                    perf->ai_infantry_entities_us += ai_perf.infantry_entities_us;
                    perf->ai_infantry_remote_us += ai_perf.infantry_remote_us;
                    perf->ai_infantry_combat_us += ai_perf.infantry_combat_us;
                    perf->ai_infantry_animation_us += ai_perf.infantry_animation_us;
                    perf->ai_infantry_collision_us += ai_perf.infantry_collision_us;
                    perf->ai_infantry_collision_contacts_us +=
                            ai_perf.infantry_collision_contacts_us;
                    perf->ai_infantry_collision_repulsion_us +=
                            ai_perf.infantry_collision_repulsion_us;
                    perf->ai_infantry_collision_ground_us +=
                            ai_perf.infantry_collision_ground_us;
                    perf->ai_other_entities_us += ai_perf.other_entities_us;
                    perf->ai_authority_vehicles_us += ai_perf.authority_vehicles_us;
                    perf->ai_vehicle_scan_us += ai_perf.vehicle_scan_us;
                    perf->ai_vehicle_motors_us += ai_perf.vehicle_motors_us;
                    perf->ai_vehicle_riders_us += ai_perf.vehicle_riders_us;
                    perf->ai_client_vehicles_us += ai_perf.client_vehicles_us;
                    perf->ai_events_us += ai_perf.events_us;
                } else {
                    perf->scripts_us += elapsed;
                }
            }
        }
        if (perf != nullptr) phase_start = io::perf_now_us();
        pose_emplacement_attachments(*this, perf);
        // Static attachment poses can change after AI collision queries. The
        // projectile/destruction half of the tick starts a fresh matrix-view
        // epoch so it never inherits a pre-attachment target transform.
        if (collision != nullptr) collision->reset_query_view_cache();
    }
    if (perf != nullptr) {
        const uint64_t now = io::perf_now_us();
        perf->attachments_us = now - phase_start;
        phase_start = now;
    }
    // Entity_UpdateAllEntities walks pool 1 before the projectile pool. That
    // prevents a newly converted charge from losing an arm-delay tick and lets
    // claymore shrapnel fly later in its detonation frame [orig:
    // Entity_UpdatePool1Slot @0x4b8dd0 -> Weapon_UpdateAllProjectiles @0x4ec020].
    // These presentation events describe only the current authoritative tick.
    if (is_authority && gameplay) {
        throwables.events.clear();
        throwables.tick(*this, ai != nullptr ? ai->collision : nullptr, terrain);
    }
    if (perf != nullptr) {
        const uint64_t now = io::perf_now_us();
        perf->throwables_us = now - phase_start;
        phase_start = now;
    }
    // The global weapon-action pump follows the complete entity/system update and
    // precedes projectile stepping. This is where an AI UseGun nextAction write can
    // become a same-frame round.
    // [orig: Entity_UpdateAllEntities @0x52674b, then
    //  WeaponAction_ProcessAllEntities @0x526786]
    // WeaponAction_ProcessAllEntities is after the timer-gated entity update
    // and is itself ungated, so an already-queued action may advance during
    // PreRound even though its spawned projectile cannot move until gameplay.
    // PreMission remains outside the frame pump entirely.
    // [orig: Game_ProcessMainFrame @0x52672C..0x526786]
    if (phase != TickPhase::PreMission && ai != nullptr)
        ai->pump_mounted_weapon_slots(*this, logic_tick);
    if (perf != nullptr) {
        const uint64_t now = io::perf_now_us();
        perf->weapons_us = now - phase_start;
        phase_start = now;
    }
    // Live rounds step on the host and on an explicitly configured MP
    // non-authority client. The latter is the retail tag-2 visual re-sim path;
    // every decoded/predicted round carries VisualOnly through all consequence
    // sites, so only the host can mutate gameplay state. Do not infer a client
    // role from is_authority=false alone -- tests and pre-mission callers use it too.
    // [orig: Weapon_UpdateAllProjectiles @0x4ec020; §5.60]
    if (gameplay &&
        (is_authority || (mp_session && !projectile_authority)))
        round_sim.tick(*this, terrain, ai != nullptr ? ai->collision : nullptr);
    if (perf != nullptr) {
        const uint64_t now = io::perf_now_us();
        perf->projectiles_us = now - phase_start;
        phase_start = now;
    }
    if (gameplay &&
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
    if (perf != nullptr) {
        const uint64_t now = io::perf_now_us();
        perf->destruction_us = now - phase_start;
        phase_start = now;
    }
    // The waypoint current-selection pass, from the local player's position (the
    // original runs it in the client frame beside the player update; our SP host
    // is that client — the pure-client view is D-HUD-16). Position converts to
    // the original's 16.16 fixed compare space. [orig: Player_UpdatePerFrame
    // @0x4de5f7]
    if (is_authority && gameplay && !waypoints.empty()) {
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
    if (perf != nullptr)
        perf->housekeeping_us = io::perf_now_us() - phase_start;
}

// Shared semantic half of Server_ProcessRoundEnd @0x5164f0. The authority
// transport emits the per-recipient wire transaction from this frozen result.
void World::process_round_end(int32_t winning_team) {
    if (!match.finish(winning_team, *this)) return; // double-run guard + frozen board [orig: @0x516502/@0x516528]
    // Match::finish applies the winner marker and builds the immutable board in
    // the original pre-send order [orig: GameEvent_ProcessScoring @0x52f550;
    // Server_BuildEndOfRoundScoreboard @0x508f30]. The authority net tail sends,
    // per active slot in state 6: S2C 0x61 round-end marker (4 zero bytes)
    // @0x516790, S2C 0x1D
    // scoreboard header [u8 winner][s16 score0][s16 score1][u8 draw][s8 myEntryIndex]
    // (EndRoundScoreboard_SerializeHeader @0x505280) @0x516839, CNetPlayer_SetGameState(11)
    // @0x516846, slot state 6->7 @0x51685e; then the per-team round-win counters for
    // game types 0x10000/65537/65540 @0x5168a0 and the MP-only 2790-tick linger
    // @0x5166c4 (drained by Server_TickUpdate -> exit reason 3 / the client frame ->
    // reason 4; SP never drains it — the epilog owns the SP exit).
    // Match::finish sets the sole outcome latch before the network/presentation
    // tails, matching retail's double-run guard without copying its global.
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
    s.match = match;
    s.spawn_waves = spawn_waves;
    s.zone_capture_state = zone_capture_state;
    s.spawn_cycle_counter = spawn_cycle_counter;
    s.logic_tick = logic_tick;
    s.preround_delay_seconds = preround_delay_seconds;
    s.prng16_state = prng16_state;
    s.crt_rand_state = crt_rand.state;
    s.local_player = cached.local_player;
    return s;
}

void World::restore(const Snapshot &s) {
    registry.restore_from(s.registry);
    vars = s.vars;
    wac_values = s.wac_values;
    env = s.env;
    network_env = s.network_env;
    match = s.match;
    spawn_waves = s.spawn_waves;
    zone_capture_state = s.zone_capture_state;
    spawn_cycle_counter = s.spawn_cycle_counter;
    logic_tick = s.logic_tick;
    preround_delay_seconds = s.preround_delay_seconds;
    prng16_state = s.prng16_state;
    crt_rand.state = s.crt_rand_state;
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
    scars.reset();
    terrain_scorches.reset();
    destruction = DestructionEvents{};
    // The baseline copy above restores the configured rules, roster, clock,
    // stats, and outcome together. This matters for SP-as-listen-server: its
    // host player and game type already exist when the play-start snapshot is
    // sealed, and reset must not reconstruct them through another seam.
    kill_stats = MissionKillStats{};
    load_systems(); // systems re-init their per-mission state
    if (collision != nullptr) collision->refresh_after_registry_change(*this);
    registry.for_each([&](const Entity &vehicle) {
        if (vehicle.primary_occupant.valid())
            emit_vehicle_control_started(*this, vehicle);
    });
}


void count_mission_units(World &world) {
    // Players (the +534 byte; our player_class != 0) count into the separate
    // player bucket the panel never draws; everything else with team >= 2 and
    // a non-zero items.def unit-class byte is one enemy unit. The original's
    // vehicle(3/4)/aircraft(9)/infantry split is fold-consumed as the total.
    // [orig: Score_ClassifyEntityForCounts @0x4fd070 — player @0x4fd074,
    //  team gate @0x4fd08d, def+0x196 gate @0x4fd09f, total @0x4fd0a8;
    //  driven over both pools by Score_CountMissionSubgoalsAndUnits @0x509e13..0x509e4a]
    int32_t total = 0;
    world.registry.for_each([&](const Entity &e) {
        if (e.player_class != 0) return;
        if (e.team < 2) return;
        if (e.item_unit_type == 0) return;
        ++total;
    });
    world.kill_stats.enemy_unit_total = total;
}

int32_t count_defined_subgoals(const World &world) {
    // The leading run of authored win conditions before the first 0 or 0xFF
    // entry, at most eight. [orig: Score_CountMissionSubgoalsAndUnits @0x509dc2..0x509dd1 scanning
    //  the header win-condition ids; the count lands in dword_C8468C]
    int32_t count = 0;
    for (int slot = 1; slot <= 8; ++slot) {
        const uint8_t id = world.subgoals.win_text_ids[slot];
        if (id == 0 || id == 0xFF) break;
        ++count;
    }
    return count;
}

} // namespace opennova::world
