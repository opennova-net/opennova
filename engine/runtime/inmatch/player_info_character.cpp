#include <runtime/inmatch/player_info_character.h>

#include <runtime/inmatch/join_character_profile.h>

#include <net/npwire/character_id.h>

#include <algorithm>

namespace opennova::inmatch {

std::optional<PlayerInfoSideSelection> player_info_side_selection(const CharacterRegistry &registry,
		int team, int nationality, int division, int combo, int player_class) {
	// [orig: EntitySlot_LookupAndPackEntry @0x57ad40 -- the entry's authored
	//  numbers and side bit packed @0x57ae47]
	const CharacterEntry *entry = registry.find_by_indices(nationality, division, combo);
	if (entry == nullptr || entry->alignment != team) return std::nullopt;
	PlayerInfoSideSelection selection;
	selection.team = team;
	selection.nationality = nationality;
	selection.division = division;
	selection.combo = combo;
	selection.player_class = player_class;
	selection.avatar_a = entry->nationality_id;
	selection.avatar_b = entry->division_id;
	selection.avatar_packed = entry->packed_id;
	return selection;
}

namespace {

// The side's retail default: the first combo of its alignment, else entry 0
// whatever its side, with the authored numbers and packed id the side block
// holds (a fallback's packed id keeps entry 0's side bit) and the entry's tree
// indices beside them [orig: EntitySlot_LookupAndPackEntry @0x57ad40 - the side loop
// @0x57ad6b..0x57ad80, the entry-0 fallback @0x57ad84..0x57adda, the stores of
// its outType / outSubtype and packed word in PlayerProfile_InitDefaults
// @0x54bba2..0x54bc17].
std::optional<PlayerInfoSideSelection> side_default(const CharacterRegistry &registry, int side,
		int player_class) {
	const CharacterEntry *first = registry.find_by_packed_id(registry.first_character_id(side));
	if (first == nullptr) return std::nullopt;
	PlayerInfoSideSelection selection;
	selection.team = side;
	selection.nationality = first->nationality_index;
	selection.division = first->division_index;
	selection.combo = first->combo_index;
	selection.player_class = player_class;
	selection.avatar_a = first->nationality_id;
	selection.avatar_b = first->division_id;
	selection.avatar_packed = first->packed_id;
	return selection;
}

} // namespace

void PlayerInfoCharacterState::set_persisted(std::optional<int> team, std::optional<int> player_class,
		const std::optional<PlayerInfoSideSelection> (&sides)[2]) {
	profile_team_ = team;
	profile_class_ = player_class;
	sides_[0] = sides[0];
	sides_[1] = sides[1];
}

void PlayerInfoCharacterState::set_registry(const CharacterRegistry &registry) {
	registry_ = registry;
	for (int side = 0; side < 2; ++side) {
		const std::optional<PlayerInfoSideSelection> &saved = sides_[side];
		// A class byte outside 5..9 reads as the default 8 (rifleman).
		int selected_class = saved ? saved->player_class : profile_class_.value_or(kJoinDefaultPlayerClass);
		if (selected_class < kJoinPlayerClassMin || selected_class > kJoinPlayerClassMax)
			selected_class = kJoinDefaultPlayerClass;
		std::optional<PlayerInfoSideSelection> selection = player_info_side_selection(registry_, side,
				saved ? saved->nationality : -1, saved ? saved->division : -1, saved ? saved->combo : -1,
				selected_class);
		if (!selection) selection = side_default(registry_, side, selected_class);
		sides_[side] = selection;
	}
}

int PlayerInfoCharacterState::initial_team(int authored_team) const {
	return std::clamp(profile_team_.value_or(authored_team), 0, 1);
}

std::optional<PlayerInfoSideSelection> PlayerInfoCharacterState::side_selection(int side) const {
	if (side < 0 || side > 1) return std::nullopt;
	return sides_[side];
}

int PlayerInfoCharacterState::player_class(int side) const {
	if (side >= 0 && side <= 1 && sides_[side]) return sides_[side]->player_class;
	return profile_class_.value_or(-1);
}

void PlayerInfoCharacterState::remember(const PlayerInfoSideSelection &selection) {
	if (selection.team == 0 || selection.team == 1) sides_[selection.team] = selection;
}

PlayerInfoCharacterState::Snapshot PlayerInfoCharacterState::snapshot(int team, int nationality,
		int division, int combo, int selected_class) {
	// [orig: PlayerInfo_SaveFromDialog @0x55ee10 -- the shown team's
	//  character @0x55eeab / @0x55eeee / @0x55ef31, the class to both team
	//  blocks @0x55ee3f..0x55ee5a]
	Snapshot out;
	out.current = player_info_side_selection(registry_, team, nationality, division, combo, selected_class);
	if (out.current) remember(*out.current);
	for (int side = 0; side < 2; ++side) {
		if (!sides_[side]) continue;
		PlayerInfoSideSelection saved = *sides_[side];
		// A side holding the entry-0 fallback (its packed id carries the other
		// side's bit) has no record of its own to save: its block keeps its own
		// bytes (a fresh record's are this same default [orig: PlayerProfile_InitDefaults
		// @0x54bba2..0x54bc17], as PlayerProfiles' set_defaults packs the same
		// first_character_id), and the store's write takes only an id with the
		// shown side's bit.
		if (character_id::alignment(static_cast<uint16_t>(saved.avatar_packed)) != side) continue;
		saved.player_class = selected_class;
		out.sides[side] = saved;
	}
	return out;
}

} // namespace opennova::inmatch
