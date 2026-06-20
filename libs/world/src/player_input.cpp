#include "world/player_input.h"

#include "world/ai.h"

namespace opennova::world {

void apply_player_move_order(AiEntity &e, const PlayerInput &in) {
    InfantryState &inf = e.inf;

    // Look → body facing (the body turns toward target_heading; the motor quarter-steps).
    // [orig: Input_ProcessMouseAxisBindings @0x499680 writes the entity Yaw, net-re §5.38]
    inf.target_heading = in.look_heading;

    // [orig: Player_PackInputStateToEntity @0x4df450] F/B/L/R combo → 8-way move index.
    int dir_bits = 0;
    if (in.forward) dir_bits |= 1;
    if (in.back) dir_bits |= 2;
    if (in.left) dir_bits |= 4;
    if (in.right) dir_bits |= 8;

    int index = -1; // -1 = not moving
    switch (dir_bits) {
        case 1:     index = 0; break; // forward
        case 1 | 4: index = 1; break; // forward + left
        case 4:     index = 2; break; // left
        case 2 | 4: index = 3; break; // back + left
        case 2:     index = 4; break; // back
        case 2 | 8: index = 5; break; // back + right
        case 8:     index = 6; break; // right
        case 1 | 8: index = 7; break; // forward + right
        default:    index = -1; break; // none / opposing keys cancel
    }

    if (index < 0) {
        inf.move_mode = 0;
        inf.target_dist = 0;
        inf.move_offset = 0;
    } else {
        inf.move_mode = 3;          // "move" → infantry_select picks the walk/run gait
        inf.target_dist = 0x10000;  // > 0 so the selector treats the order as moving (1.0 16.16)
        // index * 45° in BAM32 (2^32/8 = 0x20000000): the move direction relative to facing.
        inf.move_offset = static_cast<int32_t>(static_cast<uint32_t>(index) * 0x20000000u);
        inf.alert_timer = in.run ? 16 : 0; // nonzero → run gait
    }
}

} // namespace opennova::world
