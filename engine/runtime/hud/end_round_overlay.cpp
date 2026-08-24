#include "hud/end_round_overlay.h"

#include <algorithm>
#include <cstdio>

namespace opennova::hud {

namespace {

EndRoundArg key_arg(const char *key, const char *fallback = "") {
	EndRoundArg a;
	a.key = key;
	a.fallback = fallback;
	return a;
}
EndRoundArg literal_arg(const std::string &s) {
	EndRoundArg a;
	a.literal = s;
	return a;
}
EndRoundArg number_arg(int32_t n) {
	EndRoundArg a;
	a.number = n;
	a.is_number = true;
	return a;
}
EndRoundLine key_line(const char *key, int y, const char *fallback = "") {
	EndRoundLine l;
	l.key = key;
	l.fallback = fallback;
	l.y = y;
	return l;
}
EndRoundLine literal_line(const char *fmt, int y) {
	EndRoundLine l;
	l.literal = fmt;
	l.y = y;
	return l;
}

} // namespace

std::vector<EndRoundLine> end_round_overlay_lines(const EndRoundOverlayInput &in) {
	// [orig: draw_endround_stats_overlay @0x5b7cd0]
	std::vector<EndRoundLine> out;
	const bool team_mode = (in.game_type & 0x10000u) != 0;
	// "is_team_game" in the decompile is the two-name TIE of a non-team mode:
	// no draw flag, a second name, and the second score equal to the first
	// [orig: @0x5b7ce9..0x5b7d1a].
	const bool tie = !team_mode && !in.draw && !in.player_names[1].empty() &&
			in.player_scores[1] == in.player_scores[0];

	// The headline at y 300 [orig: @0x5b7d3e..0x5b7e9b].
	{
		EndRoundLine head;
		bool have = false;
		if (in.draw) {
			head = key_line("STROVER34", 300);
			have = true;
		} else if (tie) {
			head = key_line("STROVER35", 300);
			have = true;
		} else if (team_mode) {
			switch (in.winner_team) {
				case 1: head = key_line("STROVER32", 300); have = true; break;
				case 2: head = key_line("STROVER33", 300); have = true; break;
				case 3: head = key_line("STROVER61", 300); have = true; break;
				case 4: head = key_line("STROVER62", 300); have = true; break;
				default: break;
			}
		} else if (!in.player_names[0].empty()) {
			head = key_line("STROVER_PLAYERWIN", 300);
			head.args.push_back(literal_arg(in.player_names[0]));
			have = true;
		}
		// An empty selection (or an empty string) falls to STROVER1 with the
		// "!Mission Completed" fallback; the presenter applies the same
		// empty-string fold when the key resolves to "" [orig: LABEL_30 ->
		// GameText_GetStringWithFallback("Overlays", "STROVER1", ...)].
		if (!have) head = key_line("STROVER1", 300, "!Mission Completed");
		else head.fallback = "!Mission Completed";
		out.push_back(head);
	}

	// The second line at y 350 [orig: the g_GameType switch @0x5b7ea6..0x5b83e7].
	int y = 350;
	{
		EndRoundLine line;
		bool have = false;
		auto winner_name_arg = [&](bool &ok) -> EndRoundArg {
			ok = true;
			if (in.winner_team == 1) return key_arg("STROVER_BLUETEAM");
			if (in.winner_team == 2) return key_arg("STROVER_REDTEAM");
			if (!team_mode && !in.player_names[0].empty()) return literal_arg(in.player_names[0]);
			ok = false;
			return EndRoundArg{};
		};
		auto timed_pair = [&](const char *with_time, const char *without) {
			return in.round_time_remaining_ticks != 0 ? with_time : without;
		};
		switch (in.game_type) {
			case 0x10010u:
			case 0x50010u: {
				if (in.draw) {
					line = key_line("STROVER102", y);
					have = true;
				} else if (in.winner_team == 1 || in.winner_team == 2) {
					line = key_line(timed_pair("STROVER100", "STROVER101"), y);
					line.args.push_back(key_arg(in.winner_team == 1 ? "STROVER_BLUETEAM" : "STROVER_REDTEAM"));
					have = true;
				}
				break;
			}
			case 0x10001u:
			case 0x1u: {
				if (in.draw) {
					line = key_line(team_mode ? "STROVER105" : "STROVER117", y);
					have = true;
				} else if (tie) {
					line = key_line("STROVER118", y);
					line.args.push_back(literal_arg(in.player_names[0]));
					line.args.push_back(literal_arg(in.player_names[1]));
					have = true;
				} else {
					bool ok = false;
					const EndRoundArg who = winner_name_arg(ok);
					if (ok) {
						line = key_line(timed_pair("STROVER103", "STROVER104"), y);
						line.args.push_back(who);
						have = true;
					}
				}
				break;
			}
			case 0x10000u:
			case 0x0u: {
				if (in.draw) {
					line = key_line(team_mode ? "STROVER108" : "STROVER115", y);
					have = true;
				} else if (tie) {
					line = key_line("STROVER116", y);
					line.args.push_back(literal_arg(in.player_names[0]));
					line.args.push_back(literal_arg(in.player_names[1]));
					have = true;
				} else {
					bool ok = false;
					const EndRoundArg who = winner_name_arg(ok);
					if (ok) {
						line = key_line(timed_pair("STROVER106", "STROVER107"), y);
						line.args.push_back(who);
						have = true;
					}
				}
				break;
			}
			case 0x10004u:
			case 0x10008u:
			case 0x8u: {
				if (in.draw) {
					line = key_line(team_mode ? "STROVER111" : "STROVER119", y);
					have = true;
				} else if (tie) {
					line = key_line("STROVER120", y);
					line.args.push_back(literal_arg(in.player_names[0]));
					line.args.push_back(literal_arg(in.player_names[1]));
					have = true;
				} else {
					bool ok = false;
					const EndRoundArg who = winner_name_arg(ok);
					if (ok) {
						line = key_line(timed_pair("STROVER109", "STROVER110"), y);
						line.args.push_back(who);
						have = true;
					}
				}
				break;
			}
			case 0x10002u:
			case 0x90002u: {
				if (in.draw) {
					line = key_line("STROVER114", y);
					have = true;
				} else if (in.winner_team == 1 || in.winner_team == 2) {
					line = key_line(timed_pair("STROVER112", "STROVER113"), y);
					line.args.push_back(key_arg(in.winner_team == 1 ? "STROVER_BLUETEAM" : "STROVER_REDTEAM"));
					have = true;
				}
				break;
			}
			default: {
				// The objective family: won/lost by the local team's side
				// [orig: @0x5b83e7..0x5b842f].
				if ((in.game_type & 0x20000u) != 0 && !in.draw && !in.death_screen) {
					line = key_line(in.winner_team == in.local_team ? "STROVER1" : "STROVER2", y);
					have = true;
				}
				break;
			}
		}
		if (have) {
			out.push_back(line);
			y = 382;
		}
	}

	// The score lines from y + 32, stepping 40 [orig: @0x5b8433..0x5b8560].
	y += 32;
	if ((in.game_type & 0x20000u) == 0) {
		if (team_mode) {
			EndRoundLine blue = literal_line("%s : %ld", y);
			blue.args.push_back(key_arg("STROVER_BLUETEAM", "!Joint Ops Team"));
			blue.args.push_back(number_arg(in.team_scores[0]));
			out.push_back(blue);
			y += 40;
			EndRoundLine red = literal_line("%s : %ld", y);
			red.args.push_back(key_arg("STROVER_REDTEAM", "!Rebel Team"));
			red.args.push_back(number_arg(in.team_scores[1]));
			out.push_back(red);
			y += 40;
		} else {
			for (int i = 0; i < 3; ++i) {
				if (in.player_names[i].empty()) continue;
				EndRoundLine row = literal_line("%s : %ld", y);
				row.args.push_back(literal_arg(in.player_names[i]));
				row.args.push_back(number_arg(in.player_scores[i]));
				out.push_back(row);
				y += 40;
			}
		}
		y += 24;
	}
	// The game-time line [orig: @0x5b8567..0x5b85d8 — hours = t/62/60/60,
	// minutes = t/62/60 (NOT modulo 60: the retail quirk), seconds = t/62 % 60].
	{
		EndRoundLine time = literal_line("%s : %d:%02d:%02d", y);
		time.args.push_back(key_arg("STROVER_GAMETIME", "!Game time"));
		const int32_t t = in.round_time_remaining_ticks;
		time.args.push_back(number_arg(t / 62 / 60 / 60));
		time.args.push_back(number_arg(t / 62 / 60));
		time.args.push_back(number_arg(t / 62 % 60));
		out.push_back(time);
	}
	return out;
}

int stat_field_string_index(int field_id) {
	// [orig: dword_83C840 — (strIndex, fieldId) pairs: 1..13, 22..29 identity,
	//  (33,30), (27,31), 14..21 identity, (34,32)]
	switch (field_id) {
		case 30: return 33;
		case 31: return 27;
		case 32: return 34;
		default: return (field_id >= 1 && field_id <= 29) ? field_id : 0;
	}
}

EndRoundColumnLayout end_round_column_layout(
		int region_left, int region_right,
		const std::vector<std::pair<uint8_t, uint8_t>> &fields, bool show_disabled,
		const std::vector<std::string> &player_names,
		const std::function<int(const std::string &)> &measure,
		const std::function<std::string(const std::string &key, const std::string &fallback)> &resolve) {
	// [orig: Overlay_ComputeStatFieldColumnLayout @0x5b7a10]
	EndRoundColumnLayout out;
	out.left = region_left;
	int name_width = 100;
	for (const std::string &name : player_names) {
		const int w = measure ? measure(name) : 0;
		if (w > name_width) name_width = w;
	}
	int name_padded = name_width + 10;
	int total_field_width = 0;
	for (size_t i = 0; i < fields.size(); ++i) {
		const int field_id = fields[i].first;
		// The show-disabled toggle (dword_28E3D68 || dword_28E3D64) switches
		// every field to the SMALL labels; else only enabled fields list.
		if (!show_disabled && fields[i].second == 0) continue;
		char key[64];
		char fallback[64];
		std::snprintf(key, sizeof(key), show_disabled ? "STROVER_STATFIELDSMALL%02d" : "STROVER_STATFIELD%02d", field_id);
		std::snprintf(fallback, sizeof(fallback), "!%s", key);
		EndRoundColumn col;
		col.field_id = field_id;
		col.field_index = static_cast<int>(i);
		col.label_key = key;
		col.label_fallback = fallback;
		const std::string label = resolve ? resolve(col.label_key, col.label_fallback) : col.label_fallback;
		int w = measure ? measure(label) : 0;
		if (w < 40) w = 40;
		col.width = w + 10;
		total_field_width += col.width;
		out.columns.push_back(col);
	}
	const int available = region_right - region_left;
	const int remaining = available - total_field_width - name_padded;
	if (remaining > 0) {
		out.left += remaining >> 2;
		const int extra = (remaining >> 1) / (static_cast<int>(out.columns.size()) + 1);
		for (EndRoundColumn &c : out.columns) c.width += extra;
		name_padded += extra;
	}
	out.name_width = name_padded;
	int x = out.left + name_padded;
	for (EndRoundColumn &c : out.columns) {
		c.x = x;
		c.center_x = x + c.width / 2;
		x += c.width;
	}
	out.right = x;
	return out;
}

} // namespace opennova::hud
