// Local-player on-foot motor state (entity class org2).
//
// [orig: Entity_UpdateInfantryPhysics @0x4b40e0, continuation @0x4b434f; dispatched
//  per class via g_EntityClassPhysicsTable @0x82abc8 row "org2"]. The on-foot player
// is the INPUT-DRIVEN twin of the AI infantry motor (org1 @0x4b9910): it shares the
// same entity layout, the same ground/fall-damage helper
// (Entity_ProcessCollisionAndPlatformPhysics @0x4b2bd0), and the same ROOT-MOTION
// locomotion (the move-direction selects a locomotion clip; the clip's root track is
// the speed — there is no scalar speed field). It differs in: gravity cadence
// (-208/tick x1 vs the AI's -416/2-ticks x2, net-equal), the command source (player
// input via PlayerInputCommand vs AI waypoints), and a player-only superstructure
// (mouse-look + view-pitch clamp, sway, recoil, first-person camera). Full RE record:
// docs/world/player-controller-re.md.
//
// PlayerState colocates with the AiEntity carrier (like InfantryState); when
// player.active the entity is driven by AiSystem::tick_player.
#ifndef OPENNOVA_WORLD_PLAYER_H
#define OPENNOVA_WORLD_PLAYER_H

#include <cstdint>

#include "world/infantry.h"     // anim_state ids, RootMotionFrame, IRootMotionSource
#include "world/player_input.h"

namespace opennova::world {

// First-person eye transform, ENGINE frame (Z-up, BAM angles), 16.16 position.
// Computed at the tail of tick_player [orig: Player_UpdateFirstPersonCamera @0x4dd380];
// the host reads it via the C ABI and converts to the Godot frame at the present
// boundary (the single-sourced Z-up->Y-up + 90-yaw handoff, never re-derived here).
struct PlayerCamera {
    int32_t eye[3] = {};   // engine-frame eye position
    int32_t view_yaw = 0;  // BAM
    int32_t view_pitch = 0;
    int32_t view_roll = 0;
    bool valid = false;
};

struct PlayerState {
    bool active = false;        // routed through the player motor (org2 / local player)

    // Resolved intent for this tick, latched by the host before the sim advances.
    PlayerInputCommand input;

    // Stance: 0 stand / 1 crouch / 2 prone, derived from the input each tick. [orig: the
    // entity+0x12C 0x100/0x200 bits; server-authoritative in retail (msg 0x1D), applied
    // locally here for single-player — a tracked divergence.] Feeds the pitch clamp + camera.
    int stance = 0;

    // Anim machine — shares the clip set + root-motion path with the AI motor (org1
    // and org2 both resolve clips through the same 252-name table + AnimMap; org2's
    // locomotion clip is chosen from the move-direction/stance instead of a gait).
    int anim_state = anim_state::kIdle;   // entity[175]
    int anim_pending = 0;                 // entity[174]
    int32_t clip_phase = 0;               // channel playhead (ticks)
    uint32_t last_events = 0;             // last frame's .bad event bits

    // Velocity accumulator (entity+152/+156/+160; vel[2] = vel_z entity+0xA0 in the
    // gravity path).
    int32_t vel[3] = {};
    int32_t ground_cache = 0;             // resampled like the infantry path
    bool ground_cache_valid = false;

    // First-person camera output (filled by compute_player_camera at the tick tail).
    PlayerCamera camera;
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_PLAYER_H
