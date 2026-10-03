// The death screen's spectator motor. While g_DeathScreenActive is up the
// local player's org2 body update hands its whole tick to the free-fly camera
// motor instead of simulating a body: in the chase or first-person sub-mode
// the local entity takes the spectate target's pose every tick, so every
// reader of the local pose (the camera, the radar viewer, the spinmap centre,
// the C2S 0x0C uplink) follows the target; in the free sub-mode the entity
// flies from its own MoveOrder word.
// [orig: Entity_UpdateInfantryPlayerBody @0x4b40f8..0x4b411a ->
//  Camera_UpdateFreeFly @0x4b2980]
#pragma once

#include <cstdint>

#include <runtime/world/entity.h>

namespace opennova::world {

class World;
struct AiEntity;

// The client-local spectate state the motor reads, stamped by the role from
// its replica runtime ahead of every entity update (the authority's own
// client and a joiner alike).
struct SpectatorMotorState {
	bool death_screen = false; // g_DeathScreenActive @0xA860EC
	int32_t submode = 0;       // dword_A860F0: 0 free, 1 chase, 2 first person
	// dword_A860F4. A target the world's own registry holds (the authority's
	// replica rows name its own entities) is read at motor time; a decoded
	// remote person (a joiner's, which has no registry entity) carries the
	// pose its row held when the role stamped it.
	bool has_target = false;
	EntityHandle target_entity;
	int32_t target_pos[3] = {0, 0, 0}; // 16.16, entity+4/+8/+12
	int32_t target_yaw = 0;            // BAM32, entity+0x10
	int32_t target_pitch = 0;          // entity+0x14
	int32_t target_roll = 0;           // entity+0x18
	// The fast-flight gate: the per-recipient SU flag (S2C 0x24 "SU n"). Its
	// alternative, bit 2 of the local cheat word dword_24C1930, has no writer
	// in the image (Game_StartMission stores the whole word 0 or 0x2000000
	// @0x525b25 / @0x525b5c; the toggles flip 0x100 / 0x400 / 0x800), so that
	// arm is dead. [orig: g_ScoreboardStatusSuffixEnabled @0xA85B49, read
	// @0x4b2a35; `test byte ptr dword_24C1930, 2` @0x4b2a42]
	bool su_flag = false;
	// The replica's count of death-screen rising edges last seated on the
	// local entity (spectator_seat_eye): the role's edge bookkeeping.
	uint32_t death_screen_opens_seen = 0;

	// The motor's follow arm: a sub-mode with a target [orig: @0x4b2994
	// `cmp dword_A860F0,0`, @0x4b29c7 `test eax,eax`].
	bool follows_target() const { return death_screen && submode != 0 && has_target; }
};

// The free-fly flight speeds, 16.16 units per second before the per-tick
// scale: the stock 10.0, and under the fast-flight gate 80.0, or 40.0 with
// the crouch bit, while the prone bit holds the stock speed.
// [orig: `mov [esp+var_8], 0A0000h` @0x4b29b4; the 0x500000 / 0x280000 pick
//  @0x4b2a4b..0x4b2a6b]
inline constexpr int32_t kSpectatorFlySpeed = 0xA0000;
inline constexpr int32_t kSpectatorFlySpeedFast = 0x500000;
inline constexpr int32_t kSpectatorFlySpeedFastCrouch = 0x280000;
// The yaw/pitch key step, BAM32 per tick [orig: `mov edx, 1FFFFFFh` @0x4b2b19].
inline constexpr int32_t kSpectatorTurnStep = 0x1FFFFFF;
// The eye the death screen's rising edge seats on the local entity (the
// CameraOffset the diverted body never restamps). [orig: NapiNPClientMsg_0x00A
//  @0x42ffb8..0x42ffc9 — CameraOffset = (0, 0, 0xD000)]
inline constexpr int32_t kSpectatorEyeZ = 0xD000;

// One Camera_UpdateFreeFly tick over the local player's body, reading
// world.spectator. The follow arm copies the target's Position, Yaw, Pitch
// and Roll (the orientation matrix at +0xB4 rides with them: the port derives
// it from the Euler triple wherever it is read); the free arm moves Position
// by the MoveOrder word and turns Yaw / Pitch by its look keys, the yaw step
// landing on the local look yaw too. The port's look yaw and pitch are the
// body's staged input copies (target_heading / look_pitch), so the follow arm
// stages the copied angles there as well, keeping the next tick's input
// mirror from reverting them.
void spectator_free_fly(World &world, AiEntity &local);

// The death screen's rising edge on the local entity: the eye offset the
// free-fly camera composes from. [orig: NapiNPClientMsg_0x00A @0x42ffb8..0x42ffc9]
void spectator_seat_eye(World &world);

} // namespace opennova::world
