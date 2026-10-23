// The player body's (org2) view phase and deck ride: kernels over one body's words, which the
// authority's body tick (infantry_org2_view_tick, infantry_follow_carrier) and a client's decoded
// row (replication::row_org2_view_phase, row_org2_ride) both run. The contracts are in
// infantry_view.cpp.
#pragma once

#include <cstdint>

namespace opennova::world {

class World;
struct AiEntity;
struct Entity;

// One body's view-phase words, retail's entity fields.
struct Org2ViewWords {
    int32_t &yaw;             // +0x10 (the local body's: its look accumulator, g_LocalPlayerLookYaw)
    int32_t &pitch;           // +0x14
    int32_t &body_heading;    // +0x8C
    int32_t (&leg_yaw)[2];    // +0x2D4 (the right chain) / +0x2D8 (the left)
    int32_t (&leg_target)[2]; // +0x2E4 / +0x2E8, their re-plant targets
    int32_t body_pitch;       // +0x90, savedLivePitch
};

// The phase's gates, as its body's pass reads them.
struct Org2ViewGates {
    uint32_t flags = 0;               // Flags (+0x24)
    int anim_state = 0;               // +0x2BC
    uint32_t tick = 0;                // the raw tick the prologue banks
    bool prone = false;               // the prologue's prone local, after its mount clear
    bool yaw_clamp_skipped = false;   // the mount's config is 3, or the ground a crashed vehicle
    bool pitch_clamp_skipped = false; // the mount is a crashed vehicle
};

// The phase itself (the contract in the TU).
void infantry_org2_view_phase(const Org2ViewWords &w, const Org2ViewGates &g);

// The prologue's prone local, which the phase's pitch limit reads: the MoveOrder's prone bit
// (`prone_order`) with none of Flags 0x10A000, cleared for a living body whose mount (+0x16C) has
// a def and for any body carrying Flags 0x40.
bool infantry_org2_prone_local(bool prone_order, uint32_t flags, bool living_mount_with_def);

// The authority's player body: its words and gates off its World entity, then the phase. A
// client's wire-owned body never runs it here (its replica row does).
void infantry_org2_view_tick(const World &world, AiEntity &e, const Entity *tick_entity,
                             uint32_t tick);

// Whether an entity is a vehicle (its def's type 1, Entity_IsVehicle's test) in its crash state
// (+0x2EC); false for none.
bool crashed_vehicle(const Entity *e);

// One body's deck-ride words, retail's entity fields.
struct Org2RideWords {
    int32_t &yaw;             // +0x10
    int32_t *look_yaw;        // g_LocalPlayerLookYaw: the local body's alone, else null
    int32_t &body_heading;    // +0x8C
    int32_t (&leg_yaw)[2];    // +0x2D4 / +0x2D8
    int32_t (&leg_target)[2]; // +0x2E4 / +0x2E8
    int32_t &body_pitch;      // +0x90, savedLivePitch
    int32_t &pitch_lag;       // +0x2E0
    int32_t &aim_pitch;       // +0x2D0
    int32_t &roll;            // +0x18
};

// The ride's turn of the body's words (the contract in the TU): `dyaw` is the ground's yaw delta,
// `pitch_d` / `roll_d` its pitch and roll deltas turned by the ground-vs-body yaw the caller took
// ahead of the Yaw add, `turret_mount` whether the body's mount (+0x16C) has an IsTurret def.
void infantry_org2_ride_turn(const Org2RideWords &w, int32_t dyaw, int32_t pitch_d, int32_t roll_d,
                             bool turret_mount);

// The driver's hold gate: the body is its ground's first occupant (+0x170), the ground's def is
// PlayerControl (attrib 0x40), the body's MoveOrder carries no free-look bit (0x10) and the ground
// is no crashed vehicle.
bool infantry_org2_ride_holds(bool first_occupant, uint32_t ground_attrib, uint32_t move_order,
                              bool ground_crashed);

// The hold: the Yaw and the local look take back what they held ahead of the ride, each turned by
// the ground's own ground's yaw delta (0 without one).
void infantry_org2_ride_hold(const Org2RideWords &w, int32_t yaw_before, int32_t look_before,
                             int32_t ground_ground_dyaw);

// The pitch lag's (+0x2E0) step: a thirty-second of it leaves the lag; the caller adds the
// returned step to the Pitch. The caller skips it while the ground is a crashed vehicle.
int32_t infantry_org2_pitch_lag_step(int32_t &pitch_lag);

} // namespace opennova::world
