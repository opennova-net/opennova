#pragma once

#include <cstdint>

#include "world/entity.h"
#include "world/vehicle_mount.h" // SeatSelectionMode default args

// EntityCommands: the shared host-authoritative command layer. Split from
// the world.h umbrella (W3-7); World holds it by value and world.h
// re-includes this header.

namespace opennova::world {

class World;

// ----------------------------------------------------------------------------
// Shared entity-command primitive layer. Models the original Entity_* mutation
// functions (Entity_KillByNetId, Entity_SetWaypointByTeam, Entity_SetAlertByNetId,
// Entity_SetMoveSpeedKPH, ...) that BOTH EventAction_Dispatch and the WacScript_*
// handlers funnel through. WAC handlers and BMS actions both call these.
// ----------------------------------------------------------------------------
class EntityCommands {
public:
    explicit EntityCommands(World &world) : world_(world) {}

    // The script-facing local-player SSN [orig: the dfx2med player-slot
    // convention — the SP player entity carries 10000 as its net id].
    static constexpr uint16_t kLocalPlayerSsn = 10000;

    // Script SSN -> entity handle (the WAC SSN*/BMS Single resolve), including
    // the retail player mapping: SSN 10000 = the local player. Our player
    // entities carry net_id 0 (the wire is handle-based), so the mapping lives
    // here at the script seam. [orig: EntityPool_FindByNetId @0x4f0a20]
    EntityHandle resolve_ssn(uint16_t ssn) const;

    // --- entity (by net id) ---
    bool kill_ssn(uint16_t ssn);
    bool remove_ssn(uint16_t ssn);
    bool set_ssn_hp(uint16_t ssn, int32_t hp);
    bool add_ssn_hp(uint16_t ssn, int32_t delta);
    // `node < 0` selects the nearest node on the list (the two-argument WAC form);
    // BMS RedirectSingleTo carries an explicit node in param3.
    bool set_ssn_waypoint(uint16_t ssn, int32_t wp, int32_t node = -1);
    bool set_ssn_alert(uint16_t ssn, int32_t state);
    bool set_ssn_target(uint16_t ssn, uint16_t target);
    bool set_ssn_move_speed(uint16_t ssn, int32_t kph);
    bool set_ssn_engage_min(uint16_t ssn, int32_t v);
    bool set_ssn_engage_max(uint16_t ssn, int32_t v);
    bool set_ssn_attack_max(uint16_t ssn, int32_t v);
    bool set_ssn_anim(uint16_t ssn, int32_t anim_slot);
    bool set_ssn_hidden(uint16_t ssn, bool hidden);
    bool set_ssn_held(uint16_t ssn, bool held);
    bool set_ssn_disabled(uint16_t ssn, bool disabled);

    // --- queries ---
    bool ssn_exists(uint16_t ssn) const;
    bool ssn_alive(uint16_t ssn) const;
    bool ssn_dead(uint16_t ssn) const;
    bool ssn_in_area(uint16_t ssn, int area_id) const;
    // True only when the mission has at least one ACTIVE area trigger and the
    // local player's X/Y sits inside none of them — Z is ignored, and a world
    // with no local player (a serve-only host) reads as in-bounds. Feeds the
    // BMS player-AWOL counter. [orig: Entity_IsLocalPlayerOutOfBounds @0x439d40]
    bool local_player_out_of_bounds() const;

    // --- group (by group id) ---
    int kill_group(int group);          // returns members affected
    // `node < 0` selects the nearest node on the list; BMS RedirectGroupTo passes
    // its authored param3 here instead of silently replacing it with nearest.
    int group_to_waypoint(int group, int32_t wp, int32_t node = -1);
    int set_group_hp(int group, int32_t hp);
    int set_group_engage_min(int group, int32_t v);
    int set_group_engage_max(int group, int32_t v);
    int set_group_attack_max(int group, int32_t v);
    bool group_dead(int group) const;   // true if all members dead/absent
    bool group_alive(int group) const;  // true if any member alive

    // --- mount / emplacement (AttachToEmplaced) ---
    // [orig: Entity_FindBestSeatSlot @0x4351f0] Pick the best free seat on `target` for `occupant`:
    // skip None/taken seats, weight by type (driver/ctrl 0x2000 < gunner 0x20000 < passenger
    // 0x200000; lower wins), return its index or -1. Child-entity traversal is deferred — tracked.
    int find_best_seat(const Entity &target, EntityHandle occupant,
                       SeatSelectionMode mode = SeatSelectionMode::Any) const;
    // [orig: WacScript_TryMountEntityToVehicle @0x4f70f0] Attach occupant_ssn into target_ssn's best
    // free seat: reject if the occupant is already mounted or the target has no free seat; write both
    // sides + pose immediately. Returns false on any reject.
    bool mount(uint16_t occupant_ssn, uint16_t target_ssn,
               SeatSelectionMode mode = SeatSelectionMode::Any);
    // Port-side helper for authored "Goto SSN and board" commands 123/124/125, not a retail
    // symbol. Retail path: Entity_UpdateInfantryAI @0x4ba9ad -> Entity_FindBestSeatSlot
    // @0x4351f0 -> Entity_RequestVehicleAttach @0x4364a0. FindBestSeatSlot applies the rules:
    // 123 only accepts `sitex`, 124 rejects `ctrlx`, and 125 uses normal best-seat priority.
    bool mount_boarding_command(uint16_t occupant_ssn, uint16_t target_ssn, uint8_t command_id);
    // [orig: EventAction_Dispatch case 0x25 @0x4542e0] The BMS AttachToEmplaced entry: the action
    // carries ONLY the occupant SSN; the original finds the vehicle via the occupant model's +144
    // hierarchy link. We don't model that link, so the target is the nearest emplacement with a free
    // seat within kMountRadius (a tracked proximity proxy). Returns false if none.
    bool mount_best(uint16_t occupant_ssn);
    // [orig: Entity_DetachFromVehicle @0x4355f0] Free the occupant's seat + clear its mount ref.
    bool dismount(uint16_t occupant_ssn);
    // [orig: Vehicle_HasEnemyOccupant @0x4359f0] SSN of an entity riding target_ssn, else 0.
    uint16_t find_mounted_on(uint16_t target_ssn) const;

    // --- the BMS Player mount triggers (main type 7 subs 38-41) ---
    // All four resolve ssn, require a live local player, and test its mount/stand state
    // against the SSN entity, one carrier link deep. [orig: EventTrigger_EvaluateCondition
    // @0x453620 cat-7 subs 38-41 -> the four predicates @0x4f10d0/0x4f1260/0x4f1150/0x4f11e0
    // (renamed 2026-07-16: Entity_IsLocalPlayerSeatedOnSsn / StandingOnSsn / DrivingSsn /
    // OnGunOfSsn — the shipped IDB names were permuted misnomers).]
    // PLYRATTACHED: seated in ANY seat of the SSN (or of something the SSN carries).
    bool local_player_attached_to_ssn(uint16_t ssn) const;   // sub 38 @0x4f10d0
    // PLYRONSSN: STANDING on the SSN (ground/carrier reference), not seated.
    bool local_player_standing_on_ssn(uint16_t ssn) const;   // sub 39 @0x4f1260
    // PLYRDRIVING: seated on the SSN chain in a ctrlx/drvrx seat.
    bool local_player_driving_ssn(uint16_t ssn) const;       // sub 40 @0x4f1150
    // PLYRONGUN: seated on the SSN chain in the UseGun seat.
    bool local_player_on_gun_of_ssn(uint16_t ssn) const;     // sub 41 @0x4f11e0

    // --- AI command (the AI-change action family) ---
    // [orig: Entity_ApplyCommand @0x43ab60, reached from EventAction_Dispatch @0x4542e0
    // via Entity_HandleAlertStateEvent @0x43dee0.] Apply an AI sub-type command to the
    // target's AI component, reached through World::ai. p2/p3/p4 are the sub-type's slots
    // (e.g. PLAYPARTANIM: p2=channel, p3=play_type, p4=time). No-op (returns false / 0)
    // when there is no AI system or no brain for the target.
    bool apply_ai_command(uint16_t ssn, int sub_type, int32_t p2, int32_t p3, int32_t p4);
    int apply_group_ai_command(int group, int sub_type, int32_t p2, int32_t p3, int32_t p4);
    int apply_area_ai_command(int zone_area_id, int team, int sub_type,
                              int32_t p2, int32_t p3, int32_t p4);

    World &world() { return world_; }

private:
    World &world_;
};

} // namespace opennova::world
