// The `.bad` event trigger word, bit by bit: what a clip's per-frame event
// makes the body do. The runtime reads the word out of the clip's event record
// (formats/bad `BadEvent.trigger`, sampled unlerped from the frame's lower
// keyframe by runtime/anim/adm_root_motion) and consumes these bits on the
// body's own tick half.
//
// The table exists so an authoring front end offers the bits the engine
// actually consumes (`opennova-3di catalog` prints it) instead of keeping its
// own copy of them. Nothing in the runtime dispatches through the table: each
// consumer tests its own witnessed mask, which is what the citations name.
#pragma once

#include <cstdint>

namespace opennova::anim {

struct AnimEventBit {
	uint32_t mask;
	const char *name;
	const char *what;
};

// [orig: the footstep and foley block Entity_UpdateInfantryAI
//  @0x4bf169-0x4bf2b0 (the player body's twin @0x4b76f1-0x4b78a8): bits
//  0x20 << i are SSAudio1..6, 0x1 and 0x2 the left and right foot, the sound
//  dipped to foot level by the frame's capsule bottom; the fire block
//  @0x4BF15C..0x4BF425: 0x4 the primary round, 0x8 the secondary latch,
//  0x10 the third ammo row.]
inline constexpr AnimEventBit kAnimEventBits[] = {
		{0x1u, "FOOT_LEFT", "a left footstep at foot level, the slot picked by surface"},
		{0x2u, "FOOT_RIGHT", "a right footstep"},
		{0x4u, "FIRE_PRIMARY", "fire the body's primary ammo row"},
		{0x8u, "FIRE_SECONDARY", "latch the secondary row, fired on the next tick"},
		{0x10u, "FIRE_THIRD", "fire the third ammo row"},
		{0x20u, "FOLEY_1", "sound profile slot SSAudio1"},
		{0x40u, "FOLEY_2", "sound profile slot SSAudio2"},
		{0x80u, "FOLEY_3", "sound profile slot SSAudio3"},
		{0x100u, "FOLEY_4", "sound profile slot SSAudio4"},
		{0x200u, "FOLEY_5", "sound profile slot SSAudio5"},
		{0x400u, "FOLEY_6", "sound profile slot SSAudio6"},
};

inline constexpr int kAnimEventBitCount = static_cast<int>(sizeof(kAnimEventBits) /
		sizeof(kAnimEventBits[0]));

} // namespace opennova::anim
