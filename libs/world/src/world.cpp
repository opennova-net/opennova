#include "world/world.h"

#include <cmath>
#include <utility>
#include <vector>

#include "world/ai.h" // AiSystem / AiEntity / ai_apply_command — the AI-change command target

namespace opennova::world {

// Max distance (mission units) for mount_best's nearest-emplacement search — the proximity
// proxy for the occupant-model+144 vehicle link the original resolves through the entity
// hierarchy. A manned-gun soldier is placed on/next to its gun, so this is generous.
static constexpr double kMountRadius = 20.0;

static void emit_vehicle_control(World &world, const char *kind, uint16_t target_net_id,
                                 int32_t target_bms_id, uint32_t target_spawn_origin) {
    Effect effect;
    effect.kind = kind;
    effect.a = static_cast<int32_t>(target_net_id);
    effect.b = target_bms_id;
    effect.c = static_cast<int32_t>(target_spawn_origin);
    world.effects.push(std::move(effect));
}

void emit_vehicle_control_started(World &world, const Entity &vehicle) {
    emit_vehicle_control(world, "vehicle_control_started", vehicle.net_id, vehicle.bms_id,
                         vehicle.spawn_origin);
}

void emit_vehicle_control_stopped(World &world, const Entity &vehicle) {
    emit_vehicle_control_stopped(world, vehicle.net_id, vehicle.bms_id, vehicle.spawn_origin);
}

void emit_vehicle_control_stopped(World &world, uint16_t target_net_id,
                                  int32_t target_bms_id, uint32_t target_spawn_origin) {
    emit_vehicle_control(world, "vehicle_control_stopped", target_net_id, target_bms_id,
                         target_spawn_origin);
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
    vehicle.primary_occupant = EntityHandle{};
    emit_vehicle_control_stopped(world, vehicle);
    return true;
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

void pose_mounted_occupant(Entity &occ, const Entity &vehicle, const Seat &seat) {
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
    occ.yaw = (seat.type == SeatType::Gunner)
                      ? static_cast<int16_t>(vehicle.yaw - seat.yaw_offset)
                      : static_cast<int16_t>(vehicle.yaw + seat.yaw_offset);
    occ.pitch = vehicle.pitch;
    occ.roll = vehicle.roll;
}

// ----------------------------------------------------------------------------
// EntityCommands — the shared Entity_* primitive layer.
// In this foundational core, commands mutate the clean Entity model directly.
// (Replication routing through World::net is the deferred MP seam; LocalSink
// makes single-player run everything locally.)
// ----------------------------------------------------------------------------

bool EntityCommands::kill_ssn(uint16_t ssn) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->alive = false;
    e->health = 0;
    return true;
}

bool EntityCommands::remove_ssn(uint16_t ssn) {
    EntityHandle h = world_.registry.find_by_net_id(ssn);
    if (!world_.registry.get(h)) return false;
    world_.registry.despawn(h);
    return true;
}

bool EntityCommands::set_ssn_hp(uint16_t ssn, int32_t hp) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->health = hp;
    e->alive = hp > 0;
    return true;
}

bool EntityCommands::add_ssn_hp(uint16_t ssn, int32_t delta) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->health += delta;
    if (e->health < 0) e->health = 0;
    e->alive = e->health > 0;
    return true;
}

bool EntityCommands::set_ssn_waypoint(uint16_t ssn, int32_t wp) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->waypoint_id = static_cast<uint8_t>(wp);
    e->wp_number = 0;
    return true;
}

bool EntityCommands::set_ssn_alert(uint16_t ssn, int32_t state) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->alert_state = static_cast<uint8_t>(state);
    return true;
}

bool EntityCommands::set_ssn_target(uint16_t ssn, uint16_t target) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->ai_target = target;
    return true;
}

bool EntityCommands::set_ssn_move_speed(uint16_t ssn, int32_t kph) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->move_speed_kph = kph;
    return true;
}

bool EntityCommands::set_ssn_engage_min(uint16_t ssn, int32_t v) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->engage_min = v;
    return true;
}

bool EntityCommands::set_ssn_engage_max(uint16_t ssn, int32_t v) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->engage_max = v;
    return true;
}

bool EntityCommands::set_ssn_attack_max(uint16_t ssn, int32_t v) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->attack_max = v;
    return true;
}

bool EntityCommands::set_ssn_anim(uint16_t ssn, int32_t anim_slot) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->body_anim_slot = anim_slot; // the present-pass clip channel (not the +0x374 selector)
    return true;
}

bool EntityCommands::set_ssn_hidden(uint16_t ssn, bool hidden) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->hidden = hidden;
    return true;
}

bool EntityCommands::set_ssn_held(uint16_t ssn, bool held) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->held = held;
    return true;
}

bool EntityCommands::set_ssn_disabled(uint16_t ssn, bool disabled) {
    Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    if (!e) return false;
    e->disabled = disabled;
    return true;
}

bool EntityCommands::ssn_exists(uint16_t ssn) const {
    return world_.registry.get(world_.registry.find_by_net_id(ssn)) != nullptr;
}

bool EntityCommands::ssn_alive(uint16_t ssn) const {
    const Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    return e != nullptr && e->alive;
}

bool EntityCommands::ssn_dead(uint16_t ssn) const {
    const Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
    return e != nullptr && !e->alive;
}

bool EntityCommands::ssn_in_area(uint16_t ssn, int area_id) const {
    const Entity *e = world_.registry.get(world_.registry.find_by_net_id(ssn));
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

int EntityCommands::group_to_waypoint(int group, int32_t wp) {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (e) { e->waypoint_id = static_cast<uint8_t>(wp); e->wp_number = 0; ++n; }
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
    EntityHandle oh = world_.registry.find_by_net_id(occupant_ssn);
    EntityHandle th = world_.registry.find_by_net_id(target_ssn);
    Entity *occ = world_.registry.get(oh);
    Entity *tgt = world_.registry.get(th);
    if (!occ || !tgt) return false;
    if (occ->mounted) return false;       // [orig: entity->pad8[8] set -> return 0]
    if (tgt->seats.empty()) return false; // [orig: no model+144 vehicle / no seats]
    const int seat_idx = find_best_seat(*tgt, oh, mode);
    if (seat_idx < 0) return false;
    Seat &s = tgt->seats[seat_idx];
    s.occupant = oh;                                       // [orig: vehicle[400+2*slot] = handle]
    occ->mount_target = th;                                // [orig: occupant+364]
    occ->mount_target_net_id = tgt->net_id;
    occ->mount_target_bms_id = tgt->bms_id;
    occ->mount_target_spawn_origin = tgt->spawn_origin;
    occ->mount_seat = static_cast<int8_t>(seat_idx);       // [orig: occupant+360]
    occ->mount_type = s.type;
    occ->mounted = true;                                   // [orig: occupant+36 |= 0x40]
    pose_mounted_occupant(*occ, *tgt, s);
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
    EntityHandle oh = world_.registry.find_by_net_id(occupant_ssn);
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
    Entity *occ = world_.registry.get(world_.registry.find_by_net_id(occupant_ssn));
    if (!occ || !occ->mounted) return false;
    const bool claim_capable_seat = occ->mount_type != SeatType::Passenger &&
                                    occ->mount_type != SeatType::None;
    const uint16_t target_net_id = occ->mount_target_net_id;
    const int32_t target_bms_id = occ->mount_target_bms_id;
    const uint32_t target_spawn_origin = occ->mount_target_spawn_origin;
    const EntityHandle oh = occ->handle;
    Entity *tgt = world_.registry.get(occ->mount_target);
    if (tgt && occ->mount_seat >= 0 && occ->mount_seat < static_cast<int>(tgt->seats.size()))
        tgt->seats[occ->mount_seat].occupant = EntityHandle{}; // [orig: vehicle[400+2*slot]=0xFFFF]
    occ->mounted = false;
    occ->mount_target = EntityHandle{};
    occ->mount_target_net_id = 0;
    occ->mount_target_bms_id = 0;
    occ->mount_target_spawn_origin = 0;
    occ->mount_seat = -1;
    occ->mount_type = SeatType::None;
    if (tgt != nullptr) {
        vehicle_release_primary_occupant(world_, *tgt, oh); // [orig: +368 leg @0x4356e9]
    } else if (claim_capable_seat) {
        // The vehicle entity is already gone; its stored identity carries the stop so the
        // host tears the presentation down (host cleanup — the claimant check is
        // unavailable, and a spurious stop is idempotent downstream).
        emit_vehicle_control_stopped(world_, target_net_id, target_bms_id,
                                     target_spawn_origin);
    }
    return true;
}

uint16_t EntityCommands::find_mounted_on(uint16_t target_ssn) const {
    // [orig: Vehicle_HasEnemyOccupant @0x4359f0] first occupant riding target_ssn, else 0.
    EntityHandle th = world_.registry.find_by_net_id(target_ssn);
    if (!th.valid()) return 0;
    uint16_t result = 0;
    world_.registry.for_each([&](const Entity &e) {
        if (result == 0 && e.mounted && e.mount_target == th) result = e.net_id;
    });
    return result;
}

// --- AI command (the AI-change action family) ---
// [orig: Entity_ApplyCommand @0x43ab60.] Resolve the target's brain through World::ai and
// apply the sub-type command in-engine. No AI system / no brain -> no-op.

bool EntityCommands::apply_ai_command(uint16_t ssn, int sub_type, int32_t p2, int32_t p3, int32_t p4) {
    if (!world_.ai) return false;
    AiEntity *ae = world_.ai->for_handle(world_.registry.find_by_net_id(ssn));
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

void World::add_system(ISystem *sys) {
    if (sys) systems_.push_back(sys);
}

void World::load_systems() {
    for (ISystem *s : systems_) s->on_load(*this);
}

void World::run_logic_tick(bool is_authority, bool pre_mission) {
    // [orig: sub_4F81A0 refreshes the per-tick local-player cache via
    // WacScript_CacheLocalPlayerState @0x4f5780 at the top of the tick, before the
    // script evaluators read it. Deferred: the mission sim has no local-player avatar
    // yet, so `cached` stays host-populated and the WAC near-* builtins read it as-is.]
    TickContext ctx;
    ctx.world = this;
    ctx.logic_tick = logic_tick;
    ctx.is_authority = is_authority;
    ctx.pre_mission = pre_mission;
    // The system loop runs on BOTH the authoritative host and a non-authority client
    // (the original client also runs a tick): each system self-gates on
    // ctx.is_authority. WacSystem / BmsEventSystem early-out on a client (scripting is
    // host-only; the in-match C2S drain is host-only too, owned by Server_TickUpdate, not an
    // ISystem); AiSystem on a client simulates ONLY the
    // local player (the §5.38 entity==local-player branch) and leaves every other
    // entity to the replicated wire state. [orig: the client tick still steps the
    // local player's infantry motor; Server_TickUpdate / Game_ProcessMainFrame.]
    for (ISystem *s : systems_) s->tick(*this, ctx);
    // Live rounds step inside the world frame, authority-only — the client's visual
    // round re-sim is not modeled here [orig: Entity_UpdateAllEntities ->
    // Weapon_UpdateAllProjectiles @0x4ec020; damage is authority-gated end-to-end,
    // §5.60]. Terrain is the host-wired sampler (AI grounding shares it).
    if (is_authority && !pre_mission) round_sim.tick(*this, terrain);
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
    s.env = env;
    s.logic_tick = logic_tick;
    return s;
}

void World::restore(const Snapshot &s) {
    registry = s.registry;
    vars = s.vars;
    env = s.env;
    logic_tick = s.logic_tick;
    effects.clear();
    round_sim.reset();
    load_systems(); // systems re-init their per-mission state
    registry.for_each([&](const Entity &vehicle) {
        if (vehicle.primary_occupant.valid())
            emit_vehicle_control_started(*this, vehicle);
    });
}

} // namespace opennova::world
