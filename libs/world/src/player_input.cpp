#include "world/player_input.h"

#include "world/ai.h"

namespace opennova::world {

void apply_player_move_order(AiEntity &e, const PlayerInput &in) {
    InfantryState &inf = e.inf;

    // The local player writes look yaw/pitch directly into the entity inputs.
    // [orig: Input_ProcessMouseAxisBindings @0x499680 /
    // Input_HandleActionBinding_0 @0x4e1330]
    inf.target_heading = in.look_heading;
    inf.look_pitch = in.look_pitch;

    // Player stance bits are packed into entity+0x12C. The player-body motor tests
    // prone (0x100) before crouch (0x200). [orig: 0x4b59ce / 0x4b5b13]
    inf.stance = in.prone ? InfantryState::Stance::kProne
               : (in.crouch ? InfantryState::Stance::kCrouch : InfantryState::Stance::kStand);
    if (in.jump) inf.jump_requested = true;

    // [orig: Player_PackInputStateToEntity @0x4df450] F/B/L/R bits collapse to an
    // 8-way move_direction_index plus a moving bit. There is no heading offset here.
    int direction_bits = 0;
    if (in.forward) direction_bits |= 1;
    if (in.back) direction_bits |= 2;
    if (in.left) direction_bits |= 4;
    if (in.right) direction_bits |= 8;

    bool moving = direction_bits != 0;
    int index = 0;
    switch (direction_bits) {
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

    if (!moving) {
        inf.move_mode = 0;
        inf.target_dist = 0;
        inf.move_dir_index = 0;
    } else {
        inf.move_mode = 3;
        inf.target_dist = 0x10000;
        inf.move_dir_index = index;
    }

    // in.run is deliberately not mapped to InfantryState::alert_timer. That timer is
    // the AI alert source in Entity_UpdateInfantryAI; the player run path needs the
    // player-body state/flag path from IDA, not an AI shortcut.
    (void)in.run;
}

} // namespace opennova::world
