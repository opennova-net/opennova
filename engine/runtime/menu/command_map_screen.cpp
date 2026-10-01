// The CMAP screen's tables and ORDERS composer (command_map_screen.h).

#include <runtime/menu/command_map_screen.h>

#include <runtime/hud/hud_game_text.h>

#include <cstdio>

namespace opennova::menu {

namespace {

// The TEAMLIST / PLAYERLIST / CURRENT_ORDERS column indices (cmap.mnu).
constexpr int kTeamRecruit = 0;
constexpr int kTeamJoin = 1;
constexpr int kTeamName = 2;
constexpr int kTeamClass = 3;
constexpr int kTeamMap = 4;
constexpr int kTeamLeader = 5;
constexpr int kTeamFireteam = 6;
constexpr int kPlayerName = 0;
constexpr int kPlayerClass = 1;
constexpr int kPlayerChat = 2;
constexpr int kPlayerAudio = 3;
constexpr int kPlayerPunt = 4;
constexpr int kOrderDelete = 0;
constexpr int kOrderText = 1;

// The recruit box's hold after a press [orig: GetTickCount() + 0x3E8
// @0x548676..0x54867c].
constexpr uint32_t kRecruitHoldMs = 1000;

// The header cells' clear colour [orig: @0x5483e1 / @0x54870a].
constexpr uint32_t kHeaderClear = 0x2040u;
// The state-2 outline [orig: CGfxDevice_SetQuadDiffuse(0xFF7F7F7F) @0x54850d].
constexpr uint32_t kOutline = 0xFF7F7F7Fu;

// g_SquadColors @0x83b450 (14 entries; 0 never drawn: a zero index takes the
// team colour).
constexpr uint32_t kSquadColors[14] = {0xFF000000u, 0xFFA000F0u, 0xFFFF9999u, 0xFFFFCC99u,
		0xFFFFFF99u, 0xFFCCFF99u, 0xFF99FF99u, 0xFF99FFCCu, 0xFF99FFFFu, 0xFF99CCFFu, 0xFF9999FFu,
		0xFFCC99FFu, 0xFFFF99FFu, 0xFFFF99CCu};
// g_MinimapOverlayColorTable @0x840a10 entries 10 (team 1), 9 (team 2) and 12.
constexpr uint32_t kTeam1Map = 0xFF304080u;
constexpr uint32_t kTeam2Map = 0xFF802020u;
constexpr uint32_t kOtherMap = 0xFF208020u;

// The PLAYERLIST row colours by team [orig: CMap_UpdatePlayerListUI
// @0x547f99..0x547fc4 — sub_640110(row, 1, colour)].
constexpr uint32_t kPlayerTeam0 = 0xFF00FF00u;
constexpr uint32_t kPlayerTeam1 = 0xFF00AFFFu;
constexpr uint32_t kPlayerTeam2 = 0xFFFF0000u;

// The combo row values [orig: CMap_OnOpenPopulate @0x5492a0].
constexpr int32_t kGroupFireteam = 0x10000000;
constexpr int32_t kLocationMyPosition = 0x200000FF;
constexpr int32_t kLocationWaypoint = 0x20000000;
constexpr int32_t kLocationZone = 0x10000000;
constexpr int32_t kLocationMember = 0x40000000;

bool same_team(const CommandMapPlayer &p, const CommandMapRoster &roster) {
	// `movzx slot+14` against `movsx local+0x162` [orig: @0x547adc..0x547ae0].
	return static_cast<int>(p.team) == static_cast<int>(roster.local_team);
}

// TextResource_FindEntryBySectionAndKey(g_TextGameText, "menu", key): the
// entry's text, else the key itself.
std::string menu_text(const CommandMapText &text, const char *key) {
	return hud::game_text(text.gametext, "menu", key, key);
}

// The fireteam label [orig: CMap_PopulateTeamList @0x547b40..0x547be2 — the
// STR_CMAP_FIREaTEAMA/B/C "menu" keys, else g_EmptyStr].
std::string fireteam_label(const CommandMapText &text, uint8_t fireteam) {
	switch (fireteam) {
	case 1: return menu_text(text, "STR_CMAP_FIRETEAMA");
	case 2: return menu_text(text, "STR_CMAP_FIRETEAMB");
	case 3: return menu_text(text, "STR_CMAP_FIRETEAMC");
	default: return std::string();
	}
}

// TextResource_GetCharacterClassName: Game.bin "Menu" CHARCLASS_* for 5..9
// (a miss formats "??key??"), else "".
// [orig: TextResource_GetCharacterClassName @0x562f70;
//  TextResource_GetStringWithFallback @0x... (the "??%s??" miss)]
std::string character_class_name(const CommandMapText &text, uint8_t player_class) {
	const char *key = nullptr;
	switch (player_class) {
	case 5: key = "CHARCLASS_MEDIC"; break;
	case 6: key = "CHARCLASS_SNIPER"; break;
	case 7: key = "CHARCLASS_GUNNER"; break;
	case 8: key = "CHARCLASS_RIFLEMAN"; break;
	case 9: key = "CHARCLASS_ENGINEER"; break;
	default: return std::string();
	}
	const std::string miss = std::string("??") + key + "??";
	return hud::game_text(text.menu_ui, "Menu", key, miss.c_str());
}

// The PLAYERLIST class cell: gametext "Overlays" STROVR_* ("" on a miss).
// [orig: CMap_UpdatePlayerListUI @0x547ed6..0x547f18 — GameText_GetString]
std::string overlay_class_name(const CommandMapText &text, uint8_t player_class) {
	const char *key = "STROVR_UNKNOWN";
	switch (player_class) {
	case 5: key = "STROVR_MEDIC"; break;
	case 6: key = "STROVR_SNIPER"; break;
	case 7: key = "STROVR_GUNNER"; break;
	case 8: key = "STROVR_RIFLEMAN"; break;
	case 9: key = "STROVR_ENGINEER"; break;
	default: break;
	}
	return hud::game_text(text.gametext, "Overlays", key, "");
}

// The per-row TEAMLIST cells both populates write after the name: the class,
// the recruit / join boxes and their click mask, the leader's name and the
// fireteam. `own_leader` is the local slot's leader (0xFF without one).
// Returns whether the row names the local slot its leader.
// [orig: CMap_PopulateTeamList @0x547b23..0x547d00; CMap_PopulateTeamList_0
//  @0x548e94..0x549074]
bool write_team_row(MenuRuntime &menu, int table, int row, const CommandMapPlayer &p,
		uint8_t own_leader, const CommandMapRoster &roster, const CommandMapText &text) {
	menu.table_set_cell_text(table, row, kTeamClass, character_class_name(text, p.player_class));
	const std::string fireteam = fireteam_label(text, p.fireteam);
	const uint8_t local = roster.local_slot;
	// Recruitable: leaderless, not the local player, not the local leader
	// [orig: @0x547bea..0x547c0c].
	const bool recruit = p.leader == 0xFF && p.slot != local && own_leader != p.slot;
	// Joinable: another player outside the local squad, or the local row while
	// it has a leader (the leave) [orig: @0x547c11..0x547c35].
	bool join = true;
	if (p.leader == local || p.slot == local) join = p.slot == local && own_leader != 0xFF;
	const bool is_leader = own_leader == p.slot; // [orig: setz @0x547c46]
	std::string leader_name;
	if (const CommandMapPlayer *leader = roster.find(p.leader)) leader_name = leader->name;
	const int32_t mask = (recruit ? 1 : 0) | (join ? (is_leader ? 0 : 2) : 0);
	menu.table_set_cell_value(table, row, kTeamJoin, mask); // [orig: @0x547c9d]
	menu.table_set_cell_text(table, row, kTeamRecruit, recruit ? "1" : "-");
	menu.table_set_cell_text(table, row, kTeamJoin, join ? (is_leader ? "1" : "0") : "-");
	menu.table_set_cell_text(table, row, kTeamLeader, leader_name);
	menu.table_set_cell_text(table, row, kTeamFireteam, fireteam);
	return p.leader == local;
}

uint8_t own_leader_of(const CommandMapRoster &roster) {
	const CommandMapPlayer *self = roster.find(roster.local_slot);
	return self != nullptr ? self->leader : 0xFF;
}

// The members a fireteam order goes to, or the one member.
// [orig: CMap_BuildAndSendOrderCommand @0x54767f..0x547821;
//  CCommandMap_HandleOrderAction @0x548b4c..0x548c11]
void order_targets(const CommandMapOrders::Order &order, const CommandMapRoster &roster,
		CommandMapOrderSend &out) {
	out.targets.clear();
	if ((static_cast<uint32_t>(order.group) & 0xF0000000u) == 0x10000000u) {
		out.kind = 1;
		const uint32_t fireteam = static_cast<uint32_t>(order.group) & 0x0FFFFFFFu;
		for (const CommandMapPlayer &p : roster.players) {
			if (!same_team(p, roster)) continue;
			if (p.slot != roster.local_slot && p.leader == roster.local_slot &&
					p.fireteam == fireteam)
				out.targets.push_back(p.slot);
		}
	} else {
		out.kind = 0;
		out.targets.push_back(static_cast<uint8_t>(order.group));
	}
}

// sub_644590: a list's first selected row, 0 when none.
int first_selected(const MenuRuntime &menu, int id) {
	const int row = menu.selected_row(id);
	return row >= 0 ? row : 0;
}

// CListWnd_SetRowVisibility(row, 1): select it (a row past the list fails).
void select_list_row(MenuRuntime &menu, int id, int row) {
	if (id < 0 || row < 0 || row >= menu.item_count(id)) return;
	menu.select_row(id, row, false);
}

// The combos' "select row 0 when the first selected row is 0".
// [orig: @0x5494e0..0x5494ee (GROUP), @0x549736 (LOCATION), @0x549759..0x5497cd]
void select_first_when_zero(MenuRuntime &menu, int id) {
	if (id >= 0 && first_selected(menu, id) == 0) select_list_row(menu, id, 0);
}

} // namespace

const CommandMapPlayer *CommandMapRoster::find(uint8_t slot) const {
	for (const CommandMapPlayer &p : players)
		if (p.slot == slot) return &p;
	return nullptr;
}

CommandMapTabGates command_map_populate_team_list(MenuRuntime &menu, int table,
		const CommandMapRoster &roster, const CommandMapText &text) {
	CommandMapTabGates gates;
	if (table >= 0) {
		menu.table_remove_row(table, -1); // [orig: @0x547a7e]
		const uint8_t own_leader = own_leader_of(roster);
		for (const CommandMapPlayer &p : roster.players) {
			// [orig: @0x547ac5..0x547af6 — active, same team, not a
			//  spectator, holding an entity]
			if (!same_team(p, roster) || p.spectator || !p.has_entity) continue;
			const int row = menu.table_insert_row(table, "0", p.slot, 0, -1); // [orig: @0x547b0e]
			menu.table_set_cell_text(table, row, kTeamName, p.name);
			if (write_team_row(menu, table, row, p, own_leader, roster, text)) gates.orders = true;
		}
	}
	// [orig: @0x547d15..0x547de8]
	gates.players = roster.in_session;
	gates.team = roster.in_session && !roster.death_screen;
	gates.rules = gates.team;
	gates.sets_rules = true;
	return gates;
}

CommandMapTabGates command_map_update_team_list(MenuRuntime &menu, int table,
		const CommandMapRoster &roster, const CommandMapText &text) {
	CommandMapTabGates gates;
	if (table >= 0) {
		const uint8_t own_leader = own_leader_of(roster);
		for (const CommandMapPlayer &p : roster.players) {
			if (!same_team(p, roster) || p.spectator || !p.has_entity) continue;
			// The row by value, else a new one with the name
			// [orig: @0x548e3b..0x548e8f].
			const int count = menu.table_row_count(table);
			int row = 0;
			for (; row < count; ++row)
				if (static_cast<uint32_t>(menu.table_row_value(table, row)) == p.slot) break;
			if (row >= count) {
				row = menu.table_insert_row(table, "0", p.slot, 0, -1);
				menu.table_set_cell_text(table, row, kTeamName, p.name);
			}
			const bool leads = write_team_row(menu, table, row, p, own_leader, roster, text);
			if (leads && !roster.death_screen) gates.orders = true; // [orig: @0x548fbd..0x548fca]
		}
		// Rows whose slot is gone or a spectator [orig: @0x54908c..0x5490ec].
		int count = menu.table_row_count(table);
		for (int row = 0; row < count; ++row) {
			bool kept = false;
			for (const CommandMapPlayer &p : roster.players) {
				if (!p.spectator && p.slot == static_cast<uint32_t>(menu.table_row_value(table, row))) {
					kept = true;
					break;
				}
			}
			if (!kept) {
				menu.table_remove_row(table, row);
				--count;
				--row;
			}
		}
	}
	gates.players = roster.in_session;
	gates.team = roster.in_session && !roster.death_screen;
	gates.sets_rules = false;
	return gates;
}

void command_map_team_list_timers(MenuRuntime &menu, int table, uint32_t now_ms) {
	if (table < 0) return;
	const int count = menu.table_row_count(table);
	for (int row = 0; row < count; ++row) {
		const uint32_t hold = static_cast<uint32_t>(menu.table_cell_value(table, row, 2));
		if (!(hold < now_ms)) continue; // `jnb` @0x5484c7
		const bool recruit = (menu.table_cell_value(table, row, kTeamJoin) & 1) != 0;
		const char *text = recruit ? "0" : "-";
		if (menu.table_cell_text(table, row, kTeamRecruit) != text)
			menu.table_set_cell_text(table, row, kTeamRecruit, text);
	}
}

CommandMapTeamClick command_map_team_list_click(MenuRuntime &menu, int table, int row, int column,
		int32_t state, const CommandMapRoster &roster, uint32_t now_ms) {
	CommandMapTeamClick out;
	const uint8_t slot = static_cast<uint8_t>(menu.table_row_value(table, row));
	out.slot = slot;
	switch (column) {
	case kTeamRecruit:
		// [orig: @0x54863b..0x54869f]
		if ((menu.table_cell_value(table, row, kTeamJoin) & 1) != 0) {
			out.send_recruit = true;
			menu.table_set_cell_text(table, row, kTeamRecruit, "1");
			menu.table_set_cell_value(table, row, 2,
					static_cast<int32_t>(now_ms + kRecruitHoldMs));
		}
		break;
	case kTeamJoin:
		// [orig: @0x548602..0x54862e]
		if ((menu.table_cell_value(table, row, kTeamJoin) & 2) != 0) out.send_join = true;
		break;
	case kTeamMap: {
		// [orig: @0x5485b8..0x5485f5 — (u8)(+51 + 1) % 14]
		const CommandMapPlayer *p = roster.find(slot);
		if (p == nullptr) return out;
		out.set_squad_color = true;
		out.squad_color = static_cast<uint8_t>(static_cast<uint8_t>(p->squad_color + 1) % 14);
		break;
	}
	default:
		return out;
	}
	menu.table_set_row_selected(table, row, state != kTableRowSelected);
	return out;
}

namespace {

void outline(MenuTableCellCanvas &canvas, float l, float t, float r, float b) {
	canvas.line(kOutline, l, t, r, t);
	canvas.line(kOutline, r, t, r, b);
	canvas.line(kOutline, r, b, l, b);
	canvas.line(kOutline, l, b, l, t);
}

} // namespace

void command_map_team_list_paint(const MenuTableCellEvent &event, MenuTableCellCanvas &canvas,
		int32_t row_value, const CommandMapRoster &roster) {
	float l = event.left, t = event.top, r = event.right, b = event.bottom;
	if (event.row == -1) {
		canvas.clear_rect(kHeaderClear, l, t, r, b);
	} else {
		canvas.draw_cell(event.row, event.column, 1);
		if (event.column == kTeamMap) {
			// [orig: @0x548420..0x548499]
			const CommandMapPlayer *p = roster.find(static_cast<uint8_t>(row_value));
			uint32_t color = kOtherMap;
			if (p != nullptr && p->squad_color != 0)
				color = kSquadColors[p->squad_color % 14];
			else if (p != nullptr && p->team == 1)
				color = kTeam1Map;
			else if (p != nullptr && p->team == 2)
				color = kTeam2Map;
			l += 2.0f;
			t += 2.0f;
			r -= 2.0f;
			b -= 2.0f;
			canvas.clear_rect(color, l, t, r, b);
		}
	}
	canvas.draw_cell(event.row, event.column, 2);
	if (event.state == 2) outline(canvas, l, t, r, b);
}

void command_map_populate_player_list(MenuRuntime &menu, int table,
		const CommandMapRoster &roster, const CommandMapText &text) {
	if (table < 0) return;
	menu.table_remove_row(table, -1); // [orig: @0x547e26]
	for (const CommandMapPlayer &p : roster.players) {
		if (!p.has_entity) continue; // [orig: @0x547e40..0x547e4f]
		const int row = menu.table_insert_row(table, "0", p.slot, 0, -1);
		menu.table_set_cell_text(table, row, kPlayerName, p.name);
		if (p.spectator) {
			// [orig: @0x547e83..0x547ebc]
			menu.table_set_cell_text(table, row, kPlayerClass,
					hud::game_text(text.gametext, "Overlays", "STROVER_SPECTATOR_SUFFIX", ""));
			menu.table_set_cell_text(table, row, kPlayerChat, "1");
			menu.table_set_cell_text(table, row, kPlayerAudio, "1");
		} else {
			menu.table_set_cell_text(table, row, kPlayerClass,
					same_team(p, roster) ? overlay_class_name(text, p.player_class) : "---");
			menu.table_set_cell_text(table, row, kPlayerChat, (p.mute & 2) != 0 ? "0" : "1");
			menu.table_set_cell_text(table, row, kPlayerAudio, (p.mute & 1) != 0 ? "0" : "1");
		}
		// [orig: @0x547f6c..0x547f94]
		const char *punt = roster.death_screen ? "0" : p.punt_mark != 0xFF ? "1" : "0";
		menu.table_set_cell_text(table, row, kPlayerPunt, punt);
		switch (p.team) {
		case 0: menu.table_set_row_color(table, row, true, kPlayerTeam0); break;
		case 1: menu.table_set_row_color(table, row, true, kPlayerTeam1); break;
		case 2: menu.table_set_row_color(table, row, true, kPlayerTeam2); break;
		default: break;
		}
	}
}

CommandMapPlayerClick command_map_player_list_click(MenuRuntime &menu, int table, int row,
		int column, int32_t state, const CommandMapRoster &roster) {
	CommandMapPlayerClick out;
	const uint8_t slot = static_cast<uint8_t>(menu.table_row_value(table, row));
	out.slot = slot;
	const CommandMapPlayer *p = roster.find(slot);
	switch (column) {
	case kPlayerChat:
	case kPlayerAudio: {
		// [orig: @0x5488db..0x54897e (audio, bit 0), @0x548922..0x548950 (chat,
		//  bit 1)]
		if (p == nullptr) return out;
		const uint8_t bit = column == kPlayerChat ? 2 : 1;
		uint8_t mute = p->mute;
		if (slot != roster.local_slot && !p->spectator) {
			mute = static_cast<uint8_t>(mute ^ bit);
			out.set_mute = true;
			out.mute = mute;
		}
		menu.table_set_cell_text(table, row, column, (mute & bit) != 0 ? "0" : "1");
		break;
	}
	case kPlayerPunt: {
		// [orig: @0x5487e2..0x5488be]
		if (p != nullptr && slot != roster.local_slot && !roster.death_screen) {
			const uint8_t target = slot == p->punt_mark ? 0xFF : slot;
			const int count = menu.table_row_count(table);
			for (int r = 0; r < count; ++r) menu.table_set_cell_text(table, r, kPlayerPunt, "0");
			out.clear_punt_marks = true;
			if (target != 0xFF) {
				menu.table_set_cell_text(table, row, kPlayerPunt, "1");
				out.set_punt_mark = true;
				out.punt_mark = target;
				out.send_punt = true;
			}
		}
		break;
	}
	default:
		return out;
	}
	menu.table_set_row_selected(table, row, state != kTableRowSelected);
	return out;
}

void command_map_player_list_paint(const MenuTableCellEvent &event, MenuTableCellCanvas &canvas) {
	if (event.row == -1)
		canvas.clear_rect(kHeaderClear, event.left, event.top, event.right, event.bottom);
	else
		canvas.draw_cell(event.row, event.column, 1);
	canvas.draw_cell(event.row, event.column, 2);
	if (event.state == 2) outline(canvas, event.left, event.top, event.right, event.bottom);
}

std::vector<uint8_t> command_map_fireteam_members(const MenuRuntime &menu, int table,
		const CommandMapRoster &roster) {
	std::vector<uint8_t> out;
	if (table < 0) return out;
	const int count = menu.table_row_count(table);
	for (int row = 0; row < count; ++row) {
		if (!menu.table_row_selected(table, row)) continue;
		const CommandMapPlayer *p =
				roster.find(static_cast<uint8_t>(menu.table_row_value(table, row)));
		if (p != nullptr && p->leader == roster.local_slot) out.push_back(p->slot);
	}
	return out;
}

void command_map_populate_orders(MenuRuntime &menu, const CommandMapOrderWidgets &widgets,
		CommandMapOrders &store, const CommandMapRoster &roster,
		const CommandMapLocations &locations, const CommandMapText &text) {
	if (widgets.group >= 0) {
		// [orig: @0x549374..0x5494ee]
		std::vector<std::string> rows;
		store.group_values.clear();
		bool listed[4] = {false, false, false, false};
		for (const CommandMapPlayer &p : roster.players) {
			if (same_team(p, roster) && p.slot != roster.local_slot) {
				const uint8_t ft = p.fireteam;
				if (ft != 0 && ft <= 3 && !listed[ft]) {
					listed[ft] = true;
					rows.push_back(fireteam_label(text, ft));
					store.group_values.push_back(kGroupFireteam | ft);
				}
			}
			if (same_team(p, roster) && p.slot != roster.local_slot &&
					p.leader == roster.local_slot) {
				rows.push_back(p.name);
				store.group_values.push_back(p.slot);
			}
		}
		menu.set_widget_items(widgets.group, rows);
		select_first_when_zero(menu, widgets.group);
	}
	if (widgets.location >= 0) {
		// [orig: @0x549503..0x549736]
		std::vector<std::string> rows;
		store.location_values.clear();
		rows.push_back(hud::game_text(text.gametext, "menu", "MYPOSITION", "My Position"));
		store.location_values.push_back(kLocationMyPosition);
		for (size_t i = 0; i < locations.user_waypoints.size() && i < 16; ++i) {
			if (locations.user_waypoints[i].empty()) continue;
			rows.push_back(locations.user_waypoints[i]);
			store.location_values.push_back(kLocationWaypoint | static_cast<int32_t>(i));
		}
		for (int index : locations.zone_indices) {
			char key[32];
			std::snprintf(key, sizeof(key), "STRWPNAME%03d", index + 1);
			hud::HudTextArg name;
			name.text = hud::game_text(text.gametext, "WPNames", key, "");
			rows.push_back(hud::hud_sprintf(
					hud::game_text(text.gametext, "Overlays", "STROVER_OBJECTIVEPOINT_SHORT", ""),
					std::vector<hud::HudTextArg>{name}));
			store.location_values.push_back(kLocationZone | index);
		}
		for (size_t i = 0; i < locations.location_names.size(); ++i) {
			rows.push_back(locations.location_names[i]);
			store.location_values.push_back(static_cast<int32_t>(i));
		}
		for (const CommandMapPlayer &p : roster.players) {
			if (same_team(p, roster) && p.slot != roster.local_slot &&
					p.leader == roster.local_slot) {
				rows.push_back(p.name);
				store.location_values.push_back(kLocationMember | p.slot);
			}
		}
		menu.set_widget_items(widgets.location, rows);
		select_first_when_zero(menu, widgets.location);
	}
	select_first_when_zero(menu, widgets.command_order);
	select_first_when_zero(menu, widgets.directional);
	select_first_when_zero(menu, widgets.waitfor);
}

CommandMapOrderSend command_map_new_order(MenuRuntime &menu, const CommandMapOrderWidgets &widgets,
		CommandMapOrders &store, const CommandMapRoster &roster) {
	const int count = static_cast<int>(store.orders.size());
	int slot = count;
	uint32_t code = 0;
	CommandMapOrders::Order entry;
	if (widgets.group >= 0) {
		// [orig: @0x547313..0x54738b]
		const int sel = first_selected(menu, widgets.group);
		const int selected = menu.selected_row(widgets.group);
		const int32_t value = selected >= 0 &&
						selected < static_cast<int>(store.group_values.size())
				? store.group_values[static_cast<size_t>(selected)]
				: 0;
		for (int i = 0; i < count; ++i) {
			if (store.orders[static_cast<size_t>(i)].group == value) {
				slot = i;
				break;
			}
		}
		entry.text = menu.item_display_text(widgets.group, sel);
		entry.group = value;
		code = static_cast<uint32_t>(slot) & 0x1FFu;
	}
	entry.text += "-"; // [orig: @0x5473ad]
	if (widgets.command_order >= 0) {
		const int sel = first_selected(menu, widgets.command_order);
		entry.text += menu.item_display_text(widgets.command_order, sel);
		code ^= (code ^ (static_cast<uint32_t>(sel) << 16)) & 0xFF0000u;
	}
	entry.text += "-";
	if (widgets.location >= 0) {
		const int sel = first_selected(menu, widgets.location);
		entry.text += menu.item_display_text(widgets.location, sel);
		code = (static_cast<uint32_t>(sel) << 24) | (code & 0xFFFFFFu);
	}
	if (widgets.directional >= 0) {
		const int sel = first_selected(menu, widgets.directional);
		code &= 0xFFFF0FFFu;
		if (sel != 0) {
			entry.text += "-";
			entry.text += menu.item_display_text(widgets.directional, sel);
			code ^= (code ^ (static_cast<uint32_t>(sel) << 12)) & 0xF000u;
		}
	}
	if (widgets.waitfor >= 0) {
		const int sel = first_selected(menu, widgets.waitfor);
		if (sel != 0) {
			entry.text += "-";
			entry.text += menu.item_display_text(widgets.waitfor, sel);
			code ^= (code ^ (static_cast<uint32_t>(sel) << 9)) & 0xE00u;
		}
	}
	// An existing entry is rewritten in place (its code only when its row is
	// found) [orig: @0x547641..0x547747].
	if (slot < count) {
		CommandMapOrders::Order &existing = store.orders[static_cast<size_t>(slot)];
		existing.text = entry.text;
		existing.group = entry.group;
	}
	if (widgets.current_orders >= 0) {
		const int table = widgets.current_orders;
		if (slot == count) {
			// [orig: @0x54774c..0x5477b9]
			const int row = menu.table_insert_row(table, "", count, 0, -1);
			menu.table_set_cell_text(table, row, kOrderText, entry.text);
			menu.table_set_cell_text(table, row, kOrderDelete, "0");
			entry.code = code;
			store.orders.push_back(entry);
		} else {
			const int rows = menu.table_row_count(table);
			for (int row = 0; row < rows; ++row) {
				if (menu.table_row_value(table, row) != slot) continue;
				menu.table_set_cell_text(table, row, kOrderText, entry.text);
				menu.table_set_cell_text(table, row, kOrderDelete, "0");
				store.orders[static_cast<size_t>(slot)].code = code;
				break;
			}
		}
	}
	CommandMapOrderSend out;
	const CommandMapOrders::Order &sent =
			slot < static_cast<int>(store.orders.size()) ? store.orders[static_cast<size_t>(slot)] : entry;
	order_targets(sent, roster, out);
	out.text = sent.text;
	out.send = true;
	return out;
}

CommandMapOrderSend command_map_current_orders_click(MenuRuntime &menu,
		const CommandMapOrderWidgets &widgets, CommandMapOrders &store, int row, int column,
		int32_t state, int32_t cell_value, const CommandMapRoster &roster) {
	CommandMapOrderSend out;
	const int table = widgets.current_orders;
	if (column == kOrderText) {
		// [orig: @0x5489e2..0x548b3d]
		const int32_t index = menu.table_row_value(table, row);
		if (index >= 0 && index < static_cast<int32_t>(store.orders.size())) {
			const uint32_t code = store.orders[static_cast<size_t>(index)].code;
			select_list_row(menu, widgets.group, static_cast<int>(code & 0x1FFu));
			select_list_row(menu, widgets.command_order, static_cast<int>((code >> 16) & 0xFFu));
			select_list_row(menu, widgets.location, static_cast<int>((code >> 24) & 0xFFu));
			select_list_row(menu, widgets.directional, static_cast<int>((code >> 12) & 0xFu));
			select_list_row(menu, widgets.waitfor, static_cast<int>((code >> 9) & 7u));
		}
		menu.table_set_row_selected(table, row, state != kTableRowSelected);
		return out;
	}
	if (column != kOrderDelete) return out;
	// [orig: @0x548b4c..0x548c92]
	if (cell_value < 0 || cell_value >= static_cast<int32_t>(store.orders.size())) return out;
	order_targets(store.orders[static_cast<size_t>(cell_value)], roster, out);
	out.text.clear();
	out.send = true;
	menu.table_remove_row(table, row);
	store.orders.erase(store.orders.begin() + cell_value);
	const int rows = menu.table_row_count(table);
	for (int r = cell_value; r < rows; ++r) menu.table_set_cell_value(table, r, 0, r);
	return out;
}

void command_map_seed_current_orders(MenuRuntime &menu, int table, const CommandMapOrders &store) {
	if (table < 0) return;
	menu.table_remove_row(table, -1);
	for (size_t i = 0; i < store.orders.size(); ++i) {
		const int row = menu.table_insert_row(table, "", static_cast<int32_t>(i), 0, -1);
		menu.table_set_cell_text(table, row, kOrderText, store.orders[i].text);
		menu.table_set_cell_text(table, row, kOrderDelete, "0");
	}
}

} // namespace opennova::menu
