// Per-frame player input → the infantry move order — the portable, Godot-free port of the
// engine's input front end (docs/net/novaworld-net-re.md §5.38). The host's input layer
// (the Godot controller, or a remote client) fills a PlayerInput; this maps it onto the
// player entity's infantry state exactly as the original packs g_inputFlags into the entity.
#ifndef OPENNOVA_WORLD_PLAYER_INPUT_H
#define OPENNOVA_WORLD_PLAYER_INPUT_H

#include <cstdint>

namespace opennova::world {

struct AiEntity;

// The movement keys + look the engine reads each frame. `look_heading` is the absolute
// body/aim heading (BAM32) the caller accumulates from the mouse — the analog of
// Input_ProcessMouseAxisBindings @0x499680 (mouse × sensitivity → Yaw).
struct PlayerInput {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool run = false;          // alerted/run gait (vs walk)
    int32_t look_heading = 0;  // absolute facing, BAM32
};

// Faithful [orig: Player_PackInputStateToEntity @0x4df450]: collapse the F/B/L/R combo into
// the witnessed 8-way move_direction_index (0..7) and deposit it — plus the look heading —
// as the player's infantry move order (move_mode / target_dist / move_offset / target_heading).
// Opposing keys cancel (idle). Index → BAM move-direction offset relative to facing:
//   0 forward, 1 fwd+left, 2 left, 3 back+left, 4 back, 5 back+right, 6 right, 7 fwd+right.
void apply_player_move_order(AiEntity &e, const PlayerInput &in);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_PLAYER_INPUT_H
