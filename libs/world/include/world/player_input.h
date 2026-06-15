// Local-player input command — the de-Godot'd movement/look intent the on-foot
// player motor consumes each tick.
//
// The on-foot player is entity class org2 [orig: Entity_UpdateInfantryPhysics
// @0x4b40e0], the input-driven twin of the AI infantry motor (org1 @0x4b9910).
// In the original, the input layer packs human input into the entity each tick:
// discrete intent accumulates in g_inputFlags (dword_B3B728), then
// Player_PackInputStateToEntity @0x4df450 folds it into the move-flags DWORD
// entity+0x12C (bits0-2 = 8-way move index, 0x8 is_moving, 0x100 crouch, 0x200
// prone, + feature bits) and the four analog axis bytes entity+0x130..0x133;
// the action handler Input_HandleActionBinding_0 @0x4e0420 applies the scaled
// mouse delta to the view eulers (yaw entity+0x10, pitch entity+0x14).
//
// We model the *resolved intent* the motor consumes (the host owns the device
// layer): the 8-way index + flags + the per-tick BAM look deltas. See
// docs/world/player-controller-re.md.
#ifndef OPENNOVA_WORLD_PLAYER_INPUT_H
#define OPENNOVA_WORLD_PLAYER_INPUT_H

#include <cstdint>

namespace opennova::world {

// 8-way move-direction index, body-relative (clockwise from forward). Matches the
// index Player_PackInputStateToEntity @0x4df450 packs into entity+0x12C bits0-2.
// [orig: the dir-nibble switch @0x4df656]
enum PlayerMoveDir : int {
    kPlayerMoveForward = 0,
    kPlayerMoveForwardLeft = 1,
    kPlayerMoveStrafeLeft = 2,
    kPlayerMoveBackLeft = 3,
    kPlayerMoveBack = 4,
    kPlayerMoveBackRight = 5,
    kPlayerMoveStrafeRight = 6,
    kPlayerMoveForwardRight = 7,
};

// One tick of resolved local-player intent. Latched on the player entity by the
// host (NovaSimulation::set_player_input) before the sim advances; consumed by
// AiSystem::tick_player.
struct PlayerInputCommand {
    // Locomotion intent. move_dir is the 8-way index (valid only when is_moving).
    // [orig: entity+0x12C bits0-2 / bit3]
    int move_dir = kPlayerMoveForward;
    bool is_moving = false;

    // Stance (mutually exclusive; both false = stand). [orig: entity+0x12C 0x100 /
    // 0x200; server-authoritative round trip in retail, applied locally here]
    bool crouch = false;
    bool prone = false;

    // Action flags. [orig: entity+0x12C feature bits, action ids @0x4e0420]
    bool fire = false;       // act177 Fire Weapon
    bool aim = false;        // ADS / scope toggle state (dword_B76478)
    bool jump = false;       // act170 Jump
    bool use = false;        // act56 UseItem
    bool reload = false;     // act361 Reload
    bool lean_left = false;  // act147 Lean/Roll Left
    bool lean_right = false; // act153 Lean/Roll Right

    // Mouse-look deltas for this tick, already scaled to BAM (engine binary angle:
    // deg * 2^32/360). The host applies the witnessed gain
    // scaled = (raw_delta * (sensitivity << 11) + 0x8000) >> 16 [orig:
    // Input_ProcessMouseAxisBindings @0x499680] and feeds the result here.
    //   yaw   += look_yaw_delta   (no per-tick turn-rate clamp on foot)
    //   pitch += look_pitch_delta (clamped +/-80 deg, +/-40 deg when crouched)
    // [orig: act166/167 yaw, act164/165 pitch, clamp @0x4e0d44 / 0x4e0ffe]
    int32_t look_yaw_delta = 0;
    int32_t look_pitch_delta = 0;
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_PLAYER_INPUT_H
