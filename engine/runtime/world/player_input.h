// Per-frame player input -> the player body state: the portable, Godot-free port of the
// engine's input front end (docs/net/novaworld-net-re.md section 5.38). The host's input layer
// fills a PlayerInput each frame; its held keys fold into the retail input-flag word, and the
// pack maps that word onto the player entity's infantry state as the original packs
// g_InputFlags into entity+0x12C.
#pragma once

#include <cstdint>

#include <runtime/world/infantry.h>

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
    // Lean/roll keys (catalog ids 6/7, Q/E): g_InputFlags 0x2000/0x4000 -> MoveOrder
    // bits 6/7 [orig: cases 148/147 @0x4e10d9/@0x4e10c8; packer @0x4df708-0x4df741].
    bool lean_left = false;
    bool lean_right = false;
    bool crouch = false;
    bool prone = false;
    bool jump = false;
    bool free_look = false;
    bool look_up = false;
    bool look_down = false;
    bool turn_left = false;
    bool turn_right = false;
	int8_t analog_throttle = 0; // signed local aircraft collective axis (+0x133)
	int32_t look_heading = 0; // absolute facing (entity Yaw@+0x10), BAM32
	int32_t look_pitch = 0;   // absolute look pitch (entity Pitch@+0x14), BAM32
};

// The raw input-flag word: each held key's action handler ORs its bit in once per frame
// [orig: g_InputFlags @0xB3B728; Input_HandleActionBinding_0 @0x4e0420, the held-key rows
// re-queued every frame by Input_ProcessAnalogBindings @0x4995f0].
enum InputFlag : uint32_t {
    kInputFlagForward = 0x0002,   // case 152 [orig: @0x4e0cbe]
    kInputFlagBack = 0x0004,      // case 151 [orig: @0x4e0c6e]
    kInputFlagLeft = 0x0008,      // case 156 [orig: @0x4e1041]
    kInputFlagRight = 0x0010,     // case 157 [orig: @0x4e1066]
    kInputFlagLookUp = 0x0020,    // case 155 [orig: @0x4e0f83]
    kInputFlagLookDown = 0x0040,  // case 154 [orig: @0x4e0eb5]
    kInputFlagTurnLeft = 0x0100,  // case 158 [orig: @0x4e108c]
    kInputFlagTurnRight = 0x0200, // case 159 [orig: @0x4e10b7]
    kInputFlagJump = 0x1000,      // case 153 [orig: @0x4e0d66]
    kInputFlagLeanLeft = 0x2000,  // case 148 [orig: @0x4e10f1]
    kInputFlagLeanRight = 0x4000, // case 147 [orig: @0x4e10c8]
    kInputFlagFreeLook = 0x8000,  // case 176 [orig: @0x4e0cd5]
};
// The four direction keys' bits -- what the pack collapses to the F/B/L/R word and
// the per-frame binocular suppression tests [orig: @0x4df48e..0x4df4a5;
// Player_UpdatePerFrame `test byte ptr g_InputFlags, 1Eh` @0x4de3ae].
inline constexpr uint32_t kInputFlagDirectionMask =
    kInputFlagForward | kInputFlagBack | kInputFlagLeft | kInputFlagRight;

// The bits this frame's held keys OR into the word. The look-up/down handlers refuse while
// the AbsorbPitch seat flag answers, so those bits never enter the word [orig: cases 154/155
// -- Entity_CheckWeaponSeatFlags(EquippedSlot, 0x10000) @0x4e0ea5 / @0x4e0f73].
uint32_t player_input_flags(const PlayerInput &in, bool absorb_pitch);

// g_InputFlags and its last-packed copy g_InputFlagsPrev. Each frame clears the bits the
// previous pack reported, then ORs in the held keys; the pack reads the word, saves it as
// `prev` and zeroes it. Bits stay sticky between packs, so a key tapped on any frame of a
// send-holdoff window reaches the next pack, while a bit the previous pack reported survives
// only on the frame it is still held.
// [orig: Input_ProcessFrame @0x49d520 -- `g_InputFlags &= ~g_InputFlagsPrev` @0x49d541,
//  ahead of the handler dispatch; Player_PackInputStateToEntity @0x4df450 --
//  `g_InputFlagsPrev = g_InputFlags` @0x4df904, `g_InputFlags = 0` @0x4df909]
struct PlayerInputFlags {
    uint32_t flags = 0; // g_InputFlags @0xB3B728
    uint32_t prev = 0;  // g_InputFlagsPrev @0xB3B72C

    void fold(uint32_t held) { flags = (flags & ~prev) | held; }
    // The word this frame's fold will leave, without folding.
    uint32_t folded(uint32_t held) const { return (flags & ~prev) | held; }
    void clear_after_pack() {
        prev = flags;
        flags = 0;
    }
};

// The player-body packet that mirrors the raw input deposit at entity+0x12C without
// pretending it is an NPC route order. This is the seam future MP input sources should feed.
struct PlayerBodyInput {
    // The packed F/B/L/R word and the 8-way move index it collapses to
    // [orig: Player_PackInputStateToEntity @0x4df450].
    enum DirectionBit : uint8_t { kDirForward = 1, kDirBack = 2, kDirLeft = 4, kDirRight = 8 };
    enum MoveDir : int {
        kMoveForward = 0, kMoveForwardLeft = 1, kMoveLeft = 2, kMoveBackLeft = 3,
        kMoveBack = 4, kMoveBackRight = 5, kMoveRight = 6, kMoveForwardRight = 7,
    };
    uint8_t direction_bits = 0;       // DirectionBit flags
    bool moving = false;
    int move_dir_index = 0;           // 0 F, 1 F+L, 2 L, 3 B+L, 4 B, 5 B+R, 6 R, 7 F+R
    bool lean_left = false;           // MoveOrder bit 6 [orig: @0x4df71f]
    bool lean_right = false;          // MoveOrder bit 7 [orig: @0x4df737]
    InfantryState::Stance stance = InfantryState::Stance::kStand;
    bool jump = false;
    bool free_look = false;
    bool look_up = false;
    bool look_down = false;
    bool turn_left = false;
    bool turn_right = false;
    int32_t look_heading = 0;
    int32_t look_pitch = 0;
};

// The pack: the input-flag word's keys plus the stance latches and the look `in` carries.
// [orig: Player_PackInputStateToEntity @0x4df450]
PlayerBodyInput pack_player_body_input(uint32_t input_flags, const PlayerInput &in);
void apply_player_body_input(AiEntity &e, const PlayerBodyInput &body);

} // namespace opennova::world
