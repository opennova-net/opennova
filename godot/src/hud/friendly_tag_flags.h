#pragma once

// The HudOverlay friendly-tag flag word: the Simulation packs the sim's
// per-tag facts (world/friendly_tags.h FriendlyTagSource) into ONE int per
// tag and HudOverlay::set_friendly_tags unpacks it into the compiler's
// HudFriendlyTag (engine/runtime/hud hud_frame.h). Both ends read these
// constants, so the layout has one owner. Retail's drawer reads the facts
// straight off the entity and its connection slot (the witness lives with the
// gather in world/friendly_tags.h); the packed word is this device seam's
// transport only.

#include <cstdint>

namespace godot::friendly_tag_flags {

inline constexpr int32_t kMedic = 1;         // the red-cross plate (charattr Medic)
inline constexpr int32_t kSpeaking = 2;      // the voice pulse
inline constexpr int32_t kPlayer = 4;        // a player-slot entry (empty callsign draws the bar)
inline constexpr int32_t kDead = 8;          // entity Flags & 2
inline constexpr int32_t kHasSlot = 16;      // a connection slot owns the entity
inline constexpr int32_t kMedicRequest = 32; // the slot's medic-request latch (the pulse)
inline constexpr int32_t kReviveShift = 8;   // bits 8..15: the slot's revive countdown seconds
inline constexpr int32_t kReviveMax = 255;

inline int32_t pack(bool medic, bool speaking, bool player, bool dead, bool has_slot,
		bool medic_request, int revive_seconds) {
	int32_t flags = 0;
	if (medic) flags |= kMedic;
	if (speaking) flags |= kSpeaking;
	if (player) flags |= kPlayer;
	if (dead) flags |= kDead;
	if (has_slot) flags |= kHasSlot;
	if (medic_request) flags |= kMedicRequest;
	const int32_t revive = revive_seconds < 0 ? 0 : (revive_seconds > kReviveMax ? kReviveMax : revive_seconds);
	flags |= revive << kReviveShift;
	return flags;
}

} // namespace godot::friendly_tag_flags
