#include <runtime/hud/end_round_overlay.h>
#include <runtime/hud/hud_game_text.h>
#include <base/io/tick_rate.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

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
	// [orig: HUD_DrawEndRoundStatsOverlay @0x5b7cd0]
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
		// No selection falls straight to STROVER1 with the "!Mission
		// Completed" fallback. A SELECTED key that resolves to an empty
		// string re-looks-up Overlays/STROVER1 at draw time (the fold marker;
		// the presenter applies it) — not the line's own fallback, which
		// covers only a missing key. [orig: the empty-headline test @0x5b7e59 ->
		// GameText_GetStringWithFallback("Overlays", "STROVER1",
		// "!Mission Completed") @0x5b7e5b]
		if (!have) head = key_line("STROVER1", 300, "!Mission Completed");
		else {
			head.fallback = "!Mission Completed";
			head.fold = EndRoundEmptyFold::kHeadlineStrover1;
		}
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
			// A selected second line that resolves to an empty string draws
			// nothing and leaves y at 350 — the presenter collapses it via the
			// fold marker so the score lines start at 382, not 414.
			// [orig: the empty-second-line test @0x5b83e5 — draw @0x5b83fe and the y=382 store
			//  @0x5b8406 run only when text_buf[0]]
			line.fold = EndRoundEmptyFold::kCollapse;
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
		const int32_t seconds = t / io::kTicksPerSecondInt;
		time.args.push_back(number_arg(seconds / 60 / 60));
		time.args.push_back(number_arg(seconds / 60));
		time.args.push_back(number_arg(seconds % 60));
		out.push_back(time);
	}
	return out;
}

namespace {

// GameText_GetStringWithFallback's shape: the key resolves when present and
// non-empty, else the fallback with its leading "!" marker stripped; an
// empty key is a literal.
std::string resolve_text(const EndRoundTextLookup &lookup, const std::string &key,
		const std::string &fallback, const std::string &literal) {
	if (key.empty()) return literal;
	std::string value;
	if (lookup && lookup(key, value) && !value.empty()) return value;
	if (!fallback.empty() && fallback[0] == '!') return fallback.substr(1);
	return fallback;
}

bool key_present_but_empty(const EndRoundTextLookup &lookup, const std::string &key) {
	if (key.empty() || !lookup) return false;
	std::string value;
	return lookup(key, value) && value.empty();
}

} // namespace

std::vector<EndRoundResolvedLine> end_round_overlay_resolve(
		const std::vector<EndRoundLine> &lines, const EndRoundTextLookup &lookup) {
	std::vector<EndRoundResolvedLine> out;
	int y_shift = 0;
	for (const EndRoundLine &line : lines) {
		std::string fmt;
		if (line.fold != EndRoundEmptyFold::kNone && key_present_but_empty(lookup, line.key)) {
			if (line.fold == EndRoundEmptyFold::kHeadlineStrover1) {
				fmt = resolve_text(lookup, "STROVER1", "!Mission Completed", "");
			} else {
				// The second line draws nothing and every later line moves
				// up 32 px (retail's y stays 350, so the score lines start
				// at 382) [orig: LABEL_144 @0x5b83e5].
				y_shift = 32;
				continue;
			}
		} else {
			fmt = resolve_text(lookup, line.key, line.fallback, line.literal);
		}
		std::vector<HudTextArg> args;
		args.reserve(line.args.size());
		for (const EndRoundArg &a : line.args) {
			HudTextArg r;
			r.is_number = a.is_number;
			r.number = a.number;
			if (!a.is_number) r.text = resolve_text(lookup, a.key, a.fallback, a.literal);
			args.push_back(std::move(r));
		}
		EndRoundResolvedLine resolved;
		// The row's sprintf over its own argument list: %s, %d / %i, %ld,
		// %02d (hud_game_text.h hud_sprintf).
		resolved.text = args.empty() ? fmt : hud_sprintf(fmt, args);
		resolved.y = line.y - y_shift;
		out.push_back(std::move(resolved));
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

void EndRoundTransition::reset() {
	// [orig: Game_InitMissionRoundState @0x5b71b0 clears byte_28E561C/D, called
	//  from Game_StartMission @0x524360 (the call @0x525903)]
	header_seen = false;
	header_edge_ms = 0;
	stat_opened = false;
}

EndRoundTransitionStep EndRoundTransition::step(bool header_known, bool board_known,
		uint32_t now_ms) {
	EndRoundTransitionStep out;
	if (!header_known) {
		// The announcement went away (a new round, a departed session): the
		// latches clear with it.
		if (header_seen) {
			reset();
			out.reset = true;
		}
		return out;
	}
	if (!header_seen) {
		// The first pass: byte_28E561C and t0 [orig: @0x5b8600 first pass].
		header_seen = true;
		header_edge_ms = now_ms;
		stat_opened = false;
		out.announced = true;
	}
	if (stat_opened) {
		// The locret @0x5b864a once byte_28E561D is set: no teardown, no overlay.
		return out;
	}
	// Every PRE-STAT pass tears the scene down and draws the overlay; the
	// board + 6000 ms gate opens stat.mnu once [orig: @0x5b8615..0x5b862a].
	out.pre_stat = true;
	if (board_known &&
			static_cast<int32_t>(now_ms - header_edge_ms) >= kEndRoundStatScreenDelayMsec) {
		stat_opened = true;
		out.open_stat = true;
	}
	return out;
}

} // namespace opennova::hud
