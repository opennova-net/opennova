#include "world/player_input.h"

#include "world/ai.h"

namespace opennova::world {

PlayerBodyInput pack_player_body_input(const PlayerInput &in) {
    PlayerBodyInput body;
    body.look_heading = in.look_heading;
    body.look_pitch = in.look_pitch;
    // Lean keys map to MoveOrder bits 6/7 [orig: Player_PackInputStateToEntity
    // @0x4df708-0x4df741 — g_inputFlags 0x2000 -> 0x40, 0x4000 -> 0x80].
    body.lean_left = in.lean_left;
    body.lean_right = in.lean_right;
    body.stance = in.prone ? InfantryState::Stance::kProne
                : (in.crouch ? InfantryState::Stance::kCrouch : InfantryState::Stance::kStand);
    body.jump = in.jump;
    // [orig: Player_PackInputStateToEntity @0x4df450] F/B/L/R bits collapse to an
    // 8-way move_direction_index plus a moving bit. There is no heading offset here.
    if (in.forward) body.direction_bits |= 1;
    if (in.back) body.direction_bits |= 2;
    if (in.left) body.direction_bits |= 4;
    if (in.right) body.direction_bits |= 8;

    bool moving = body.direction_bits != 0;
    int index = 0;
    switch (body.direction_bits) {
        case 1:
        case 13:
            index = 0;
            break;
        case 2:
        case 14:
            index = 4;
            break;
        case 3:
        case 12:
        case 15:
            moving = false;
            index = 0;
            break;
        case 4:
        case 7:
            index = 2;
            break;
        case 5:
            index = 1;
            break;
        case 6:
            index = 3;
            break;
        case 8:
        case 11:
            index = 6;
            break;
        case 9:
            index = 7;
            break;
        case 10:
            index = 5;
            break;
        default:
            moving = false;
            index = 0;
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
