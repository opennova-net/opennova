#include <runtime/world/deploy_screen_feed.h>

#include <base/io/cp1252.h>
#include <runtime/hud/feed_format.h>
#include <runtime/hud/hud_vehicle_panel.h>
#include <runtime/menu/list_sort.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace opennova::world {

std::vector<DeployListRow> build_deploy_rows(const DeployListInput &in) {
    std::vector<DeployListRow> rows;
    // The rows are the game's code-page bytes throughout: its sprintfs join the
    // colour tag, the gametext's strings (the page's bytes) and a zone's letter,
    // the byte index + 0x41, which past 'Z' is a high byte (index 63 is 0x80).
    // The embedder's key, home and zone names are UTF-8, so each is encoded
    // (utf8_to_retail_text) before it joins a row (D-MNU-46).
    // Row 0: "<color>'<key>' <home>", value 0 [orig: sprintf("%s'%s' %s")
    // @0x553b30..0x553b36, the UIList_AddRow call @0x553b4e].
    {
        DeployListRow def;
        def.text = in.team_color_tag + "'" + utf8_to_retail_text(in.default_key) + "' " +
                utf8_to_retail_text(in.default_home);
        def.value = 0;
        rows.push_back(def);
    }
    // The first loop: one row per SECURED team zone, "<color>'<letter>' <name>",
    // value index + 1 [orig: @0x553aef..0x553c55; GameText_GetString("WPNames")
    // @0x553c10, the letter's add @0x553c16, sprintf("%s'%c' %s") @0x553c1f..0x553c25].
    for (const DeployZoneRow &z : in.zones) {
        if (!z.secured) continue;
        DeployListRow row;
        const std::string name =
                utf8_to_retail_text(in.zone_name ? in.zone_name(z.name_key) : z.name_key);
        row.text = in.team_color_tag + "'" + std::string(1, z.letter) + "' " + name;
        row.value = z.index + 1;
        rows.push_back(row);
    }
    // ListWidget_SortRows(list, 0, 1) over the whole list — the Default row
    // included [orig: @0x553c5a]: {0 = string mode, 1 = ascending}, so the
    // compare is stricmp over the rows' texts [orig: cmp @0x6448a0], and
    // rows whose texts tie (two zones 32 indices apart, whose letters fold
    // together, under one name, or the Default row matching a zone's text)
    // stand where the list sort's quicksort leaves them
    // [orig: ListWidget_SortRows @0x644990 -> Utility_SortDualArray (the call
    // @0x6449c0) -> Utility_QuickSortDualArray @0x651AB0] (D-MNU-40).
    menu::sort_dual_array(rows, [](const DeployListRow &a, const DeployListRow &b) {
        return menu::list_text_compare(a.text, b.text);
    });
    // The second loop: EVERY team zone (team byte, def attrib 0x40000, in the
    // spawn-zone list — NO secured gate and NO occupant gate) inserts its
    // members after the row valued index + 1, else at position 0, then appends
    // one blank spacer row unconditionally — so a zone with nobody queued still
    // contributes its blank [orig: @0x553c5f..0x553de3; the member gate
    // `cmp dword_A85BC4[ecx*4],0; jle` @0x553d2f..0x553d36 skips only the member
    // run, the blank UIList_AddRow @0x553dbf..0x553dce follows either way].
    for (const DeployZoneRow &z : in.zones) {
        // An unregistered zone (SpawnZoneList_IndexOf -1) is skipped here
        // [orig: @0x553cea].
        if (z.index < 0) continue;
        int insert_pos = 0;
        for (size_t r = 0; r < rows.size(); ++r) {
            if (rows[r].value == z.index + 1) {
                insert_pos = static_cast<int>(r);
                break;
            }
        }
        for (const DeployOccupant &o : z.occupants) {
            DeployListRow row;
            char buf[512];
            if (o.self)
                std::snprintf(buf, sizeof(buf), "<b><cFF4040>** %s **", o.name.c_str());
            else
                std::snprintf(buf, sizeof(buf), "%s", o.name.c_str());
            row.text = buf;
            row.value = -1;
            // UIList_AddRow(text, -1, 0, insert_pos + 1) returns the row it
            // landed on; the next member follows it [orig: the sprintf
            // @0x553d8e, the UIList_AddRow call @0x553da5].
            ++insert_pos;
            rows.insert(rows.begin() + std::min<int>(insert_pos, static_cast<int>(rows.size())), row);
        }
        DeployListRow blank;
        blank.value = -1;
        ++insert_pos;
        rows.insert(rows.begin() + std::min<int>(insert_pos, static_cast<int>(rows.size())), blank);
    }
    return rows;
}

std::vector<std::string> deploy_list_items(const std::vector<DeployListRow> &rows) {
    std::vector<std::string> items;
    items.reserve(rows.size());
    for (const DeployListRow &row : rows)
        items.push_back(hud::strip_inline_tags(row.text));
    return items;
}

DeployStatusLine build_deploy_status(const DeployStatusInput &in) {
    DeployStatusLine out;
    // The penalty timer wins [orig: dword_A85B5C @0x553928..0x553965].
    if (in.penalty_seconds != 0) {
        out.kind = DeployStatusLine::Kind::Penalty;
        out.seconds = in.penalty_seconds;
        return out;
    }
    // Else the wave zone listing the local player, if it is in the list
    // [orig: word_A85BC0 != -1 @0x55396e; SpawnZoneList_IndexOf >= 0 @0x5539a0].
    if (in.self_zone_index >= 0) {
        out.kind = DeployStatusLine::Kind::Wave;
        out.seconds = in.self_zone_countdown;
        out.numbered = in.self_zone_numbered;
        out.zone_index = in.self_zone_index;
    }
    return out;
}

std::string deploy_status_text(const DeployStatusLine &line,
                               const std::string &penalty_label,
                               const std::string &zone_name) {
    char buf[256];
    switch (line.kind) {
    case DeployStatusLine::Kind::Penalty:
        // [orig: sprintf("%s  <cFF4040>%i", STROVER_PENALTYTIMER, penalty)]
        std::snprintf(buf, sizeof buf, "%s  <cFF4040>%i", penalty_label.c_str(), line.seconds);
        return buf;
    case DeployStatusLine::Kind::Wave:
        if (line.numbered) {
            // [orig: sprintf("'%s':  <cFF4040>%d", WPNames name, countdown)]
            std::snprintf(buf, sizeof buf, "'%s':  <cFF4040>%d", zone_name.c_str(), line.seconds);
        } else {
            // [orig: sprintf("%c:  <cFF4040>%d", 'A' + index, countdown)
            // @0x553a56..0x553a64]
            std::snprintf(buf, sizeof buf, "%c:  <cFF4040>%d",
                          static_cast<char>('A' + line.zone_index), line.seconds);
        }
        return buf;
    case DeployStatusLine::Kind::None:
    default:
        return "";
    }
}

DeployStaticsVisibility deploy_statics_visibility(const DeployStaticsInput &in) {
    DeployStaticsVisibility v;
    // [orig: dword_A85B68 @0x553e2a; dword_A85B60 && !entity+0x1E0 @0x553eb8..0x553ecc]
    v.psp_respawn = in.hold_seconds != 0;
    v.medic = in.revive_seconds != 0 && !in.local_medic_reviving;
    return v;
}

bool deploy_team_buttons_shown(const DeployTeamButtonsInput &in) {
    // [orig: @0x553412..0x553443]
    if (!in.in_session || (in.game_type & 0x20000u) != 0 || (in.game_type & 0x10000u) == 0 ||
            in.team == 0 || (in.permanent_death && in.dead))
        return false;
    // [orig: `shr esi, 2; and esi, 1` @0x553454 / @0x553463]
    return (in.rules_word & 0x4u) != 0;
}

DeployStaticsText deploy_statics_text(const DeployStaticsInput &in,
                                      const std::string &medic_key_label,
                                      const hud::GameTextLookup &lookup) {
    const auto text = [&lookup](const char *key, const char *fallback) {
        return hud::game_text(lookup, hud::kGameTextOverlays, key, fallback);
    };
    DeployStaticsText out;
    char buf[256];
    // [orig: sprintf("%s  <cFF4040>%d", STROVER_PSPRESPAWN, dword_A85B68) @0x553e10]
    std::snprintf(buf, sizeof buf, "%s  <cFF4040>%d",
                  text("STROVER_PSPRESPAWN", "Spawn point available in").c_str(),
                  in.hold_seconds);
    out.psp_respawn = buf;
    // [orig: sprintf("%s  <cFF4040>%d", STROVER_MEDICTIMER, dword_A85B60) @0x553e74]
    std::snprintf(buf, sizeof buf, "%s  <cFF4040>%d",
                  text("STROVER_MEDICTIMER", "Medic time remaining").c_str(),
                  in.revive_seconds);
    out.medic_timer = buf;
    // [orig: sprintf(STROVER_CALLMEDIC, KeyBinding_FormatDisplayString(MedicReq))
    //  @0x553f60]; a format without the key slot draws as authored.
    const std::string call_format = text("STROVER_CALLMEDIC", "Press %s to call a medic");
    const size_t slot = call_format.find("%s");
    out.call_medic = slot == std::string::npos
            ? call_format
            : call_format.substr(0, slot) + medic_key_label + call_format.substr(slot + 2);
    return out;
}


DeployInstructions build_deploy_instructions(
        const DeployInstructionsInput &in, const hud::GameTextLookup &lookup) {
    // [orig: UI_UpdateDeathScreenContent @ 0x5536A0]
    const auto text = [&lookup](const char *key, const char *fallback = "") {
        return hud::game_text(lookup, hud::kGameTextOverlays, key, fallback);
    };
    DeployInstructions out;
    out.permanent_death = in.permanent_death && in.dead;
    if (out.permanent_death) {
        out.show_first = out.show_second = true;
        out.first_text = text("STROVER_PERMANENTDEATH");
        out.second_text = text(in.spectators_allowed
                ? "STROVER_SPECTATORSPAWN" : "STROVER_NORESPAWN");
        out.show_round_status = in.round_ticks >= 0;
        if (out.show_round_status) {
            const int seconds = in.round_ticks / 62;
            char clock[48];
            std::snprintf(clock, sizeof clock, " <cFF4040>%i:%02i:%02i",
                    seconds / 3600, seconds / 60 % 60, seconds % 60);
            out.round_text = text("STROVER50") + clock;
            const std::string label = lookup ? lookup(hud::kGameTextClient, "STRCLI25", "") : "";
            out.remaining_players_text = label + " <cFF4040>" + std::to_string(in.alive_players);
        }
        return out;
    }
    const bool assault = (in.game_type & 0xFFFDFFFFu) == 0x10020u;
    const bool full_spawn = in.check_secured_spawn && in.has_full_team_spawn;
    out.show_first = !assault;
    out.show_second = !full_spawn;
    out.replace_first = !full_spawn;
    if (!full_spawn) {
        if (in.dead) {
            out.first_text = in.kill_announcement;
        } else {
            std::string name = in.player_name.substr(0, 255);
            if (!in.clan.empty()) name = (name + "<ch>" + in.clan + "<co>").substr(0, 255);
            const bool team_join = (in.game_type & 0x10000u) != 0 &&
                    (in.game_type & 0x20000u) == 0 && in.team != 0;
            const std::string format = team_join
                    ? text("STROVER_INITIALSPAWN", "%s, you have joined the %s.")
                    : text("STROVER_WELCOMESPAWN", "Welcome to the game, %s!");
            std::string team = "Unknown";
            if (in.team == 1) team = text("STROVER_BLUETEAM", "!Joint Ops Team");
            else if (in.team == 2) team = text("STROVER_REDTEAM", "!Rebel Team");
            // Resolve the authored string arguments without interpreting markup
            // or any format characters inside the player/clan names.
            unsigned argument = 0;
            for (size_t i = 0; i < format.size(); ++i) {
                if (format[i] == '%' && i + 1 < format.size() && format[i + 1] == 's') {
                    out.first_text += argument++ == 0 ? name : team;
                    ++i;
                } else if (format[i] == '%' && i + 1 < format.size() && format[i + 1] == '%') {
                    out.first_text += '%';
                    ++i;
                } else out.first_text += format[i];
            }
        }
    }
    const char *key = "STROVER_RESPAWN1";
    if (!in.has_spawn_zones) {
        if (in.team == 0) {
            key = "STROVER_SPECTATORSPAWN";
        } else if (in.penalty_line) {
            // [orig: sub_551E10 - the test @0x5526a5, the penalty line @0x5526b8..0x552708,
            //  RESPAWN4 @0x552711..0x55272f, RESPAWN3 @0x552733, RESPAWN4 @0x5526a9 (dfx2.exe)]
            if (!assault || ((in.game_type & 0x20000u) != 0 && in.dead)) {
                if (in.dead && in.penalty_seconds != 0) {
                    char line[256];
                    std::snprintf(line, sizeof line, "%s <cFF4040>%i<co> %s.",
                            text("STROVER_PENALTYTIMER").c_str(), in.penalty_seconds,
                            text("SECONDS").c_str());
                    out.second_text = line;
                    return out;
                }
                key = (in.game_type & 0x20000u) != 0 ? "STROVER_RESPAWN4" : "STROVER_RESPAWN3";
            } else {
                key = "STROVER_RESPAWN4";
            }
        } else if (!assault) {
            key = "STROVER_RESPAWN3";
        } else {
            key = (in.game_type & 0x20000u) != 0 || !in.dead
                    ? "STROVER_RESPAWN4" : "STROVER_RESPAWN5";
        }
    }
    out.second_text = text(key);
    return out;
}

const char *deploy_list_team_color_tag(int team) {
    return team == 2 ? "<cFF2020>" : "<c4040FF>";
}

int deploy_table_icon(uint8_t vehicle_category, int32_t class_id) {
	int icon = 1;
	switch (vehicle_category) {
		case 1:
		case 2:
		case 0xC:
			icon = 2;
			break;
		case 3:
		case 4:
			icon = 4;
			break;
		case 5:
		case 6:
		case 7:
		case 8:
			icon = 3;
			break;
		default:
			break;
	}
	switch (class_id) {
		case 0x210:
			icon = 13;
			break; // Ka-52
		case 0x211:
			icon = 6;
			break; // Apache
		case 0x212:
			icon = 14;
			break; // M1A1 Tank
		case 0x213:
			icon = 19;
			break; // T80
		case 0x214:
		case 0x215:
		case 0x216:
			icon = 8;
			break; // Little Bird
		case 0x217:
			icon = 10;
			break; // Chinook
		case 0x218:
			icon = 20;
			break; // Super Puma
		case 0x219:
			icon = 11;
			break; // Blackhawk
		case 0x21A:
		case 0x21B:
			icon = 18;
			break;
		case 0x21E:
		case 0x21F:
			icon = 7;
			break;
		case 0x220:
			icon = 16;
			break; // Mark V
		case 0x222:
			icon = 9;
			break; // BTR-80
		case 0x223:
			icon = 17;
			break; // Stryker
		case 0x224:
			icon = 15;
			break; // Mil-26 Helo
		case 0x225:
		case 0x226:
		case 0x227:
			icon = 5;
			break; // Nb0-105 Helo
		case 0x228:
		case 0x229:
			icon = 12;
			break; // Assault Boat
		default:
			break;
	}
	return icon;
}

std::string deploy_table_seat_line(const std::string &name, int slot, bool is_driver) {
	std::string line = name;
	if (slot == 8 || is_driver)
		line += "  <i>(Driver)<-i>";
	if (slot == 9)
		line += "  <i>(Gunner)<-i>";
	return line + "\r\n";
}

std::string deploy_table_gunner_line(const std::string &name, const std::string *seat_weapon) {
	return name +
			(seat_weapon == nullptr ? std::string("  <i>(Gunner)<-i>")
									: "  <i>(" + *seat_weapon + ")<-i>") +
			"\r\n";
}

uint32_t deploy_table_health_color(int32_t health, int32_t max_health, uint32_t stance_middle,
		uint32_t stance_bad) {
	switch (hud::seat_health_band(health, max_health)) {
		case hud::SeatHealthBand::Good:
			return 0xFFDDDDDDu; // the argument stands for the good colour
		case hud::SeatHealthBand::Middle:
			return stance_middle;
		default:
			return stance_bad;
	}
}

std::string deploy_table_waiting_line(const std::string &name, bool self) {
	return (self ? "<b><cC0FFFF>** " + name + " **<-b> <i>waiting</i>"
				 : "<cC0F0F0>" + name + " <i>waiting</i>") +
			"\r\n";
}

std::vector<DeployTableRow> build_deploy_table_rows(
		int local_team, const std::vector<DeployTableZone> &zones) {
	std::vector<DeployTableRow> rows;
	DeployTableRow def;
	def.cells[0] = "' '";
	def.cells[1] = "0";
	def.cells[2] = "\r\nDefault";
	// The team's colour, then 0xFFDDDDDD over it: the second write is the one that stands.
	def.color = local_team == 0 ? 0xFF00FF00u : local_team == 2 ? 0xFFFF0000u : 0xFF00AFFFu;
	def.color = 0xFFDDDDDDu;
	rows.push_back(def);
	for (const DeployTableZone &zone : zones) {
		DeployTableRow row;
		row.value = zone.index + 1;
		row.cell1_value = zone.index + 1;
		row.cells[0] = std::string("'") + static_cast<char>('A' + zone.index) + "'";
		row.cells[1] = std::to_string(deploy_table_icon(zone.vehicle_category, zone.class_id));
		row.cells[2] = "\r\n";
		for (const std::string &line : zone.lines)
			row.cells[2] += line;
		row.color = zone.greyed ? 0xFF404040u : zone.health_color;
		row.zone = true;
		row.state1_color = zone.greyed;
		rows.push_back(std::move(row));
	}
	return rows;
}

bool fill_deploy_spawn_table(menu::MenuRuntime &menu, const std::vector<DeployTableRow> &rows) {
	const int table = menu.find_screen_control("DEATH", "SPAWNPOINTS_TABLE");
	if (table < 0) return false;
	const int scroll_top = menu.scroll_row(table);
	const std::vector<int> selected = menu.table_selected_rows(table);
	const int selected_row = selected.empty() ? -1 : selected.front();
	menu.table_clear_rows(table);
	menu.set_widget_shown(table, true);
	const int list_title = menu.find_screen_control("DEATH", "STATIC_LIST_TITLE");
	if (list_title >= 0) menu.set_widget_shown(list_title, true);
	for (const DeployTableRow &row : rows) {
		const int at = menu.table_insert_row(table, row.zone ? "" : "0", row.value, 0, -1);
		menu.table_set_cell_value(table, at, 1, row.cell1_value);
		for (int column = 0; column < 3; ++column)
			menu.table_set_cell_text(table, at, column, row.cells[column]);
		menu.table_set_row_color(table, at, true, row.color);
		if (row.zone) menu.table_set_row_state1_color(table, at, row.state1_color);
	}
	menu.table_set_column_ascending(table, 0, true);
	menu.table_sort_by_column(table, 0);
	if (scroll_top >= 0 && scroll_top < menu.table_row_count(table))
		menu.set_scroll_row(table, scroll_top);
	if (selected_row >= 0) menu.table_set_row_selected(table, selected_row, true);
	return true;
}

DeployTableClick deploy_table_click(const menu::MenuRuntime &menu, int table, int row,
		int32_t row_state, bool double_click) {
	DeployTableClick click;
	if (row_state == menu::kTableRowLocked) return click;
	const std::optional<int32_t> value = menu.table_cell_record(table, row, 1);
	if (!value.has_value()) return click;
	if (!double_click) {
		if (*value - 1 >= 0) click.pan_zone_index = *value - 1;
	} else if (*value != -1) {
		click.pick_value = *value;
	}
	return click;
}

DeployTeamLists deploy_team_lists(const std::vector<DeployTeamListPlayer> &players, int local_team,
		const hud::GameTextLookup &gametext) {
	DeployTeamLists out;
	for (const DeployTeamListPlayer &player : players) {
		std::string &list = player.team == 1 ? out.blue : out.red;
		if (player.local) list += player.team == 1 ? "<cC0C0FF>" : "<cFFC0C0>";
		list += player.name;
		list += "\r\n";
	}
	const uint32_t color = local_team == 0 ? 0xFF00FF00u
			: local_team == 2 ? 0xFFFF0000u : 0xFF00AFFFu;
	const char *key = "STRCLI01";
	switch (static_cast<int8_t>(local_team)) {
		case 0: key = "STRCLI19"; break;
		case 1: key = "STRCLI05"; break;
		case 2: key = "STRCLI06"; break;
		case 3: key = "STRCLI17"; break;
		case 4: key = "STRCLI18"; break;
		default: break;
	}
	char head[32];
	std::snprintf(head, sizeof(head), "Team: <c%08X>", color);
	out.team = head + hud::game_text(gametext, hud::kGameTextClient, key, "");
	return out;
}

void fill_deploy_team_lists(menu::MenuRuntime &menu, const DeployTeamLists &lists) {
	const int blue = menu.find_screen_control("DEATH", "TEAMBLUE");
	const int red = menu.find_screen_control("DEATH", "TEAMRED");
	if (blue >= 0) menu.set_widget_text(blue, lists.blue);
	if (red >= 0) menu.set_widget_text(red, lists.red);
	const int team = menu.find_screen_control("DEATH", "STATIC_TEAM");
	if (team >= 0) menu.set_widget_text(team, lists.team);
}

void apply_deploy_activation(menu::MenuRuntime &menu, const DeployActivation &activation) {
	const auto find = [&menu](const char *name) { return menu.find_screen_control("DEATH", name); };
	if (activation.permanent_dead) {
		for (const char *name : { "STATIC_MEDIC_MSG1", "STATIC_CALLMEDIC_MSG",
					 "STATIC_INSTRUCTIONS2_MSG", "STATIC_LIST_TITLE", "SPAWNPOINTS_LIST" }) {
			const int id = find(name);
			if (id >= 0) menu.set_widget_shown(id, false);
		}
		const int table = activation.table ? find("SPAWNPOINTS_TABLE") : -1;
		if (table >= 0) menu.set_widget_shown(table, false);
	}
	const int briefing = activation.has_briefing ? find("BRIEFING") : -1;
	if (briefing >= 0) menu.set_widget_text(briefing, activation.briefing);
	const int game_type = activation.game_type_line ? find("STATIC_GAMETYPE") : -1;
	if (game_type >= 0) menu.set_widget_text(game_type, "Gametype: <cFFFF40>" + activation.game_type_label);
}

void deploy_table_paint(const menu::MenuTableCellEvent &event, menu::MenuTableCellCanvas &canvas,
		const std::string &zone_name) {
	const float l = event.left, t = event.top, r = event.right, b = event.bottom;
	if (event.row != -1) {
		canvas.draw_cell(event.row, event.column, 1);
		if (event.column == 1 && !zone_name.empty()) {
			const float x = static_cast<float>((static_cast<int>(l) + static_cast<int>(r)) >> 1);
			canvas.hud_label(zone_name, x, b - 2.0f, event.state == 1 ? 0xFF404040u : 0xFFDDDDDDu);
		}
	}
	canvas.draw_cell(event.row, event.column, 2);
	if (event.state == 2) {
		const uint32_t frame = 0xFF7F7F7Fu;
		canvas.line(frame, l, t, r, t);
		canvas.line(frame, r, t, r, b);
		canvas.line(frame, r, b, l, b);
		canvas.line(frame, l, b, l, t);
	}
}

std::string deploy_table_zone_name(const World &world, const SpawnZoneRegistry &zones,
		int32_t cell1_value) {
	const int index = cell1_value - 1;
	if (index < 0 || index >= static_cast<int>(zones.entries.size())) return std::string();
	const Entity *zone = world.registry.get(zones.entries[static_cast<size_t>(index)]);
	if (zone == nullptr || !zone->has_item_def) return std::string();
	return zone->item_ui_name;
}

bool deploy_map_moved(menu::MenuRuntime &menu) {
	const int table = menu.find_screen_control("DEATH", "SPAWNPOINTS_TABLE");
	if (table < 0) return false;
	menu.table_set_row_selected(table, -1, false);
	return true;
}

bool deploy_table_pan_target(const World &world, const SpawnZoneRegistry &zones, int index,
		int32_t &dx, int32_t &dy) {
	if (index < 0 || index >= static_cast<int>(zones.entries.size())) return false;
	const Entity *zone = world.registry.get(zones.entries[static_cast<size_t>(index)]);
	const Entity *local = world.registry.get(world.cached.local_player);
	if (zone == nullptr || local == nullptr) return false;
	dx = io::bam_sub(zone->position.x.q16, local->position.x.q16);
	dy = io::bam_sub(zone->position.y.q16, local->position.y.q16);
	return true;
}

} // namespace opennova::world
