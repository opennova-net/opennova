#include <runtime/inmatch/mission_rotation.h>

#include <base/gameprofile/game_type.h>
#include <base/io/log.h>
#include <base/io/strutil.h>

namespace opennova::inmatch {

namespace {

bool row_in_catalog(const std::vector<mission_catalog::Row> &catalog, int32_t index) {
	return index >= 0 && static_cast<size_t>(index) < catalog.size();
}

uint32_t row_game_type(const mission_catalog::Row &row) {
	return game_type::for_mission_mode(row.game_mode);
}

} // namespace

int32_t &MissionRotation::launch_option(const std::vector<mission_catalog::Row> &catalog,
		int32_t catalog_index) {
	if (launch_options.size() != catalog.size()) launch_options.resize(catalog.size(), 0);
	return launch_options[static_cast<size_t>(catalog_index)];
}

// [orig: MissionRotation_Alloc @0x501960 -- the free @0x50196A..0x501975, the
//  min-1 capacity, the memset of the block, count 0 and cursor -1; the alt
//  cursor is the memset's 0]
void MissionRotation::alloc(int32_t new_capacity) {
	if (new_capacity < 1) new_capacity = 1;
	allocated = true;
	slots.assign(static_cast<size_t>(new_capacity), MissionRotationEntry{});
	count = 0;
	cursor = -1;
	alt_cursor = 0;
}

// [orig: MissionRotation_Append @0x5019D0 -- the bounds test @0x5019D5..0x5019E3,
//  the lazy MissionRotation_Alloc(1), the regrow by five that copies the count
//  and the cursor and resets the alt cursor to -1 @0x501A6A, the append
//  @0x501A86..0x501AA0, the launch option's clear @0x501AA8..0x501AC1]
void MissionRotation::append(const std::vector<mission_catalog::Row> &catalog,
		int32_t catalog_index, int32_t flag) {
	if (!row_in_catalog(catalog, catalog_index)) return;
	if (!allocated) alloc(1);
	if (count >= capacity()) {
		slots.resize(slots.size() + 5, MissionRotationEntry{});
		alt_cursor = -1;
	}
	slots[static_cast<size_t>(count)] = MissionRotationEntry{catalog_index, flag};
	++count;
	if (!game_type::host_rotation_default(row_game_type(catalog[static_cast<size_t>(catalog_index)])))
		launch_option(catalog, catalog_index) = 0;
}

// Retail walks to the capacity, so a slot past the count (a zeroed one names
// catalog row 0) can match; the seeds always find the entry they appended
// first, within the count.
// [orig: MissionList_FindByName @0x4FC4C0 -- the cursor reset @0x4FC4D0, the
//  walk @0x4FC4F0..0x4FC51B, the hit @0x4FC521..0x4FC52F]
void MissionRotation::find_by_name(const std::vector<mission_catalog::Row> &catalog,
		const std::string &name) {
	if (!allocated) return;
	cursor = -1;
	for (int32_t i = 0; i < capacity(); ++i) {
		const int32_t row = entry(i).catalog_index;
		if (row_in_catalog(catalog, row) &&
				strutil::iequals(catalog[static_cast<size_t>(row)].file, name)) {
			cursor = i;
			alt_cursor = -1;
			return;
		}
	}
}

// [orig: MissionList_GetCurrentEntryPtr @0x4FC690]
const MissionRotationEntry *MissionRotation::current() const {
	if (!allocated || cursor < 0 || cursor >= count) return nullptr;
	return &entry(cursor);
}

// Retail copies 8 * (count - index) bytes, one slot past the live tail (the
// slot at the count, stale or zero, slides into the last live position and
// then falls out of the count); the live entries come out an exact erase.
// [orig: MissionList_RemoveEntry @0x4FC6C0 -- the range test @0x4FC6D5..0x4FC6DF,
//  the copy @0x4FC6F4, the cursor steps @0x4FC707 / @0x4FC714, the count
//  @0x4FC71E]
bool MissionRotation::remove_entry(int32_t index) {
	if (!allocated || count == 0 || index >= count || index < 0) return false;
	for (int32_t i = index; i < count - 1; ++i)
		slots[static_cast<size_t>(i)] = slots[static_cast<size_t>(i) + 1];
	if (count < capacity()) slots[static_cast<size_t>(count) - 1] = slots[static_cast<size_t>(count)];
	if (cursor > index) --cursor;
	if (alt_cursor > index) --alt_cursor;
	--count;
	return true;
}

// [orig: MissionList_SetAltCursor @0x4FC730]
bool MissionRotation::set_alt_cursor(int32_t index) {
	if (!allocated || count == 0 || index >= count || index < 0) return false;
	alt_cursor = index;
	return true;
}

// [orig: MissionList_ResetCursor @0x4FC4A0]
void MissionRotation::reset_cursor() {
	if (!allocated) return;
	cursor = -1;
	count = 0;
}

// [orig: MissionList_FreeBuffer @0x4FC480]
void MissionRotation::free_buffer() {
	allocated = false;
	slots.clear();
	count = 0;
	cursor = -1;
	alt_cursor = -1;
}

// The catalog row copied out: the file, the loose word, the launch option as
// a byte, and the row's code word as the session's game type.
// [orig: MissionList_GetCurrentEntry @0x4FC620..0x4FC62C (the file),
//  @0x4FC647 (the loose word), @0x4FC662 (the launch option byte), @0x4FC678
//  (g_GameType)]
void MissionRotation::take_row(const std::vector<mission_catalog::Row> &catalog,
		int32_t catalog_index) {
	if (!row_in_catalog(catalog, catalog_index)) return;
	const mission_catalog::Row &row = catalog[static_cast<size_t>(catalog_index)];
	map_file = row.file;
	map_source_is_loose = row.loose;
	map_launch_option = static_cast<uint8_t>(launch_option(catalog, catalog_index));
	map_game_type = row_game_type(row);
}

// [orig: MissionList_GetCurrentEntry @0x4FC540]
bool MissionRotation::advance(const std::vector<mission_catalog::Row> &catalog,
		bool replay_enabled) {
	// A null or empty list ends the rotation; a cursor at or past the count
	// starts over [orig: @0x4FC547, @0x4FC551, @0x4FC556..0x4FC558].
	if (!allocated || count == 0) return false;
	if (cursor >= count) cursor = 0;
	// Retail has no `cursor < 0` test: after an in-game MISSION CLEAR and a
	// later ADD the cursor sits at -1, and the flag read at data + 8 * -1 + 4
	// is the header's last word, the alt cursor (§5.70.1; reachable only
	// through the admin console, PR5) [orig: @0x4FC561..0x4FC572].
	const int32_t flag = cursor >= 0 ? entry(cursor).flag : alt_cursor;
	if (flag != 0) {
		// The one-shot arm: the entry is removed in place and the next one
		// plays without a cursor move [orig: @0x4FC574..0x4FC5B1]. Only the
		// admin console's `MISSION ADD ... ONESHOT` appends a flagged entry,
		// so the arm and its retail defects (the copy one entry short, the
		// heap overrun of a last one-shot entry) are ADR 0051 PR5's to port.
		io::logf(io::LogLevel::kWarn,
				"rotation: a one-shot entry reached the advance; its removal is not ported");
		return false;
	}
	// A clear flag steps to the alt cursor when one is set, else by one, and
	// the alt cursor clears [orig: @0x4FC5B6..0x4FC5CC].
	if (alt_cursor == -1)
		++cursor;
	else
		cursor = alt_cursor;
	alt_cursor = -1;
	// Past the end: the rotation is exhausted unless REPLAY wraps it; an alt
	// cursor past the end clears [orig: @0x4FC5D4..0x4FC5E9, @0x4FC5FD].
	if (cursor >= count) {
		if (!replay_enabled) return false;
		cursor = 0;
	}
	if (alt_cursor >= count) alt_cursor = -1;
	take_row(catalog, entry(cursor).catalog_index);
	return true;
}

void seed_rotation_from_host_screen(HostRotation &host_rotation,
		const std::vector<mission_catalog::Row> &catalog, const std::vector<int32_t> &rows,
		const std::vector<int32_t> &launch_options) {
	MissionRotation &rotation = host_rotation.list;
	// The Switch cells are the catalog rows' words before START reads them.
	rotation.launch_options = launch_options;
	rotation.launch_options.resize(catalog.size(), 0);
	rotation.alloc(static_cast<int32_t>(rows.size()));
	bool first = true;
	for (const int32_t row : rows) {
		rotation.append(catalog, row, 0);
		if (!first || !row_in_catalog(catalog, row)) continue;
		// The first row: g_MapFileName, the loose word, the launch option (after
		// the append's clear), the cursor, g_GameType [orig: @0x556DC3..0x556E1F].
		rotation.take_row(catalog, row);
		rotation.find_by_name(catalog, catalog[static_cast<size_t>(row)].file);
		host_rotation.previous_game_type = rotation.map_game_type;
		first = false;
	}
}

} // namespace opennova::inmatch
