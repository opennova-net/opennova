#include "hud/hud_scoreboard.h"

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

} // namespace

std::string scoreboard_status_glyphs(uint16_t status_flags) {
	// Zero word -> no suffix: exact for the SU gate's OFF default (the parser
	// zeroes the words then [orig: @0x42fbfb]); the gate-ON empty " []" is a
	// residual with the gate itself (hud_scoreboard.h).
	if (status_flags == 0u) return std::string();
	std::string out = " [";
	for (const GlyphToken &t : kGlyphs)
		if ((status_flags & t.mask) != 0u) out.push_back(t.glyph);
	out.push_back(']');
	if ((status_flags & kTrailingS) != 0u) out.push_back('S');
	return out;
}

std::string scoreboard_row_text(const ScoreboardEntry &e, bool non_team) {
	char buf[192];
	if (non_team && !e.spectator) {
		// [orig: "%3i %s<ch>%s<co> [%02ld]" @0x7c4bc4 — the score is the
		// record's SIGN-EXTENDED read, movsx @0x42fb9d]
		std::snprintf(buf, sizeof(buf), "%3i %s [%02d]",
				static_cast<int>(static_cast<int16_t>(e.score1)),
				e.name.c_str(), static_cast<int>(e.slot_id));
	} else {
		// [orig: "%s<ch>%s<co> [%02ld]" @0x7c4c00 — team rows carry no score,
		// and the spectator arm takes this format in BOTH modes @0x423e04]
		std::snprintf(buf, sizeof(buf), "%s [%02d]", e.name.c_str(),
				static_cast<int>(e.slot_id));
	}
	return std::string(buf) + scoreboard_status_glyphs(e.status_flags);
}

int scoreboard_column_x(const ScoreboardEntry &e, bool non_team, int ordinal) {
	if (e.spectator) return kColumnSpectatorX;
	if (non_team) return (ordinal & 1) == 0 ? kColumnAX : kColumnBX;
	return e.team == 1 ? kColumnAX : kColumnBX;
}

uint32_t scoreboard_row_color(const ScoreboardEntry &e, bool non_team,
                              uint32_t hud_color) {
	if (e.spectator || non_team) return hud_color;
	return e.team == 1 ? kTeamAColor : kTeamBColor;
}

} // namespace opennova::hud
