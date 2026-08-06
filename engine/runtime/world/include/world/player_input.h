// Per-frame player input -> the player body state: the portable, Godot-free port of the
// engine's input front end (docs/net/novaworld-net-re.md section 5.38). The host's input layer
// fills a PlayerInput; this maps it onto the player entity's infantry state as the original
// packs g_inputFlags into entity+0x12C.
#ifndef OPENNOVA_WORLD_PLAYER_INPUT_H
#define OPENNOVA_WORLD_PLAYER_INPUT_H

#include <cstdint>

#include "world/infantry.h"

namespace opennova::world {

struct AiEntity;

// The movement keys + look the engine reads each frame. look_heading is the absolute
// body/aim heading (BAM32) the caller accumulates from the mouse, matching
// Input_ProcessMouseAxisBindings @0x499680. There is no run key in the original's
// catalog: running is the automatic forward-walk promotion in the body selection
// [orig: @0x4b729d], so no run bit exists here.
struct PlayerInput {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    // Lean/roll keys (catalog ids 6/7, Q/E): g_inputFlags 0x2000/0x4000 -> MoveOrder
    // bits 6/7 [orig: cases 148/147 @0x4e10d9/@0x4e10c8; packer @0x4df708-0x4df741].
    bool lean_left = false;
    bool lean_right = false;
    bool crouch = false;
    bool prone = false;
    bool jump = false;
    int32_t look_heading = 0; // absolute facing (entity Yaw@+0x10), BAM32
    int32_t look_pitch = 0;   // absolute look pitch (entity Pitch@+0x14), BAM32
};

// The player-body packet that mirrors the raw input deposit at entity+0x12C without
// pretending it is an NPC route order. This is the seam future MP input sources should feed.
struct PlayerBodyInput {
    uint8_t direction_bits = 0;       // F/B/L/R = 1/2/4/8
    bool moving = false;
    int move_dir_index = 0;           // 0 F, 1 F+L, 2 L, 3 B+L, 4 B, 5 B+R, 6 R, 7 F+R
    bool lean_left = false;           // MoveOrder bit 6 [orig: @0x4df71f]
    bool lean_right = false;          // MoveOrder bit 7 [orig: @0x4df737]
    InfantryState::Stance stance = InfantryState::Stance::kStand;
    bool jump = false;
    int32_t look_heading = 0;
    int32_t look_pitch = 0;
};

PlayerBodyInput pack_player_body_input(const PlayerInput &in);
void apply_player_body_input(AiEntity &e, const PlayerBodyInput &body);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_PLAYER_INPUT_H
