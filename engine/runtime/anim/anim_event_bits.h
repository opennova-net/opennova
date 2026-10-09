// The `.bad` event trigger word, bit by bit: what a clip's per-frame event
// makes the body do. The runtime reads the word out of the clip's event record
// (formats/bad `BadEvent.trigger`, sampled unlerped from the frame's lower
// keyframe by runtime/anim/adm_root_motion) and consumes these bits on the
// body's own tick half.
//
// The named masks are what each consumer tests (runtime/world infantry_sound,
// wire_body_sound, infantry_combat), and the table is what an authoring front
// end offers (`opennova-3di catalog` prints it as `trigger 0xMASK NAME`)
// instead of keeping its own copy of the bits.
#pragma once

#include <cstdint>

namespace opennova::anim {

// [orig: the footstep and foley block Entity_UpdateInfantryAI
//  @0x4bf169-0x4bf2b0 (the player body's twin @0x4b76f1-0x4b78a8): bits
//  0x20 << i are SSAudio1..6, 0x1 and 0x2 the left and right foot, the sound
//  dipped to foot level by the frame's capsule bottom.]
inline constexpr uint32_t kAnimEventFootLeft = 0x1u;
inline constexpr uint32_t kAnimEventFootRight = 0x2u;
// The first of the six foley bits; foley slot i (0..5) is kAnimEventFoley1 << i.
inline constexpr uint32_t kAnimEventFoley1 = 0x20u;
inline constexpr int kAnimEventFoleyCount = 6;
// [orig: the fire block Entity_UpdateInfantryAI @0x4bf31d-0x4bf4ad over the
//  body's four ammo bytes +0x358..+0x35B (closeattack/easyrocket/
//  advancedrocket/marker3, runtime/world/ai.h) and its launch points
//  +0x365/+0x366/+0x367: 0x4 fires +0x358 from +0x365 (@0x4bf322-0x4bf35c);
//  0x8 sets the local fire-secondary flag (@0x4bf39b) that the SAME pass
//  consumes (@0x4bf406), firing +0x359 and then +0x35A when it differs, both
//  from +0x366 (@0x4bf414-0x4bf498); 0x10 fires +0x35B from +0x367
//  (@0x4bf3a6-0x4bf3e0). The fire block is the NPC think's alone: the player body's twin of the
//  footstep and foley block has none, so a player's body (US01.adm's) fires nothing from a clip.]
inline constexpr uint32_t kAnimEventFirePrimary = 0x4u;
inline constexpr uint32_t kAnimEventFireSecondary = 0x8u;
inline constexpr uint32_t kAnimEventFireMarker3 = 0x10u;
// The fire block's three bits together: a frame whose word carries any of them
// makes an NPC's body fire (a player's body reads none).
inline constexpr uint32_t kAnimEventFireMask =
		kAnimEventFirePrimary | kAnimEventFireSecondary | kAnimEventFireMarker3;

// A bit: its mask, its token (what the .o3a and the add-on's markers spell), what the body does
// on the frame, and the same in a few words for a title or a timeline's mark ("right footstep").
struct AnimEventBit {
	uint32_t mask;
	const char *name;
	const char *what;
	const char *words;
};

inline constexpr AnimEventBit kAnimEventBits[] = {
		{kAnimEventFootLeft, "FOOT_LEFT", "a left footstep at foot level, the slot picked by surface",
				"left footstep"},
		{kAnimEventFootRight, "FOOT_RIGHT", "a right footstep", "right footstep"},
		{kAnimEventFirePrimary, "FIRE_PRIMARY",
				"an NPC's body fires the first ammo byte (closeattack) from the first launch point",
				"an NPC fires its first ammo"},
		{kAnimEventFireSecondary, "FIRE_SECONDARY",
				"an NPC's body fires the second ammo byte (easyrocket), then the third (advancedrocket) "
				"when it differs, from the second launch point, in the same pass",
				"an NPC fires its second ammo"},
		{kAnimEventFireMarker3, "FIRE_MARKER3",
				"an NPC's body fires the fourth ammo byte (marker3) from the third launch point",
				"an NPC fires its fourth ammo"},
		{kAnimEventFoley1 << 0, "FOLEY_1", "sound profile slot SSAudio1", "sound 1"},
		{kAnimEventFoley1 << 1, "FOLEY_2", "sound profile slot SSAudio2", "sound 2"},
		{kAnimEventFoley1 << 2, "FOLEY_3", "sound profile slot SSAudio3", "sound 3"},
		{kAnimEventFoley1 << 3, "FOLEY_4", "sound profile slot SSAudio4", "sound 4"},
		{kAnimEventFoley1 << 4, "FOLEY_5", "sound profile slot SSAudio5", "sound 5"},
		{kAnimEventFoley1 << 5, "FOLEY_6", "sound profile slot SSAudio6", "sound 6"},
};

inline constexpr int kAnimEventBitCount = static_cast<int>(sizeof(kAnimEventBits) /
		sizeof(kAnimEventBits[0]));

// Every bit a reader tests: the table's masks together. A trigger word's other
// bits make the body do nothing.
inline constexpr uint32_t anim_event_known_mask() {
	uint32_t mask = 0;
	for (const AnimEventBit &bit : kAnimEventBits) mask |= bit.mask;
	return mask;
}
inline constexpr uint32_t kAnimEventKnownMask = anim_event_known_mask();

} // namespace opennova::anim
