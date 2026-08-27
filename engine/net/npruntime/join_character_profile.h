#pragma once

// The profile-to-wire projection a joiner's ClientAuth uploads: retail does
// not invent a network-only player id — it packs the two profile character
// selections from Avatars.def (CI0/CI1), stamps the two class bytes
// (CTA/CTB) and the two avatar bytes (VCA/VCB, each the selected combo's head
// voice). A side that is absent or no longer resolves takes the retail
// default: the first combo of its alignment, class 8 (rifleman).
// [orig: PlayerProfile_InitDefaults @0x54BB40 (the fresh profile),
//  lookup_entity_slot_and_pack_entry @0x57AD40 (the per-side default),
//  Avatars_ResolveSelectionIndex @0x57AE60,
//  CNapiServerInfo_SerializeToSession @0x4C3650 <- the profile's two 0x8006
//  side blocks; a saved id the registry no longer resolves is reallocated to
//  the same default by PlayerSession_InitFromProfile @0x50ca80]

#include <net/npruntime/character_registry.h>

#include <cstdint>

namespace opennova::npruntime {

// One side's persisted selection: the Avatars.def tree indices and the class
// byte. `present` false = the side was never saved (the retail default).
inline constexpr int kJoinDefaultPlayerClass = 8; // rifleman
inline constexpr int kJoinPlayerClassMin = 5;
inline constexpr int kJoinPlayerClassMax = 9;
inline constexpr int kJoinDefaultAvatar = 1;

struct JoinSideSelection {
	bool present = false;
	int nationality_index = -1;
	int division_index = -1;
	int combo_index = -1;
	int player_class = kJoinDefaultPlayerClass;
};

struct JoinCharacterProfile {
	uint16_t character_ids[2] = { 0, 0 }; // CI0 / CI1
	int player_classes[2] = { kJoinDefaultPlayerClass, kJoinDefaultPlayerClass };
	int avatars[2] = { kJoinDefaultAvatar, kJoinDefaultAvatar };
	int team_request = -1; // the companion assigns the side
};

// The selected side character when the saved tree indices still resolve to a
// combo of the side's alignment: its packed id and head voice.
inline bool join_side_character(const CharacterRegistry &registry, int side,
		int nationality_index, int division_index, int combo_index,
		uint16_t &out_packed_id, int &out_avatar) {
	const CharacterEntry *entry = registry.find_by_indices(nationality_index,
			division_index, combo_index);
	if (entry == nullptr || entry->alignment != side) return false;
	out_packed_id = entry->packed_id;
	out_avatar = entry->head_voice;
	return true;
}

inline JoinCharacterProfile join_character_profile(
		const CharacterRegistry &registry, const JoinSideSelection saved[2]) {
	JoinCharacterProfile profile;
	for (int side = 0; side < 2; ++side) {
		// The retail default: the first combo of the side's alignment.
		const CharacterEntry *first = registry.find_by_packed_id(
				registry.first_character_id(side), side);
		if (first != nullptr) {
			profile.character_ids[side] = first->packed_id;
			profile.avatars[side] = first->head_voice;
		}
		if (!saved[side].present) continue;
		uint16_t packed = 0;
		int avatar = 0;
		if (join_side_character(registry, side, saved[side].nationality_index,
					saved[side].division_index, saved[side].combo_index, packed,
					avatar)) {
			profile.character_ids[side] = packed;
			profile.avatars[side] = avatar;
		}
		if (saved[side].player_class >= kJoinPlayerClassMin &&
				saved[side].player_class <= kJoinPlayerClassMax)
			profile.player_classes[side] = saved[side].player_class;
	}
	return profile;
}

} // namespace opennova::npruntime
