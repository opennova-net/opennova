#pragma once

// The PLAYER_INFO screen's character memory for the active profile slot: the
// per-(slot, team) selection globals collapsed to slot 0 with both team sides
// kept, the per-side retail default, and the dialog save's shared-class rule.
// It lives beside join_character_profile.h because both read the character
// registry (the packed id is the wire's, npwire/character_id.h); the shell's
// PlayerCharacterSelectionState keeps only the profile Dictionary's marshalling.
// [orig: g_CharSelClass @0x2551130, g_CharSelNationality @0x2551131,
//  g_CharSelDivision @0x2551132, g_CharSelCombo @0x2551134 -- strides 67596
//  per slot and 32774 per team; PlayerInfo_SaveFromDialog @0x55ee10]

#include <runtime/inmatch/character_registry.h>

#include <optional>

namespace opennova::inmatch {

// One side's selection: the side, the Avatars.def tree indices, the class
// byte, and the authored nationality / division numbers with the packed id
// they make.
struct PlayerInfoSideSelection {
	int team = -1;
	int nationality = -1;
	int division = -1;
	int combo = -1;
	int player_class = 8;
	int avatar_a = 0;
	int avatar_b = 0;
	int avatar_packed = 0;
};

// The side's selection at those tree indices when they name a combo of the
// side's alignment: avatar_a / avatar_b are the authored nationality and
// division numbers, not the visible rows, and the packed word also carries the
// authored combo number and the side bit
// [orig: EntitySlot_LookupAndPackEntry @0x57ad40; the packed store @0x57ae47].
std::optional<PlayerInfoSideSelection> player_info_side_selection(const CharacterRegistry &registry,
		int team, int nationality, int division, int combo, int player_class);

class PlayerInfoCharacterState {
public:
	// The persisted profile the screen opens over: its shown side and class
	// when it names them, and each side's saved selection (absent = never
	// saved).
	void set_persisted(std::optional<int> team, std::optional<int> player_class,
			const std::optional<PlayerInfoSideSelection> (&sides)[2]);

	// Validate both remembered sides against the mounted Avatars.def: a saved
	// side that still resolves is kept as saved, a stale or absent one takes
	// the retail default, the first combo of the side's alignment
	// [orig: EntitySlot_LookupAndPackEntry @0x57ad40 -- the side loop
	// @0x57ad6b..0x57ad80; PlayerProfile_InitDefaults @0x54bb40 seeds it, and
	// PlayerSession_InitFromProfile @0x50ca80 reallocates a saved id the
	// registry no longer resolves to it]. The class is the side's byte, else the
	// profile's, read as the default 8 outside 5..9 [orig: the default 8 the
	// fresh profile seeds into both blocks, PlayerProfile_InitDefaults
	// @0x54bbe0 / @0x54bbe3], as join_character_profile.h reads it out of a
	// session (D-PLAYERINFO-33). An Avatars.def with no combo of the side's
	// alignment gives that side entry 0, the other side's first combo, its
	// numbers and packed id as the packer leaves them in the side block
	// [orig: EntitySlot_LookupAndPackEntry @0x57ad84..0x57adda, the entry-0
	// fallback]; an empty registry (no Avatars.def) leaves neither side.
	void set_registry(const CharacterRegistry &registry);

	// The side the screen shows first: the profile's, else the authored
	// radio's, clamped to 0..1.
	int initial_team(int authored_team) const;
	std::optional<PlayerInfoSideSelection> side_selection(int side) const;
	// The side's class byte, else the profile's, else -1.
	int player_class(int side) const;

	// The dialog save's character half: the shown side's selection (when it
	// resolves) remembered, then the selected class stamped across both sides'
	// selections while the character bytes stay per side, as the save writes
	// the class to both team blocks and the character to the shown one
	// [orig: PlayerInfo_SaveFromDialog @0x55ee10 -- @0x55ee3f..0x55ef38: the
	// class loop @0x55ee3f..0x55ee5a over both team blocks, then the
	// nationality / division / combo stores of the shown team @0x55eeab /
	// @0x55eeee / @0x55ef31]. A side holding the entry-0 fallback has no
	// record of its own in `sides`.
	struct Snapshot {
		std::optional<PlayerInfoSideSelection> current;
		std::optional<PlayerInfoSideSelection> sides[2];
	};
	Snapshot snapshot(int team, int nationality, int division, int combo, int selected_class);

private:
	void remember(const PlayerInfoSideSelection &selection);

	CharacterRegistry registry_;
	std::optional<int> profile_team_;
	std::optional<int> profile_class_;
	std::optional<PlayerInfoSideSelection> sides_[2];
};

} // namespace opennova::inmatch
