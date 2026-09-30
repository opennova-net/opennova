// The server-status page's policy (hud_server_status.h): the throttle, the
// roster grid and cells, the class code and the player score list's rows.
#include <runtime/hud/hud_server_status.h>

#include <base/gameprofile/game_type.h> // host_abbreviation_key

#include <cstdio>

namespace opennova::hud {

namespace {

// The slot colours by team byte (+0x1A0) [orig: the switch @0x50a571 —
// 0xFF00FF00 / 0xFF00AFFF / 0xFFFF0000 / 0xFFFFFF00 / 0xFFFF027F, else -1
// @0x50a59b].
uint32_t roster_team_color(uint8_t team) {
	switch (team) {
		case 0: return 0xFF00FF00u;
		case 1: return 0xFF00AFFFu;
		case 2: return 0xFFFF0000u;
		case 3: return 0xFFFFFF00u;
		case 4: return 0xFFFF027Fu;
		default: return 0xFFFFFFFFu;
	}
}

// The status word's colour override [orig: @0x50a5a6..0x50a5e9]. The word
// only ever holds 0 (hud_server_status.h), so the override never applies;
// ported as written.
uint32_t roster_rights_color(uint32_t word, uint32_t team_color) {
	if (word == 0) return team_color;
	uint32_t c = (word & 1u) != 0 ? 0xFFC08000u : 0xFF008000u;
	if ((word & 0x02u) != 0) c |= 0x3F0000u;
	if ((word & 0x04u) != 0) c |= 0xC0u;
	if ((word & 0x08u) != 0) c |= 0x3Fu;
	if ((word & 0x10u) != 0) c |= 0x23FFu;
	if ((word & 0x20u) != 0) c |= 0x43FFu;
	if ((word & 0x80u) != 0) c |= 0x13FFu;
	return c;
}

// The dimmed colour of an in-game slot whose entity is dead: only the three
// base team colours dim [orig: @0x50a69a..0x50a6ba].
uint32_t roster_dead_color(uint32_t c) {
	switch (c) {
		case 0xFF00AFFFu: return 0xFF2030FFu;
		case 0xFFFF0000u: return 0xFFC01010u;
		case 0xFF00FF00u: return 0xFF00A000u;
		default: return c;
	}
}

} // namespace

ServerStatusText server_status_text(const GameTextLookup &gametext, uint32_t game_type) {
	ServerStatusText t;
	const auto server = [&](const char *key) { return game_text(gametext, "Server", key, ""); };
	t.empty_slot = server("STRSRV01");       // [orig: @0x50a3a2]
	t.server_novaworld = server("STRSRV02"); // [orig: @0x50a817]
	t.server_lan = server("STRSRV03");       // [orig: @0x50a84b]
	t.team_wins = server("STRSRV04");
	t.team2 = server("STRSRV05");
	t.team1 = server("STRSRV06");
	t.ties = server("STRSRV07");
	t.team_scores = server("STRSRV08");
	t.frames = server("STRSRV10");         // [orig: @0x50afc3 / @0x50afe8]
	t.total_logins = server("STRSRV11");   // [orig: @0x50b0ef]
	t.cpu = server("STRSRV17");            // [orig: @0x50b038]
	t.score_list_title = server("STRSRV23"); // [orig: HUD_DrawPlayerScoreList @0x50032b]
	t.start_timer = server("STRSRV24");    // [orig: @0x50b0a0]
	t.current_logins = server("STRSRV25"); // [orig: @0x50b165]
	t.game_type_abbreviation = game_text(gametext, "GateTypeAbbrev",
			game_type::host_abbreviation_key(game_type), "");
	return t;
}

const char *quit_dialog_text_key(bool in_session, bool authority) {
	if (!in_session) return "STROVER5";
	return authority ? "STROVER_QUITSERVER" : "STROVER_QUITCLIENT";
}

bool server_status_page_due_at(uint32_t last_ms, uint32_t now_ms, bool window_active) {
	// [orig: @0x50a305..0x50a315 — a zero stamp seeds GetTickCount - 1000;
	//  @0x50a31a..0x50a334 the 200 ms / inactive 10 s gates]
	const uint32_t since = now_ms - (last_ms == 0 ? now_ms - 1000u : last_ms);
	if (since < kServerStatusRedrawMs) return false;
	return window_active || since >= kServerStatusInactiveRedrawMs;
}

bool server_status_page_due(uint32_t *last_ms, uint32_t now_ms, bool window_active) {
	// [orig: the seed @0x50a305..0x50a315 and the store @0x50a35d; the skipped
	//  frame's dword_24D1E00 = 1 @0x50a338 has no reader]
	if (*last_ms == 0) *last_ms = now_ms - 1000u;
	if (!server_status_page_due_at(*last_ms, now_ms, window_active)) return false;
	*last_ms = now_ms;
	return true;
}

ServerStatusRosterGrid server_status_roster_grid(int capacity) {
	ServerStatusRosterGrid g;
	// [orig: `(cap + 29) / 30` @0x50a3f8, `*= 2` under 3 @0x50a40d,
	//  `(cap + columns - 1) / columns` @0x50a420, `1010 / columns` @0x50a424]
	g.columns = (capacity + 29) / 30;
	if (g.columns < 3) g.columns *= 2;
	if (g.columns <= 0) return g;
	g.rows = (capacity + g.columns - 1) / g.columns;
	g.column_step = 1010 / g.columns;
	// [orig: `nameBuffer += 540 / rows` @0x50a7b6]
	g.row_step = g.rows > 0 ? 540 / g.rows : 0;
	return g;
}

char server_status_class_code(int32_t class_word) {
	// [orig: GameType_GetShortCodeWChar @0x4fd9a0]
	switch (class_word) {
		case 1: return 'd';
		case 2: return 'r';
		case 3: return 't';
		case 4: return 'U';
		case 5: return 'M';
		case 6: return 'S';
		case 7: return 'G';
		case 8: return 'R';
		case 9: return 'E';
		default: return '?';
	}
}

ServerStatusRosterCell server_status_roster_cell(const ServerStatusPageState &page,
		int slot_index, uint32_t previous_color) {
	ServerStatusRosterCell cell;
	cell.color = previous_color;
	char buf[160];
	// A slot past the limit prints the empty string [orig: @0x50a4a0 ->
	// sprintf(g_EmptyStr) @0x50a4ac].
	if (slot_index >= page.slot_limit) return cell;
	const ServerStatusSlot *slot = slot_index >= 0 && slot_index < page.capacity &&
					static_cast<size_t>(slot_index) < page.slots.size()
			? &page.slots[static_cast<size_t>(slot_index)]
			: nullptr;
	if (slot == nullptr || !slot->active) {
		// [orig: @0x50a50e..0x50a518 — 0xFF505050, "#%02ld %s" with STRSRV01]
		cell.color = 0xFF505050u;
		std::snprintf(buf, sizeof(buf), "#%02ld %s", static_cast<long>(slot_index),
				page.text.empty_slot.c_str());
		cell.text = buf;
		return cell;
	}
	uint32_t color = roster_rights_color(slot->status_word, roster_team_color(slot->team));
	// [orig: "#%2.2ld " @0x50a5fa, then strcat "%s:%s" (the class letter, the
	//  slot name) @0x50a62d..0x50a663; the page cursor's "???" arm @0x50a609
	//  is dead: the page zeroes the cursor first @0x50a43b]
	std::snprintf(buf, sizeof(buf), "#%2.2ld ", static_cast<long>(slot_index));
	cell.text = buf;
	cell.text += server_status_class_code(slot->class_word);
	cell.text += ':';
	cell.text += slot->name;
	// Not in game: grey; in game with a dead entity: dimmed
	// [orig: `cmp [slot+0x20], 6` @0x50a670; 0xFFA0A0A0 @0x50a672; the entity
	//  health test @0x50a674..0x50a68e]
	if (!slot->in_game)
		color = 0xFFA0A0A0u;
	else if (slot->entity_dead)
		color = roster_dead_color(color);
	// The idle seconds of a remote human, from two seconds
	// [orig: `!slot+0x178E3 && !slot+5 && idle >= 2` @0x50a70b..0x50a720,
	//  " (%ld)" @0x50a734]
	if (!slot->bot && !slot->local && slot->idle_seconds >= 2) {
		std::snprintf(buf, sizeof(buf), " (%ld)", static_cast<long>(slot->idle_seconds));
		cell.text += buf;
	}
	cell.color = color;
	return cell;
}

ServerStatusScoreRow server_status_score_row(const ServerStatusPageState &page, int index) {
	ServerStatusScoreRow row;
	if (index < 0 || static_cast<size_t>(index) >= page.slots.size()) return row;
	const ServerStatusSlot &slot = page.slots[static_cast<size_t>(index)];
	// The host's own slot and every index from 64 are skipped
	// [orig: `cmp [slot+5], 0` @0x500370, `cmp ebp, 40h` @0x50037d].
	if (slot.local || index >= 64) return row;
	row.drawn = true;
	// [orig: x = 200 * (i / 16) + 130 @0x500386..0x50039b; y = 30 * (i % 16 +
	//  6) @0x5003a1..0x5003b7]
	row.x = 200 * (index / 16) + 130;
	row.y = 30 * (index % 16 + 6);
	// The default colour is g_HUDColors.value186C, 0x0000FF00 [orig: read
	// @0x500314; HUD_InitTeamColorTable @0x51f240]; the half-bright drawer
	// forces its alpha.
	constexpr uint32_t kDefault = 0x0000FF00u;
	if (!slot.active) {
		// [orig: "--" @0x5003c2..0x5003d3]
		row.text = "--";
		row.color = kDefault;
		return row;
	}
	// A team game colours by team: 1 palette[3], 2 palette[5], 3 yellow,
	// 4 pink, 0 green, else the default [orig: @0x5003d8..0x50043b;
	// palette[3] 0xFF80A0FF, palette[5] 0xFFFF5050 from HUD_InitTeamColorTable].
	uint32_t color = kDefault;
	if ((page.game_type & 0x10000u) != 0) {
		switch (slot.team) {
			case 1: color = 0xFF80A0FFu; break;
			case 2: color = 0xFFFF5050u; break;
			case 3: color = 0xFFFFFF00u; break;
			case 4: color = 0xFFFF027Fu; break;
			case 0: color = 0xFF00FF00u; break;
			default: break;
		}
	}
	char buf[160];
	const long i = index;
	if (slot.spectator) {
		// A spectator row; one still loading turns auxiliaryColors[0]
		// [orig: `cmp [slot+0x188D7]` @0x50043f, `cmp [slot+0x188E3]`
		//  @0x500448, "%2i --- %s" @0x50045b, auxiliaryColors[0] 0xFFFF40FF
		//  @0x500468].
		std::snprintf(buf, sizeof(buf), "%2li --- %s", i, slot.name.c_str());
		if (slot.loading) color = 0xFFFF40FFu;
	} else {
		switch (page.game_type) {
			case 1u:
			case 0x10001u: {
				// [orig: "%2i %2i:%02i %s" over slot+0x170AC @0x5004ce..0x5004f0]
				const int32_t s = slot.objective_seconds;
				std::snprintf(buf, sizeof(buf), "%2li %2i:%02i %s", i, s / 60, s % 60,
						slot.name.c_str());
				break;
			}
			case 0x10004u:
			case 8u:
				// [orig: "%2i %2i %s", field 0x0B @0x5004ac..0x5004cc]
				std::snprintf(buf, sizeof(buf), "%2li %2i %s", i, slot.flag_captures,
						slot.name.c_str());
				break;
			default:
				// [orig: "%2i %3i %s", field 0x1C @0x5004fa..0x50051a]
				std::snprintf(buf, sizeof(buf), "%2li %3i %s", i, slot.points, slot.name.c_str());
				break;
		}
	}
	row.text = buf;
	row.color = color;
	return row;
}

} // namespace opennova::hud
