#pragma once

// DFX2-36: dfx2.exe's EXPPOINTS element. The label "eXperience Points: " at the hudpos EXPPOINTS
// anchor in the bold label slot, the latest S2C 0x81 score after it, and under the S2C 0x80 / 0x82
// score tracker its "Session XP" and "Total XP" lines. A 0x81 that sounds the EXP_FANFARE ladder
// (score_fanfare.h) also starts the element's fade: the number pulses by the hudpos percent and
// tints toward the tier's colour, both easing back over 31, 62 or 93 draws. Jointops.exe parses
// the EXPPOINTS line and draws nothing (its four words have no reader), and its 0x81 has no fade,
// so the element is a title's (Title::hud_exp_points). HudFrameCompiler::element_exp_points draws
// it.
// [orig: sub_5A6D20 @0x5A6D20 (dfx2.exe) - g_HUDDeclutterVisible[9] (dword_272501C) @0x5A6EDE,
//  sub_59B7E0 called @0x5A6EE7 between sub_597F80 @0x5A6ED9 and the CLOCK pair @0x5A6EEC;
//  NapiNPClientMsg_ScoreDeltaSound @0x42BEF0 -> @0x422350..0x4223E5, sub_59B730 called
//  @0x4223C5 (dfx2.exe); HUD_ParseHudposToken @0x59F370 (@0x5A0463..0x5A04AF -> dword_27236B4..C0,
//  no reader)]

#include <runtime/hud/game_text_lookup.h>
#include <runtime/hud/score_fanfare.h>

#include <cstdint>
#include <string>

namespace opennova::hud {

// The hudpos EXPPOINTS line's four words: x, y, the hidden dword and the pulse percent (100: no
// pulse) (formats/def DefHudPosDef::exp_points).
// [orig: HUD_ParseHudposToken @0x59DDA0 (@0x59EE93..0x59EEDF) -> dword_2724A2C..dword_2724A38
//  (dfx2.exe)]
struct HudExpPointsRecord {
	int x = 0;
	int y = 0;
	int hidden = 0;
	int pulse_percent = 0;
	bool present = false;
};

// The element's fade: its intensity (1 at the start, less the rate each draw), the rate (1 /
// the tier's duration), the pulse (percent / 100 - 1) and the colour's per-channel distance from
// g_HUDColors.active to the tier's colour (red, green, blue). Floats rounded to single precision
// at every step, as the game's x87 runs (game_font.h glyph_extent carries the witness).
// [orig: flt_2724BC4 / flt_2724BC8 / flt_2724BCC and word_2724BD0 / word_2724BD2 / word_2724BD4
//  (dfx2.exe); HUD_InitOverlaySystem @0x5A3490 (dfx2.exe) zeroes them @0x5A372C..0x5A3764]
struct HudExpPointsFade {
	float intensity = 0.0f;
	float rate = 0.0f;
	float pulse = 0.0f;
	int16_t delta[3] = {0, 0, 0};
};

// A fade's start: nothing while the element is hidden or for a duration of 0; else the intensity
// 1, each channel's distance `color - active` as two zero-extended bytes subtracted in 16 bits, the
// rate 1 / duration and the pulse percent * 0.01 - 1.
// [orig: sub_59B730 @0x59B730 (dfx2.exe) - the hidden test @0x59B731, the duration's 16-bit test
//  @0x59B73E..0x59B746, the 1.0 @0x59B751, the red / green / blue distances @0x59B764..0x59B7AF,
//  the rate @0x59B760..0x59B7B7, the pulse @0x59B7BD..0x59B7CB]
void hud_exp_points_fade_start(HudExpPointsFade &fade, bool hidden, uint16_t duration,
		uint32_t color, uint32_t active, int32_t pulse_percent);

// One draw's step: the intensity less the rate; while it stays above 0, the number's scale is the
// pulse times it plus 1 and its colour g_HUDColors.active with each channel byte plus its
// distance times the intensity, truncated, added in 8 bits; at 0 or below the fade clears to zero
// and the number draws at scale 1 in g_HUDColors.active.
// [orig: sub_59B7E0 @0x59B7E0 (dfx2.exe) - the step @0x59B805..0x59B81D, the test
//  @0x59B823..0x59B82C, the scale @0x59B83B..0x59B85E, the red @0x59B832 / @0x59B862..0x59B891,
//  the green @0x59B86A / @0x59B895..0x59B8BA, the blue @0x59B88A / @0x59B8BE..0x59B8E8, the chop
//  control word `or eax, 0C00h` at each fistp, the clear @0x59B8F6..0x59B91B]
struct HudExpPointsPulse {
	float scale = 1.0f;
	uint32_t color = 0;
};
HudExpPointsPulse hud_exp_points_fade_step(HudExpPointsFade &fade, uint32_t active);

// The fade a tone of the EXP_FANFARE ladder starts: a HIT 31 draws toward g_HUDColors.active (no
// tint), a KILL 62 toward palette entry 4, a HEADSHOT 93 toward entry 5; None starts none.
// [orig: @0x422380..0x4223C5 (dfx2.exe) - `mov eax, 1Fh` / alpha @0x422385..0x422391, 3Eh /
//  dword_24BADF8 @0x42239F..0x4223AB, 5Dh / dword_24BADFC @0x4223B2..0x4223BE;
//  HUD_InitTeamColorTable @0x51C200 (dfx2.exe) - entry 4 0xFFF0F000 @0x51C237, entry 5 0xFFFF5050
//  @0x51C219]
struct HudExpPointsTier {
	uint16_t duration = 0;
	int palette = -1; // -1: g_HUDColors.active
};
HudExpPointsTier hud_exp_points_tier(ScoreTone tone);

// The element's three gametext strings, resolved at the HUD's init.
// [orig: HUD_InitOverlaySystem @0x5A3490 (dfx2.exe) - GameText_GetStringWithFallback("Overlays",
//  "STROVER_HUDEXPPOINTS", "!eXperience Points: ") @0x5A3693..0x5A36A7 (stored @0x5A36BE),
//  "STROVER_HUDSESSIONPTS" / "!Session XP: %d" @0x5A36AF..0x5A36C3 (stored @0x5A36D7),
//  "STROVER_HUDTOTALPTS" / "!Total XP: %d" @0x5A36C8..0x5A36DC (stored @0x5A36F0)]
struct HudExpPointsText {
	std::string label;          // dword_2724BEC
	std::string session_format; // dword_2724BF0
	std::string total_format;   // dword_2724BF4
};
void hud_exp_points_text(const GameTextLookup &lookup, HudExpPointsText &out);

// What the element reads of the frame, the role feed's (inmatch::hud_role_facts) but the strings
// (hud_exp_points_text). A frame whose title draws no element (element false) starts no fade and
// draws nothing.
struct HudExpPointsState {
	bool element = false; // Title::hud_exp_points
	// The latest S2C 0x81 score (replication::ClientScoreFeedback::score).
	// [orig: dword_A7BA40 read @0x59B998 (dfx2.exe)]
	int32_t score = 0;
	// The S2C 0x80 / 0x82 score tracker (replication::ClientScoreTracker): its valid word, the
	// session line's points (the entry's gain since its first 0x82) and the total line's (the last
	// 0x82's value). [orig: dword_A7BA4C @0x59BA38, dword_A7BA58 @0x59BA40, dword_A7BA54 @0x59BA48
	// (dfx2.exe)]
	bool tracker_valid = false;
	int32_t session_points = 0;
	int32_t total_points = 0;
	// The 0x81 fanfare arms the replica counted, and the last one's tone: each new arm starts
	// the fade (replication::ClientScoreFeedback::fanfare_arms).
	uint32_t fade_arms = 0;
	ScoreTone fade_tone = ScoreTone::None;
	HudExpPointsText text;
};

} // namespace opennova::hud
