#include <runtime/world/player_input.h>

#include <runtime/world/ai.h>

namespace opennova::world {

PlayerBodyInput pack_player_body_input(const PlayerInput &in) {
    PlayerBodyInput body;
    body.look_heading = in.look_heading;
    body.look_pitch = in.look_pitch;
    // Lean keys map to MoveOrder bits 6/7 [orig: Player_PackInputStateToEntity
    // @0x4df708-0x4df741 — g_InputFlags 0x2000 -> 0x40, 0x4000 -> 0x80].
    body.lean_left = in.lean_left;
    body.lean_right = in.lean_right;
    body.stance = in.prone ? InfantryState::Stance::kProne
                : (in.crouch ? InfantryState::Stance::kCrouch : InfantryState::Stance::kStand);
    body.jump = in.jump;
    body.free_look = in.free_look;
    body.look_up = in.look_up;
    body.look_down = in.look_down;
    body.turn_left = in.turn_left;
    body.turn_right = in.turn_right;
    // [orig: Player_PackInputStateToEntity @0x4df450] F/B/L/R bits collapse to an
    // 8-way move_direction_index plus a moving bit. There is no heading offset here.
    using B = PlayerBodyInput;
    if (in.forward) body.direction_bits |= B::kDirForward;
    if (in.back) body.direction_bits |= B::kDirBack;
    if (in.left) body.direction_bits |= B::kDirLeft;
    if (in.right) body.direction_bits |= B::kDirRight;

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
    inf.jump_held = body.jump; // the level bit the wire mirror reads (0x20)
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
