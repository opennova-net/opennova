#include "hud/feed_format.h"

#include <cctype>
#include <cstdio>

namespace opennova::hud {

bool feed_event_suppressed(uint8_t event_type) {
	switch (event_type) {
		// The LFP result lines format their WPNames string and return — they
		// call neither the feed sink nor the tip system
		// [orig: 50 @0x42702E, 51 @0x427084, 52 @0x4270DA, 53 @0x42716D].
		case 50:
		case 51:
		case 52:
		case 53:
			return true;
		// Posts to the tip system only, never the feed
		// [orig: 58 @0x427202 -> CTipSystem_HandleEvent 17 @0x427286].
		case 58:
			return true;
		default:
			return false;
	}
}

bool feed_event_verbose_only(uint8_t event_type) {
	switch (event_type) {
		// The grey (not-own) branch returns without posting when the verbose
		// toggle is off — 15 event types behind 13 g_MpVerbose2 test sites:
		// cases 1-6 and 13/14/15 and 32/33/34 each own a test, and the
		// spotted/heard/lost trio 10/11/12 funnels through ONE shared test at
		// its common body [orig: @0x426472 (case 1) .. @0x4267C6 (case 15);
		// the 10/11/12 shared test @0x42664A on the @0x42663C body;
		// @0x42668C/@0x4266DD/@0x42670E (32/33/34)]. The friendly-fire trio
		// 7/8/9, the killer-less deaths 22-26, and types 24/49 post
		// unconditionally; 39/45 jump to their canned post before the gate
		// [orig: jmp @0x426454/@0x426468].
		case 1: case 2: case 3:
		case 4: case 5: case 6:
		case 10: case 11: case 12:
		case 13: case 14: case 15:
		case 32: case 33: case 34:
			return true;
		default:
			return false;
	}
}

uint32_t feed_event_color(uint8_t event_type, bool own, uint8_t team) {
	switch (event_type) {
		// The kill/death families: white when the local player took part,
		// grey otherwise [orig: -1 / -5263441 per case, e.g. @0x426539
		// (case 4), @0x426DF9 (22/23), @0x426E47 (24), @0x427B25 (49)].
		case 1: case 2: case 3:
		case 4: case 5: case 6:
		case 10: case 11: case 12:
		case 13: case 14: case 15:
		case 22: case 23: case 24: case 25: case 26:
		case 49:
			return own ? kFeedColorWhite : kFeedColorGrey;
		// The friendly-fire trio is white for everyone [orig: 7/8/9
		// @0x4265FF pass -1 unconditionally], as are 16/17/18 (they pass
		// g_hudColorTable[0], initialized to -1 [orig: HUD_InitTeamColorTable
		// @0x51F245]) and the announcement lines 27-31/35/36/37.
		case 7: case 8: case 9:
		case 16: case 17: case 18:
		case 27: case 28: case 29: case 30: case 31:
		case 35: case 36: case 37:
			return kFeedColorWhite;
		// The multi-kill bonus trio [orig: 32/33/34 push -256 @0x4266C5].
		case 32: case 33: case 34:
			return kFeedColorBonusKill;
		// The medic pair and 39 — one shared post [orig: 38 @0x42640F /
		// 39 @0x426456 / 45 @0x426442 all reach the -16741138 sink call],
		// plus the SSKB bonus pair [orig: 46/47 post 0xFF008CEE
		// @0x427A68/@0x427AD8].
		case 38: case 39: case 45:
		case 46: case 47:
			return kFeedColorMedic;
		// [orig: case 40 @0x4263E8 posts -65536].
		case 40:
			return kFeedColorRed;
		// PSP/LFP: the BLUE lines post -16732161, the RED lines -65536
		// [orig: 41 @0x42758C / 54 @0x4274E2 (blue post @0x427519) vs
		// 42 @0x4276E0 (red post @0x427717) / 55/57; taken: 43/56 vs 44].
		case 41: case 43: case 54: case 56:
			return kFeedColorBlue;
		case 42: case 44: case 55: case 57:
			return kFeedColorRed;
		// [orig: case 48 @0x427AE5 posts -32768].
		case 48:
			return kFeedColorMortar;
		// Camp events color on the team byte [orig: 59 @0x4272D7 /
		// 60 @0x4273DC — team 1 posts -16732161, team 2 -65536].
		case 59: case 60:
			return team == 1 ? kFeedColorBlue : kFeedColorRed;
		default:
			return kFeedColorWhite;
	}
}

namespace {

// The ported String_ReplaceAllCaseInsensitive [orig: @0x422970]: every
// case-insensitive occurrence is replaced and the scan resumes AFTER the
// inserted text [orig: cursor += strlen(replacement) @0x422ADD] — a name
// containing the needle is not re-matched within the same pass.
void replace_all_ci(std::string &haystack, const char *needle,
		const std::string &replacement) {
	const size_t needle_len = std::char_traits<char>::length(needle);
	size_t pos = 0;
	while (pos + needle_len <= haystack.size()) {
		bool match = true;
		for (size_t i = 0; i < needle_len; ++i) {
			if (std::toupper(static_cast<unsigned char>(haystack[pos + i])) !=
					std::toupper(static_cast<unsigned char>(needle[i]))) {
				match = false;
				break;
			}
		}
		if (match) {
			haystack.replace(pos, needle_len, replacement);
			pos += replacement.size();
		} else {
			++pos;
		}
	}
}

// The single-`%s` fill of retail's sprintf-composed lines, bounds-safe: the
// first `%s` takes `arg`, everything else is copied through. Retail hands the
// game-data template straight to sprintf; the templates carry exactly one
// `%s`, so the visible result is identical.
std::string sprintf_first_s(const std::string &tmpl, const std::string &arg) {
	const size_t pos = tmpl.find("%s");
	if (pos == std::string::npos) return tmpl;
	std::string out;
	out.reserve(tmpl.size() + arg.size());
	out.append(tmpl, 0, pos);
	out.append(arg);
	out.append(tmpl, pos + 2, std::string::npos);
	return out;
}

} // namespace

std::string feed_format_line(const std::string &tmpl, const std::string &attacker,
		const std::string &victim, const std::string &extra,
		const std::string &bonus_tmpl) {
	// [orig: Chat_FormatMessage @0x422C60] — the bonus re-compose first
	// [orig: sprintf(haystack, STRCND48, format, extra) @0x422CA2], then `$A`
	// [orig: @0x422CB4], then `$B` [orig: @0x422CD6], each a sequential
	// case-insensitive replace-all over the whole line.
	std::string out = tmpl;
	if (!extra.empty() && !bonus_tmpl.empty()) {
		out = sprintf_first_s(sprintf_first_s(bonus_tmpl, tmpl), extra);
	}
	replace_all_ci(out, "$A", attacker);
	replace_all_ci(out, "$B", victim);
	return out;
}

std::string feed_format_camp_line(const std::string &tmpl,
		const std::string &wpname) {
	// [orig: sprintf(msg_buffer, camp_tmpl, wpname) @0x427327/@0x42736B].
	return sprintf_first_s(tmpl, wpname);
}

std::string feed_camp_key(uint8_t event_type, uint8_t team) {
	const char *base = nullptr;
	if (event_type == 59) base = "STRCND_FULLYCAMPED";
	else if (event_type == 60) base = "STRCND_LOSTCAMP";
	if (base == nullptr) return std::string();
	// Retail emits for team 1/2 only — no else branch [orig: the team tests
	// @0x4272F4/@0x427338 (case 59) and @0x4273F9/@0x42743D (case 60)].
	if (team == 1) return std::string(base) + "_BLUE";
	if (team == 2) return std::string(base) + "_RED";
	return std::string();
}

std::string feed_camp_wpname_key(uint8_t level_index) {
	// [orig: sprintf(key, "STRWPNAME%03d", v141 + 1) @0x4272EC/@0x4273F1].
	char key[16];
	std::snprintf(key, sizeof(key), "STRWPNAME%03d",
			static_cast<int>(level_index) + 1);
	return std::string(key);
}

ChatSink chat_channel_sink(int channel) {
	// [orig: Chat_DispatchToChannel @0x42b910 — case 0 and the default fall
	//  to Chat_AddDebugMessage @0x42bb0c; 8 -> CMessageQueue_Enqueue @0x42bab8;
	//  14 -> Chat_AddMessageChannel3 @0x42bb01; 1..7/9..13 ->
	//  Chat_AddMessageChannel1; 13 ALSO calls HUD_SetTrackedEntityTarget(sender)
	//  before posting (Phase W, no site address taken) -- that target write is a
	//  world leg, not a ring leg, and is not folded here]
	switch (channel) {
		case 1: case 2: case 3: case 4: case 5: case 6: case 7:
		case 9: case 10: case 11: case 12: case 13:
			return ChatSink::Chat;
		case 8:
			return ChatSink::Queue;
		case 14:
			return ChatSink::Channel3;
		default:
			return ChatSink::System;
	}
}

uint32_t chat_channel_color(int channel) {
	// [orig: the per-case colour pushes — 1/4/5 g_hudColorLightBlue
	//  @0x42ba5c; 9 dword_24C184C @0x42b9a5; 2 dword_24C183C @0x42b9c2;
	//  11 dword_24C1860 @0x42ba26; 3 `color` @0x42ba43; 7 dword_24C1854
	//  @0x42baa2; 12 dword_24C1850 @0x42baea; 0/6/10/13/14 and the default
	//  g_hudColorTable[0] @0x42bb0c/@0x42ba86/@0x42b9de/@0x42bb01]
	switch (channel) {
		case 1: case 4: case 5:
			return kHudColorLightBlue;
		case 2:
			return kHudColorGreen;
		case 3:
			return kHudColorYellow;
		case 7:
			return kHudColorOrange;
		case 9:
			return kHudColorSalmon;
		case 11:
			return kHudColorCyan;
		case 12:
			return kHudColorMagenta;
		default:
			return kHudColorWhite;
	}
}

} // namespace opennova::hud
