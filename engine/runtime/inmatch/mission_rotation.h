#pragma once

// The host's map rotation (D-NET-331, net-re §5.70): the list the host screen
// or the host file seeds, its cursor rules, the round-end advance, and the
// session-level words the map change reads and writes beside it.
//
// g_MissionRotation @0xC86FDC points at a heap block: a five-dword header
// [data, count, capacity, cursor, alt_cursor] and `capacity` 8-byte entries
// {catalog index into g_MissionList, one-shot flag}. The launch option (the
// Attack-and-Defend side switch) lives on the CATALOG row (+0x1140), not on
// the entry, so every entry of one map shares it.
//
// The admin console's MissionList_InsertEntryAtIndex @0x501AD0 and the
// advance's one-shot arm (a flag-1 entry, which only `MISSION ADD ... ONESHOT`
// adds) are the remote admin's (ADR 0051 PR5b, rotation_admin.h). Retail's
// memory faults in them are not ported: the one-shot removal's short copy
// (D-NET-364), the insert's unchecked position and uninitialized alt cursor
// (D-NET-365), and the advance's read before the entries at cursor -1
// (D-NET-366), each a PERMANENT class-D row.

#include <runtime/mission/mission_catalog.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::inmatch {

struct MissionRotationEntry {
	int32_t catalog_index = 0;
	int32_t flag = 0; // the one-shot flag (1 = removed when the advance reaches it)
};

struct MissionRotation {
	// The block's existence: null until the first MissionRotation_Alloc (or an
	// append's lazy one), null again after MissionList_FreeBuffer.
	bool allocated = false;
	// `capacity` slots, the first `count` live. The slots past the count keep
	// whatever the block held (zero after the alloc), as retail's walks that
	// read past the count see them.
	std::vector<MissionRotationEntry> slots;
	int32_t count = 0;
	int32_t cursor = -1;
	int32_t alt_cursor = -1;
	// The catalog entries' +0x1140 launch option word, kept beside the catalog
	// it indexes (0 for a row nothing set).
	std::vector<int32_t> launch_options;
	// The advance's outputs (and the seed's): g_MapFileName, the loose word
	// g_MissionSourceIsLoose, the byte g_MissionLaunchOption @0x24D217D and
	// the catalog row's session code word, which the advance stores into
	// g_GameType.
	std::string map_file;
	bool map_source_is_loose = false;
	uint8_t map_launch_option = 0;
	uint32_t map_game_type = 0;

	int32_t capacity() const { return static_cast<int32_t>(slots.size()); }
	const MissionRotationEntry &entry(int32_t index) const {
		return slots[static_cast<size_t>(index)];
	}

	// [orig: MissionRotation_Alloc @0x501960] frees the old block, then
	// `capacity` (min 1) zeroed slots, count 0, cursor -1, alt cursor 0.
	void alloc(int32_t capacity);
	// [orig: MissionRotation_Append @0x5019D0] a catalog index in
	// [0, catalog count) joins the end; an objective or non-team row loses its
	// launch option.
	void append(const std::vector<mission_catalog::Row> &catalog, int32_t catalog_index,
			int32_t flag);
	// [orig: MissionList_InsertEntryAtIndex @0x501AD0] a catalog index in
	// [0, catalog count] (one past the catalog) joins at `index`: the tail
	// moves up one and a cursor or alt cursor past the index steps up; the
	// launch option is left alone. The position is clamped into [0, count]
	// and a regrow keeps the alt cursor (D-NET-365).
	void insert_entry(const std::vector<mission_catalog::Row> &catalog, int32_t catalog_index,
			int32_t index, int32_t flag);
	// [orig: MissionList_FindByName @0x4FC4C0] the cursor to the first slot
	// (walking the capacity) whose catalog file matches, the alt cursor -1.
	void find_by_name(const std::vector<mission_catalog::Row> &catalog, const std::string &name);
	// [orig: MissionList_GetCurrentEntryPtr @0x4FC690] the cursor's entry, else null.
	const MissionRotationEntry *current() const;
	// [orig: MissionList_RemoveEntry @0x4FC6C0] false when the index is out of
	// [0, count).
	bool remove_entry(int32_t index);
	// [orig: MissionList_SetAltCursor @0x4FC730] false when the index is out of
	// [0, count).
	bool set_alt_cursor(int32_t index);
	// [orig: MissionList_ResetCursor @0x4FC4A0] cursor -1, count 0.
	void reset_cursor();
	// [orig: MissionList_FreeBuffer @0x4FC480] the block freed: no list.
	void free_buffer();

	// The round-end advance, MissionList_GetCurrentEntry @0x4FC540: false when
	// there is no next mission (a null or empty list, a one-shot removal that
	// emptied it, or the cursor past the last entry with REPLAY off), else the
	// next entry's catalog row copied into the outputs. A one-shot entry under
	// the cursor is removed in place, so the entry after it plays next. The
	// catalog's launch options ride `launch_options`.
	bool advance(const std::vector<mission_catalog::Row> &catalog, bool replay_enabled);
	// The outputs from one catalog row (the seed's and the advance's copy).
	void take_row(const std::vector<mission_catalog::Row> &catalog, int32_t catalog_index);
	// The launch-option word of a row, sized to the catalog on first use.
	int32_t &launch_option(const std::vector<mission_catalog::Row> &catalog, int32_t catalog_index);
};

// The session-level rotation state: the words the round end, the map change
// and the team assignment read beside the list. Process globals in retail;
// the embedder owns one for its whole run (the half toggle and the latch are
// never reset, so a session that ended mid-map hands a set toggle to the
// next, §5.70.5) and the host context points at it.
struct HostRotation {
	MissionRotation list;
	// [orig: g_RotationIsFlipped (byte_24D217E)] set while a launch-option
	// map's second half plays; the admin console's (IS FLIPPED).
	bool is_flipped = false;
	// [orig: g_RotationSetNextLatch (dword_24D2180)] the admin MISSION
	// SETNEXT's latch (PR5 sets it), consumed by the teardown's swap arm.
	bool setnext_latch = false;
	// [orig: g_LastGameToggle (dword_C8FC5C)] input action 108's toggle;
	// cleared only by Server_InitNewRoundState (session create and destroy).
	bool last_game = false;
	// [orig: byte_82F240 / byte_82F241] the side-to-team map (initially 1, 2)
	// Server_AssignPlayerTeam's side arms read; the teardown swaps it.
	std::array<uint8_t, 2> side_team = {1, 2};
	// [orig: dword_24D212C] the previous mission's mode: START's first row,
	// then each round init's g_GameType; auto-balance reads it.
	uint32_t previous_game_type = 0;
	// [orig: g_SessionRoundCount (dword_24C116C)] +1 at every authority round
	// end and every received S2C 0x25; zeroed at the host's session start.
	int32_t round_count = 0;
};

// The host screen's START: the list allocated at the SELECTED_MISSIONS table's
// row count, every row appended with flag 0 in table order, and the first row
// the starting map (its file, loose word and launch option, the cursor on it,
// its code word the session's game type and the previous-mode word). `rows` are the table's catalog
// indexes; `launch_options` the catalog rows' words, one per row, as the
// host screen left them (its ADD's default and its Switch cell's toggles,
// game_type::host_rotation_default).
// [orig: UI_HandleHostSessionStart @0x556D66..0x556E2E (LAN);
//  HostDialog_StartSession @0x558804..0x5588BB (the NovaWorld page's connect)]
void seed_rotation_from_host_screen(HostRotation &rotation,
		const std::vector<mission_catalog::Row> &catalog, const std::vector<int32_t> &rows,
		const std::vector<int32_t> &launch_options);

} // namespace opennova::inmatch
