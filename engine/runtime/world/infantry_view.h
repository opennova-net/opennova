// The player body's (org2) view phase: one kernel over one body's words, which the authority's
// body tick (infantry_org2_view_tick) and a client's decoded row (replication::row_org2_view_phase)
// both run. The contract is in infantry_view.cpp.
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

} // namespace opennova::world
