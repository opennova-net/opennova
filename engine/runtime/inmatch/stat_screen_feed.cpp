#include <runtime/inmatch/stat_screen_feed.h>

#include <runtime/hud/end_round_overlay.h> // stat_field_string_index

#include <cstdio>
#include <cstdint>
#include <vector>

namespace opennova::inmatch {

using hud::stat_field_string_index;

std::vector<StatScreenColumn> stat_screen_columns(const EndRoundStats &board,
		bool show_disabled, int table_width) {
	// [orig: StatScreen_PopulateStatResultsList @0x562240 — the column count
	//  @0x562290..0x5622b0, the width split @0x5622c7..0x5622e6, NAME @0x562318,
	//  "Squad" @0x56236b, the field headers @0x562399..0x56249e]
	std::vector<StatScreenColumn> out;
	int count = 2;
	for (const auto &f : board.team_fields)
		if (show_disabled || f.second != 0) ++count;
	const int column_width = count == 1 ? table_width - 150 : (table_width - 150) / (count - 1);
	{
		StatScreenColumn name;
		name.literal = "!Name";
		name.header_fallback = "!Name";
		name.width = 150;
		out.push_back(name);
		StatScreenColumn squad;
		squad.literal = "Squad";
		squad.header_fallback = "Squad";
		squad.width = column_width;
		out.push_back(squad);
	}
	for (size_t i = 0; i < board.team_fields.size(); ++i) {
		const int field_id = board.team_fields[i].first;
		if (!show_disabled && board.team_fields[i].second == 0) continue;
		StatScreenColumn col;
		col.field_id = field_id;
		col.field_index = static_cast<int>(i);
		col.width = column_width;
		const int str_index = stat_field_string_index(field_id);
		char key[64];
		char fallback[64];
		if (str_index != 0) {
			std::snprintf(key, sizeof(key),
					show_disabled ? "STROVER_STATFIELD%02d" : "STROVER_STATFIELDSMALL%02d",
					str_index);
			std::snprintf(fallback, sizeof(fallback), "!%s", key);
			col.header_key = key;
			col.header_fallback = fallback;
		} else {
			// [orig: "Unk entry %d" @0x5623f5]
			std::snprintf(fallback, sizeof(fallback), "Unk entry %d", field_id);
			col.header_key = fallback;
			col.header_fallback = fallback;
		}
		out.push_back(col);
	}
	return out;
}

std::vector<StatScreenRow> stat_screen_rows(const EndRoundStats &board,
		const std::vector<StatScreenPlayer> &players, bool show_disabled,
		int local_slot) {
	// [orig: @0x5624a3..0x5626fa]
	std::vector<StatScreenRow> out;
	for (const StatScreenPlayer &p : players) {
		if (p.team == 0) continue; // field_lookup_ptr[14] != 0
		const EndRoundPlayerRow *row = nullptr;
		for (const EndRoundPlayerRow &r : board.players)
			if (r.slot == p.slot) { row = &r; break; }
		if (row == nullptr) continue;
		StatScreenRow sr;
		sr.slot = p.slot;
		sr.team = p.team;
		sr.name = p.name;
		sr.squad = p.squad.empty() ? "-" : p.squad;
		if (p.team == 1) sr.color_argb = 0xFF00BFFFu;      // -16732161 @0x562564
		else if (p.team == 2) sr.color_argb = 0xFFFF0000u; // -65536 @0x562579
		for (size_t i = 0; i < board.team_fields.size(); ++i) {
			const int field_id = board.team_fields[i].first;
			if (!show_disabled && board.team_fields[i].second == 0) continue;
			const int value = i < row->per_team.size() ? row->per_team[i] : 0;
			char cell[32];
			switch (field_id) {
				case 5:
					std::snprintf(cell, sizeof(cell), "%2i:%02i", value / 60, value % 60);
					break;
				case 19:
					std::snprintf(cell, sizeof(cell), "%i", value);
					break;
				default:
					if ((field_id >= 1 && field_id <= 4) || (field_id >= 6 && field_id <= 18) ||
							(field_id >= 20 && field_id <= 32)) {
						if (value != -1) std::snprintf(cell, sizeof(cell), "%i", value);
						else std::snprintf(cell, sizeof(cell), "-");
					} else {
						std::snprintf(cell, sizeof(cell), "??");
					}
					break;
			}
			sr.cells.push_back(cell);
		}
		sr.selected = local_slot >= 0 && p.slot == local_slot;
		out.push_back(sr);
	}
	return out;
}

bool stat_screen_row_visible(int tab_index, uint8_t team) {
	// [orig: StatScreen_StatFilterTabHandler @0x562140]
	if (tab_index == 1) return team == 2;
	if (tab_index == 2) return team == 1;
	return true;
}

} // namespace opennova::inmatch
