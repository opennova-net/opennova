#include <runtime/world/player_input.h>

#include <runtime/world/ai.h>

namespace opennova::world {

uint32_t player_input_flags(const PlayerInput &in, bool absorb_pitch) {
    uint32_t held = 0;
    if (in.forward) held |= kInputFlagForward;
    if (in.back) held |= kInputFlagBack;
    if (in.left) held |= kInputFlagLeft;
    if (in.right) held |= kInputFlagRight;
    if (in.look_up && !absorb_pitch) held |= kInputFlagLookUp;
    if (in.look_down && !absorb_pitch) held |= kInputFlagLookDown;
    if (in.turn_left) held |= kInputFlagTurnLeft;
    if (in.turn_right) held |= kInputFlagTurnRight;
    if (in.jump) held |= kInputFlagJump;
    if (in.lean_left) held |= kInputFlagLeanLeft;
    if (in.lean_right) held |= kInputFlagLeanRight;
    if (in.free_look) held |= kInputFlagFreeLook;
    return held;
}

PlayerBodyInput pack_player_body_input(uint32_t input_flags, const PlayerInput &in) {
    PlayerBodyInput body;
    body.look_heading = in.look_heading;
    body.look_pitch = in.look_pitch;
    // The input word's bits onto the MoveOrder word [orig: Player_PackInputStateToEntity
    // @0x4df6d7..0x4df790 -- 0x8000 -> 0x10 free look, 0x1000 -> 0x20 jump, 0x2000/0x4000
    // -> 0x40/0x80 lean, 0x100/0x200 -> 0x1000/0x2000 turn, 0x20/0x40 -> 0x4000/0x8000
    // look up/down]. The stance bits are the prone/crouch latches, not input bits
    // [orig: g_PlayerStanceProneLatch @0x4df6ae / g_PlayerStanceCrouchLatch @0x4df6c6].
    body.lean_left = (input_flags & kInputFlagLeanLeft) != 0;
    body.lean_right = (input_flags & kInputFlagLeanRight) != 0;
    body.stance = in.prone ? InfantryState::Stance::kProne
                : (in.crouch ? InfantryState::Stance::kCrouch : InfantryState::Stance::kStand);
    body.jump = (input_flags & kInputFlagJump) != 0;
    body.free_look = (input_flags & kInputFlagFreeLook) != 0;
    body.look_up = (input_flags & kInputFlagLookUp) != 0;
    body.look_down = (input_flags & kInputFlagLookDown) != 0;
    body.turn_left = (input_flags & kInputFlagTurnLeft) != 0;
    body.turn_right = (input_flags & kInputFlagTurnRight) != 0;
    // [orig: Player_PackInputStateToEntity @0x4df48a..0x4df4a5] the 0x2/0x4/0x8/0x10
    // input bits collapse to the F/B/L/R word, then to an 8-way move_direction_index
    // plus a moving bit. There is no heading offset here.
    using B = PlayerBodyInput;
    if (input_flags & kInputFlagForward) body.direction_bits |= B::kDirForward;
    if (input_flags & kInputFlagBack) body.direction_bits |= B::kDirBack;
    if (input_flags & kInputFlagLeft) body.direction_bits |= B::kDirLeft;
    if (input_flags & kInputFlagRight) body.direction_bits |= B::kDirRight;

    bool moving = body.direction_bits != 0;
    int index = B::kMoveForward;
    switch (body.direction_bits) {
        case B::kDirForward:
        case B::kDirForward | B::kDirLeft | B::kDirRight:
            index = B::kMoveForward;
            break;
        case B::kDirBack:
        case B::kDirBack | B::kDirLeft | B::kDirRight:
            index = B::kMoveBack;
            break;
        case B::kDirForward | B::kDirBack:
        case B::kDirLeft | B::kDirRight:
        case B::kDirForward | B::kDirBack | B::kDirLeft | B::kDirRight:
            moving = false;
            index = B::kMoveForward;
            break;
        case B::kDirLeft:
        case B::kDirForward | B::kDirBack | B::kDirLeft:
            index = B::kMoveLeft;
            break;
        case B::kDirForward | B::kDirLeft:
            index = B::kMoveForwardLeft;
            break;
        case B::kDirBack | B::kDirLeft:
            index = B::kMoveBackLeft;
            break;
        case B::kDirRight:
        case B::kDirForward | B::kDirBack | B::kDirRight:
            index = B::kMoveRight;
            break;
        case B::kDirForward | B::kDirRight:
            index = B::kMoveForwardRight;
            break;
        case B::kDirBack | B::kDirRight:
            index = B::kMoveBackRight;
            break;
        default:
            moving = false;
            index = B::kMoveForward;
            break;
    }
    body.moving = moving;
    body.move_dir_index = index;
    return body;
}

void apply_player_body_input(AiEntity &e, const PlayerBodyInput &body) {
    InfantryState &inf = e.inf;

    // The local player writes look yaw/pitch directly into the entity inputs.
    // [orig: Input_ProcessMouseAxisBindings @0x499680 /
    // Input_HandleActionBinding_0 @0x4e1330]
    inf.target_heading = body.look_heading;
    inf.look_pitch = body.look_pitch;

    // Player stance bits are packed into entity+0x12C. The player-body motor tests
    // prone (0x100) before crouch (0x200). [orig: 0x4b59ce / 0x4b5b13]
    inf.stance = body.stance;
    if (body.jump) inf.jump_requested = true;
    inf.free_look = body.free_look;
    inf.view_input_bits = (body.turn_left ? 0x1000 : 0) |
        (body.turn_right ? 0x2000 : 0) | (body.look_up ? 0x4000 : 0) |
        (body.look_down ? 0x8000 : 0);
    inf.jump_held = body.jump; // the packed MoveOrder bit 0x20 the wire mirror reads
    inf.player_moving = body.moving;
    inf.player_move_dir_index = body.move_dir_index;
    // Lean inputs (MoveOrder bits 6/7): consumed by the lean-angle ramp and the prone
    // roll-anim selection [orig: ramp @0x4b7dbf/@0x4b7dd6; anims 41/42 @0x4b731b].
    inf.lean_left = body.lean_left;
    inf.lean_right = body.lean_right;

    // Clear NPC route fields so the local player cannot accidentally route through org1
    // waypoint/scripted-order semantics.
    inf.move_mode = 0;
    inf.target_dist = 0;
    inf.move_dir_index = 0;
}

} // namespace opennova::world
