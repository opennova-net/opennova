#include <runtime/hud/hud_scoreboard.h>

#include <base/gameprofile/game_type.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace opennova::hud {

namespace {

// The witnessed APPEND ORDER — deliberately not the bit order. Each entry is
// (mask, glyph) in the sequence the drawer tests them
// [orig: the chain @0x423f29-0x4240a8].
struct GlyphToken {
	uint16_t mask;
	char glyph;
};
constexpr GlyphToken kGlyphs[] = {
		{ 0x0001u, 'R' }, { 0x0008u, 'r' }, { 0x0004u, 'g' }, { 0x0002u, 'b' },
		{ 0x0800u, 'y' }, { 0x0010u, 'Z' }, { 0x0020u, 'C' }, { 0x0040u, '+' },
		{ 0x0080u, 'T' }, { 0x0100u, 's' }, { 0x0200u, 't' }, { 0x1000u, 'A' },
};
// The one glyph that lands AFTER the closing bracket [orig: @0x4240cb-0x4240e5].
constexpr uint16_t kTrailingS = 0x0400u;

// [orig: GameText_GetString("Overlays", key) @0x423d96..0x423dbe]
constexpr const char *kClassNameKeys[kScoreboardClassNameCount] = {
	"STROVR_MEDIC", "STROVR_SNIPER", "STROVR_GUNNER", "STROVR_RIFLEMAN",
	"STROVR_ENGINEER", "STROVR_UNKNOWN",
};

// The game types the team-score switch names [orig: the cases @0x4232bf].
constexpr uint32_t kGameTypeTdm = 0x10000u;
constexpr uint32_t kGameTypeTkoth = 0x10001u;
constexpr uint32_t kGameTypeAttackDefend = 0x10002u;
constexpr uint32_t kGameTypeCtf = 0x10004u;
constexpr uint32_t kGameTypeFlagBall = 0x10008u;
constexpr uint32_t kGameTypeSearchDestroy = 0x90002u;

std::string format_line(const char *fmt, int a, const std::string &s) {
	char buf[256];
	std::snprintf(buf, sizeof(buf), fmt, a, s.c_str());
	return buf;
}

// TKOTH's per-team line: the team's remaining hold time, with the hold count
// when nonzero [orig: "%2i:%02i %s (%i)" / "%2i:%02i %s" @0x42346a/@0x4234a1;
// remaining = 60T - score1, the truncating /60 and %60].
std::string koth_team_line(int time_limit, const ScoreboardTeamScore &t,
		const std::string &name) {
	const int remaining = 60 * time_limit - t.score1;
	char buf[256];
	if (t.koth_hold != 0)
		std::snprintf(buf, sizeof(buf), "%2i:%02i %s (%i)", remaining / 60, remaining % 60,
				name.c_str(), t.koth_hold);
	else
		std::snprintf(buf, sizeof(buf), "%2i:%02i %s", remaining / 60, remaining % 60,
				name.c_str());
	return buf;
}

// The CTF-family line [orig: "%i %s %i)" @0x4238b0/@0x4238f2].
std::string of_line(int score, const std::string &label, int flag) {
	char buf[256];
	std::snprintf(buf, sizeof(buf), "%i %s %i)", score, label.c_str(), flag);
	return buf;
}

} // namespace

std::string scoreboard_status_suffix(bool enabled, uint16_t status_flags) {
	// [orig: `cmp g_ScoreboardStatusSuffixEnabled, 0` @0x423ef1 — the whole
	// append chain rides the gate; a zero word under it is " []"]
	if (!enabled) return std::string();
	std::string out = " [";
	for (const GlyphToken &t : kGlyphs)
		if ((status_flags & t.mask) != 0u) out.push_back(t.glyph);
	out.push_back(']');
	if ((status_flags & kTrailingS) != 0u) out.push_back('S');
	return out;
}

const char *scoreboard_class_name_key(int index) {
	return index >= 0 && index < kScoreboardClassNameCount ? kClassNameKeys[index] : "";
}

const std::string &scoreboard_class_name(const ScoreboardClassNames &names,
		uint8_t player_class) {
	// [orig: `switch (player_entity->playerClass)` @0x423d8a — 5..9 named,
	// every other value the unknown row]
	if (player_class >= 5u && player_class <= 9u) return names[player_class - 5u];
	return names[kScoreboardClassNameCount - 1];
}

std::string ScoreboardRowComposer::compose(const ScoreboardEntry &e) {
	char buf[256];
	const std::string tail = e.label;
	if (ctx_.non_team && !e.spectator) {
		if (ctx_.timed) {
			// Solo KOTH's countdown [orig: (60 * T - score) / 60 and % 60 into
			// "%2i:%02i %s<ch>%s<co> [%02ld]" @0x423e7d..0x423ec9]
			const int remaining = 60 * ctx_.time_limit - static_cast<int>(e.score1);
			std::snprintf(buf, sizeof(buf), "%2i:%02i %s<ch>%s<co> [%02d]", remaining / 60,
					remaining % 60, e.name.c_str(), tail.c_str(), static_cast<int>(e.slot_id));
		} else {
			// [orig: "%3i %s<ch>%s<co> [%02ld]" @0x423ee9 — the score is the
			// record's SIGN-EXTENDED read, movsx @0x42fb9d]
			std::snprintf(buf, sizeof(buf), "%3i %s<ch>%s<co> [%02d]",
					static_cast<int>(e.score1), e.name.c_str(), tail.c_str(),
					static_cast<int>(e.slot_id));
		}
	} else {
		// The team arm stashes the same-team class name BEFORE the format
		// — spectators included [orig: `player_entity->Team ==
		// g_LocalPlayerEntity->Team` @0x423d64..0x423d7a; the stash @0x423dc3].
		// Non-team spectators take the spectator arm, which never stashes
		// [orig: @0x423e04].
		if (!ctx_.non_team && e.has_entity && ctx_.local_team >= 0 &&
				e.team == static_cast<uint8_t>(ctx_.local_team))
			pending_class_ = &scoreboard_class_name(names_, e.player_class);
		// [orig: "%s<ch>%s<co> [%02ld]" @0x423ddf / @0x423e2d]
		std::snprintf(buf, sizeof(buf), "%s<ch>%s<co> [%02d]", e.name.c_str(), tail.c_str(),
				static_cast<int>(e.slot_id));
	}
	std::string out(buf);
	out += scoreboard_status_suffix(ctx_.status_suffix, e.status_flags);
	// Only a non-spectator row consumes the stash — the leak
	// [orig: `if (v77 && !spectator)` @0x4240f0; " (%s)" @0x424104; the clear
	// @0x424150].
	if (pending_class_ != nullptr && !e.spectator) {
		out += " (";
		out += *pending_class_;
		out += ')';
		pending_class_ = nullptr;
	}
	return out;
}

ScoreboardTeamPage scoreboard_team_page(int team_count, int frame_counter) {
	// [orig: HUD_DrawKillList @0x423cd0-0x423cf1 — the palette pair for
	// teams 1/2 unless `g_NumTeamsConfig > 2` AND bit 7 of the HUD frame
	// counter, which selects teams 3/4 with the two literal colors]
	ScoreboardTeamPage page;
	if (team_count > 2 && (frame_counter & 0x80) != 0) {
		page.team_a = 3;
		page.team_b = 4;
		page.color_a = kTeamCColor;
		page.color_b = kTeamDColor;
	}
	return page;
}

int scoreboard_column_x(const ScoreboardEntry &e, bool non_team, int ordinal,
                        const ScoreboardTeamPage &page) {
	if (e.spectator) return kColumnSpectatorX;
	if (non_team) return (ordinal & 1) == 0 ? kColumnAX : kColumnBX;
	return e.team == page.team_a ? kColumnAX : kColumnBX;
}

uint32_t scoreboard_row_color(const ScoreboardEntry &e, bool non_team,
                              uint32_t hud_color, const ScoreboardTeamPage &page) {
	if (e.spectator || non_team) return hud_color;
	return e.team == page.team_a ? page.color_a : page.color_b;
}

ScoreboardColumnCounts scoreboard_column_counts(const std::vector<ScoreboardEntry> &rows,
		bool non_team) {
	ScoreboardColumnCounts c;
	int toggle = 0;
	for (const ScoreboardEntry &e : rows) {
		if (e.spectator) {
			++c.spectators; // [orig: @0x423b06 / @0x423b75]
		} else if (non_team) {
			// [orig: the alternating toggle @0x423b7a..0x423b84]
			toggle ^= 1;
			if (toggle) ++c.column_a;
			else ++c.column_b;
		} else if (e.has_entity) {
			// [orig: team 1 || 3 -> A @0x423b35/@0x423b44, 2 || 4 -> B
			// @0x423b3d/@0x423b3f — the live slot's entity required @0x423b20]
			if (e.team == 1 || e.team == 3) ++c.column_a;
			else if (e.team == 2 || e.team == 4) ++c.column_b;
		}
	}
	// [orig: `separator_count && (team1 || team2)` @0x423ba1]
	c.header_rows = (c.spectators > 0 && (c.column_a > 0 || c.column_b > 0))
			? kSpectatorGapRows
			: 0;
	return c;
}

int scoreboard_page_count(const ScoreboardColumnCounts &c, int list_base) {
	// [orig: fild (max + header_rows + spectators) / (fild (490 - base) *
	// flt_7C4C78) -> ceil_impl -> _ftol2_sse; max(.., 1) @0x423bb4..0x423bf1]
	const int rows = std::max(c.column_a, c.column_b) + c.header_rows + c.spectators;
	const double per_page = static_cast<double>(kListBottom - list_base) *
			static_cast<double>(kScoreboardPageScale);
	const int pages = static_cast<int>(std::ceil(static_cast<double>(rows) / per_page));
	return pages < 1 ? 1 : pages;
}

int scoreboard_page_fold(int page, int pages) {
	// [orig: page < 0 -> pages - 1 @0x423c05; page > pages - 1 -> 0 @0x423c0d]
	if (page < 0) return pages - 1;
	if (page > pages - 1) return 0;
	return page;
}

int scoreboard_row_base(int list_base, int page) {
	return list_base - page * (kListBottom - list_base); // [orig: @0x423c19..0x423c1c]
}

std::vector<std::string> scoreboard_team_score_lines(const ScoreboardHeaderInput &in,
		const ScoreboardHeaderText &text) {
	std::vector<std::string> lines;
	const uint32_t g = in.game_type;
	// [orig: `(g_GameType & 0xFFFDFFFF) != 0x10020 && g_GameType >= 2 &&
	// g_GameType != 8` @0x4232b4]
	if ((g & 0xFFFDFFFFu) == 0x10020u || g < 2u || g == 8u) return lines;
	const auto &t = in.teams;
	const bool four = in.team_count == 4; // [orig: `g_ScoreboardTeamCount != 4` @0x42335e]
	switch (g) {
		case kGameTypeTdm:
		case kGameTypeFlagBall:
			// [orig: "%3i %s" t1/STRCLI05, t2/STRCLI06 @0x4232e6/@0x42332f
			// (FlagBall @0x423778/@0x4237c0); the 4-team pair t3/STRCLI17,
			// t4/STRCLI18 @0x423385/@0x4233ce]
			lines.push_back(format_line("%3i %s", t[1].score1, text.team_names[0]));
			lines.push_back(format_line("%3i %s", t[2].score1, text.team_names[1]));
			if (four) {
				lines.push_back(format_line("%3i %s", t[3].score1, text.team_names[2]));
				lines.push_back(format_line("%3i %s", t[4].score1, text.team_names[3]));
			}
			break;
		case kGameTypeTkoth:
			// [orig: @0x423414..0x423704]
			lines.push_back(koth_team_line(in.time_limit, t[1], text.team_names[0]));
			lines.push_back(koth_team_line(in.time_limit, t[2], text.team_names[1]));
			if (four) {
				lines.push_back(koth_team_line(in.time_limit, t[3], text.team_names[2]));
				lines.push_back(koth_team_line(in.time_limit, t[4], text.team_names[3]));
			}
			break;
		case kGameTypeCtf:
		case kGameTypeSearchDestroy:
			// Both lines: each team's score against the OTHER team's ctf byte
			// [orig: (t1.score1, STRCLI07, t2.ctf) @0x42388b..0x4238b0, then
			// (t2.score1, STRCLI08, t1.ctf) @0x4238dc..0x4238f2]
			lines.push_back(of_line(t[1].score1, text.of_team_a, t[2].ctf_flag));
			lines.push_back(of_line(t[2].score1, text.of_team_b, t[1].ctf_flag));
			break;
		case kGameTypeAttackDefend:
			// One line: whichever side the team-2 ctf byte names
			// [orig: `if (dword_A85B14)` @0x423735 -> the 07 line, else the 08 line]
			if (t[2].ctf_flag != 0)
				lines.push_back(of_line(t[1].score1, text.of_team_a, t[2].ctf_flag));
			else
				lines.push_back(of_line(t[2].score1, text.of_team_b, t[1].ctf_flag));
			break;
		default:
			break; // [orig: the default arm @0x423751 draws nothing]
	}
	return lines;
}

std::string scoreboard_flag_carrier_text(const std::string &label, const std::string &name) {
	// [orig: sprintf(buf, label) @0x423964, strcat "  " @0x4239b1, strcat
	// carrier+0xF4 @0x4239ee — the label rides as the format string with no
	// arguments; the shipped text carries no conversion]
	return label + "  " + name;
}

uint32_t scoreboard_flag_carrier_color(uint8_t team, uint32_t hud_color) {
	if (team == 1) return kTeamAColor; // [orig: palette[3] @0x42397c]
	if (team == 2) return kTeamBColor; // [orig: palette[5] @0x423986]
	return hud_color;                  // [orig: g_HUDColors.active @0x42398e]
}

ScoreboardHeaderStrings scoreboard_header_strings(const GameTextLookup &gametext, bool gametext_loaded,
		const GameTextLookup &keyhelp, uint32_t game_type, int players, int spectators) {
	ScoreboardHeaderStrings out;
	// [orig: GameText_GetStringWithFallback @0x423a75]
	out.title = game_text(gametext, kGameTextOverlays, "STROVER_KILLLIST", "!Kill List");
	// [orig: HUD_GetGameTypeOverlayLabel @0x5b8680]
	const char *rung = game_type::overlay_label_key(game_type);
	if (rung[0] != 0) out.game_type_label = game_text(gametext, kGameTextOverlays, rung, "");
	// [orig: "%s %i" @0x4231fb; the spectators' gate @0x42322a, "%s %i" @0x42324a]
	out.players_line = game_text(gametext, kGameTextClient, "STRCLI04", "") + " " + std::to_string(players);
	if (spectators > 0)
		out.spectators_line = game_text(gametext, kGameTextClient, "STRCLI23", "") + " " + std::to_string(spectators);
	// [orig: KeyHelp_GetStringWithFallback @0x51ed40 from @0x424272]
	static constexpr const char *kPageHint = "!PgUp and PgDn to change pages";
	out.footer = gametext_loaded && keyhelp ? keyhelp("Text", "CHANGE_SCREEN", kPageHint) : std::string(kPageHint);
	return out;
}

ScoreboardText scoreboard_text(const GameTextLookup &gametext) {
	ScoreboardText t;
	for (int i = 0; i < kScoreboardClassNameCount; ++i)
		t.class_names[static_cast<size_t>(i)] =
				game_text(gametext, "Overlays", kClassNameKeys[i], "");
	// [orig: STRCLI05 @0x4232d5, STRCLI06 @0x42331b, STRCLI17 @0x423374,
	//  STRCLI18 @0x4233ba, STRCLI07 @0x42389c, STRCLI08 @0x4238ed]
	t.header.team_names[0] = game_text(gametext, "Client", "STRCLI05", "");
	t.header.team_names[1] = game_text(gametext, "Client", "STRCLI06", "");
	t.header.team_names[2] = game_text(gametext, "Client", "STRCLI17", "");
	t.header.team_names[3] = game_text(gametext, "Client", "STRCLI18", "");
	t.header.of_team_a = game_text(gametext, "Client", "STRCLI07", "");
	t.header.of_team_b = game_text(gametext, "Client", "STRCLI08", "");
	t.flag_carrier_label =
			game_text(gametext, "Overlays", "STROVER_FLAGCARRIER", "!FlagCarrier:");
	return t;
}

} // namespace opennova::hud
