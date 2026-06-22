// Per-frame player input -> the infantry move order: the portable, Godot-free port of the
// engine's input front end (docs/net/novaworld-net-re.md section 5.38). The host's input layer
// fills a PlayerInput; this maps it onto the player entity's infantry state as the original
// packs g_inputFlags into entity+0x12C.
#ifndef OPENNOVA_WORLD_PLAYER_INPUT_H
#define OPENNOVA_WORLD_PLAYER_INPUT_H

#include <cstdint>

namespace opennova::world {

struct AiEntity;

// The movement keys + look the engine reads each frame. look_heading is the absolute
// body/aim heading (BAM32) the caller accumulates from the mouse, matching
// Input_ProcessMouseAxisBindings @0x499680.
struct PlayerInput {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool run = false;       // reserved until the IDA player-run path is ported
    bool crouch = false;
    bool prone = false;
    bool jump = false;
    int32_t look_heading = 0; // absolute facing (entity Yaw@+0x10), BAM32
    int32_t look_pitch = 0;   // absolute look pitch (entity Pitch@+0x14), BAM32
};

// Faithful [orig: Player_PackInputStateToEntity @0x4df450]: collapse the F/B/L/R combo into
// the witnessed 8-way move_direction_index (0..7) plus moving bit and deposit it, with the
// look heading, as the player's infantry move order (move_mode / target_dist /
// move_dir_index / target_heading). Opposing-key combinations follow the IDA switch.
// Index names match the input packer: 0 forward, 1 forward+left, 2 left, 3 back+left,
// 4 back, 5 back+right, 6 right, 7 forward+right.
void apply_player_move_order(AiEntity &e, const PlayerInput &in);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_PLAYER_INPUT_H
