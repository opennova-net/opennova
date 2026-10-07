#pragma once

// The player profile: player.sav's five records and weapon.sav's five, held in
// memory the way the original holds them (the record array @0x252DE58 and
// g_CharSelClass @0x2551130), with the current record the screens and the
// session read (g_curProfileSlot @0x25506B8, g_curPlayerProfile @0x25510FC).
// The image loads when the menu starts and again after an expansion switch,
// the screens edit it in memory, and the save points write both files whole
// [orig: PlayerProfile_LoadAllFromDisk @0x54f4d0; PlayerProfile_SaveToFiles
// @0x54be00]. The embedder owns the files: it hands the loader their bytes and
// writes the bytes the saver returns, player.sav in the game's working
// directory and weapon.sav at playersav::weapon_sav_relpath(expansion).
// Witness record: docs/playerinfo/player-sav-re.md.

#include <formats/playersav/player_sav.h>
#include <formats/playersav/weapon_sav.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::rtxt {
struct File;
}

namespace opennova::profile {

// What a fresh record takes from outside the profile
// [orig: PlayerProfile_InitDefaults @0x54bb40].
struct ProfileDefaults {
	// The default key-binding table (default_binding_table()).
	std::vector<playersav::BindingEntry> bindings;
	// The ten chat macros (default_macros()).
	std::array<std::string, playersav::kMacroCount> macros;
	// The current joystick's capability bits, as the input layer latches them
	// (bit 1, 2, 4) [orig: InputDevice_SetCurrent @0x7644d1..0x7644f8 ->
	// byte_3342F80..82]. 0 with no joystick.
	uint32_t joystick_caps = 0;
	// Per side, the character the character table's first entry of that side
	// packs: its nationality and division ids and its packed id
	// [orig: EntitySlot_LookupAndPackEntry @0x57ad40, called @0x54bbea /
	// @0x54bc0f].
	struct Avatar {
		uint8_t nationality = 0;
		uint8_t division = 0;
		uint16_t packed = 0;
	};
	std::array<Avatar, 2> avatars{};
};

// The default key-binding table: every catalog row whose flag word carries
// 0x4000000, in action-code order (the order the boot's sort leaves the rows
// in), each entry the row's identity and its default bindings, at most
// kBindingCapacity [orig: KeyBinding_BuildFilteredTable @0x54c2b0 — the walk
// @0x54c2d6..0x54c3a5, the capacity stop @0x54c39b; KeyBinding_SortBySequentialId
// @0x498260]. The original then parses a loose default.key over it when one
// exists (@0x54c3b6); no shipped game carries one and it is not ported.
std::vector<playersav::BindingEntry> default_binding_table();

// The ten chat macros a fresh record takes: the menu table's "Macros" section,
// MACRO_0 .. MACRO_9, the expansion's override table first, a miss the
// "??MACRO_n??" marker; each cut to 39 characters [orig: PlayerProfile_InitDefaults
// @0x54bcb5..0x54bccc over TextResource_GetStringWithFallback @0x562ee0 and
// TextResource_FindEntryBySectionAndKey @0x75d250; the table is game.bin,
// Menu_InitShellResources @0x552510]. Either table may be null.
std::array<std::string, playersav::kMacroCount> default_macros(
		const rtxt::File *override_table, const rtxt::File *menu_table);

// A fresh record and its weapon record [orig: PlayerProfile_InitDefaults
// @0x54bb40]: the record cleared and seeded, the weapon record's two class
// bytes, avatar bytes and kit pages written (the rest of it kept, as the
// original writes only those).
void init_defaults(playersav::ProfileRecord &record, playersav::Record &weapons,
		const ProfileDefaults &defaults);

// The table a loaded record keeps: the defaults, each entry whose action code
// and token (without case) a saved entry matches taking that entry's eight
// binding fields; saved entries no default carries are dropped
// [orig: merge_weapon_slot_params @0x54c3f0, run per record by the load
// @0x54f600..0x54f62a, which then stores the default count @0x54f62a].
std::vector<playersav::BindingEntry> merge_bindings(
		const std::vector<playersav::BindingEntry> &defaults,
		const std::vector<playersav::BindingEntry> &saved);

// Whether the name edit keeps a typed character at the end of its text: a
// letter, a digit, a space, '-' or '.' [orig: handle_player_name_change
// @0x55fbf9..0x55fc29].
bool name_char_allowed(char c);

class PlayerProfiles {
public:
	PlayerProfiles();

	// The files the load reads; a null pointer is a file that does not exist.
	struct LoadInput {
		const std::vector<uint8_t> *player_sav = nullptr;
		const std::vector<uint8_t> *weapon_sav = nullptr;
		// The account name GetUserNameA reads into a 16-byte buffer: a longer
		// one fails and leaves the field empty [orig: @0x54f525..0x54f535].
		std::string user_name;
	};

	// [orig: PlayerProfile_LoadAllFromDisk @0x54f4d0]: every record seeded,
	// record 0 named for the account, player.sav's records read over them and
	// each one's bindings merged into the defaults, then weapon.sav's. A file
	// shorter than its records leaves the defaults past its end, as the
	// original's File_Read into the seeded memory does.
	void load(const LoadInput &in, const ProfileDefaults &defaults);

	// The two files' bytes [orig: PlayerProfile_SaveToFiles @0x54be00]: the
	// same header on both, player.sav's records and trailer, weapon.sav's
	// records.
	std::vector<uint8_t> player_sav_bytes() const;
	std::vector<uint8_t> weapon_sav_bytes() const;

	// The current record. The original keeps the index in game.cfg's
	// player_index and clamps it when the load ends [orig: @0x54f65d..0x54f66b,
	// `>= 6` -> 0]; here an index outside the five records is 0.
	int current_slot() const { return slot_; }
	void select_slot(int slot);
	playersav::ProfileRecord &current() { return player_.slots[static_cast<size_t>(slot_)]; }
	const playersav::ProfileRecord &current() const {
		return player_.slots[static_cast<size_t>(slot_)];
	}
	playersav::Record &current_weapons() { return weapons_.slots[static_cast<size_t>(slot_)]; }
	const playersav::Record &current_weapons() const {
		return weapons_.slots[static_cast<size_t>(slot_)];
	}
	const playersav::PlayerSav &player_sav() const { return player_; }
	playersav::PlayerSav &player_sav() { return player_; }
	const playersav::File &weapon_sav() const { return weapons_; }
	playersav::File &weapon_sav() { return weapons_; }
	// No player.sav at the last load [orig: dword_2551124 @0x54f645; nothing
	// reads it].
	bool first_run() const { return first_run_; }

	// The PLAYERNAME edit's change [orig: handle_player_name_change @0x55fbb0]:
	// text whose last character the edit keeps (or no text) names the current
	// record with its first 16 characters and clears the no-name flag; false
	// when the last character is refused, which the screen then cuts from the
	// edit.
	bool rename(const std::string &text);

	// The PLAYER_INFO ACCEPT's name leg [orig: save_player_info_from_dialog
	// @0x55efa8..0x55f039]: the edit's text (15 characters) into the name; an
	// empty one or one that starts or ends with white space empties the name
	// and raises the no-name flag.
	void commit_name(const std::string &text);

	// The QUERY_YES of the PLAYER_INFO delete query: the current record seeded
	// again and flagged nameless [orig: sub_561400 @0x561400 — @0x561435,
	// @0x561442].
	void reset_current(const ProfileDefaults &defaults);

	// SinglePlayer_StartMission's record of the started mission's campaign
	// index (its catalog entry +260, -1 for none) [orig: @0x561b99].
	void record_mission_start(int32_t campaign);

	// The session start's copy of +1460 [orig: Game_ApplySessionSettingsToGlobals
	// @0x55163c -> dword_24D2368] and the round end's write back, after a won
	// campaign mission's completion byte and count outside a session
	// [orig: sub_54D6A0 @0x54d6a0 — the gate @0x54d6c6..0x54d6e5, @0x54d70a,
	// @0x54d712, @0x54d724]. True when the original goes on to save
	// (@0x54d72f); outside a session a player who never spawned returns first.
	void begin_session() { session_word_1460_ = current().word_1460; }
	bool record_round_end(bool in_session, bool spawn_gate, int32_t campaign,
			int32_t campaign_mission, bool won);

	// After the intro videos: +1412 cleared in every record [orig:
	// Game_PlayIntroVideos @0x56385e..0x563876; the save after it is the
	// caller's].
	void clear_intro_pending();

private:
	playersav::PlayerSav player_;
	playersav::File weapons_;
	int slot_ = 0;
	bool header_latched_ = false;  // dword_2540CD8
	uint8_t header_byte_ = 0;      // byte_252DE54
	bool first_run_ = false;
	int32_t session_word_1460_ = 0;  // dword_24D2368
};

}  // namespace opennova::profile
