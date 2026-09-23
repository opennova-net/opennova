#include <runtime/hud/feed_format.h>

#include <cctype>
#include <cstdio>
#include <utility>

namespace opennova::hud {

// event_type classification — the 0x426270 switch. The cases that resolve
// attacker+victim (HUD_FormatKillEventMessage with both) are kills; the
// flag/zone/camp/base cases are objectives; killer-less deaths are SelfDeath
// (their victim/aux slots are literal zero on the wire) and the two medic
// lines carry their own color. Corrected 2026-08-19: 38/39/45 were misfiled
// as kills and 1/2/3 + 22/23/25/26 as Other by the earlier structural read.
GameEventKind game_event_kind(uint8_t t) {
	switch (t) {
	case 4: case 5: case 6: case 7: case 8: case 9:
	case 10: case 11: case 12: case 13: case 14: case 15:
	case 24: case 32: case 33: case 34: case 49:
		return GameEventKind::Kill;
	// Deaths with no killer — the handler leaves victim/aux zero
	// [orig: GameEvent_PlayerDeath @0x516DD0].
	case 1: case 2: case 3: case 22: case 23: case 25: case 26:
		return GameEventKind::SelfDeath;
	// The medic trio, previously misfiled as kills by this structural read.
	// 39 has no emitter in the image but the handler files it with 38/45 —
	// all three share the one 0xFF008CEE post [orig: cases 38 @0x42640F /
	// 39 @0x426456 / 45 @0x426442 fall into the shared sink call].
	case 38: case 39: case 45:
		return GameEventKind::Medic;
	case 19: case 20: case 21:
	case 41: case 42: case 43: case 44:
	case 50: case 51: case 52: case 53:
	case 54: case 55: case 56: case 57: case 58: case 59: case 60:
		return GameEventKind::Objective;
	default:
		return GameEventKind::Other;
	}
}

// The witnessed "Canned Msg" string key for an event_type, where the handler
// uses a single deterministic key. Types that pick the string by team/gametype
// at runtime (19/20/21/50-53/58) return nullptr. [orig: 0x426270 switch]
const char *game_event_strcnd_key(uint8_t t) {
	switch (t) {
	case 1: return "STRCND01"; case 2: return "STRCND02"; case 3: return "STRCND03";
	case 4: return "STRCND04"; case 5: return "STRCND05"; case 6: return "STRCND06";
	case 7: case 8: case 9: return "STRCND07";
	case 10: case 11: case 12: return "STRCND08";
	case 13: return "STRCND09"; case 14: return "STRCND10"; case 15: return "STRCND11";
	case 16: case 17: case 18: return "STRCND12";
	case 22: case 23: return "STRCND19";
	case 24: return "STRCND22"; case 25: return "STRCND28"; case 26: return "STRCND29";
	case 27: return "STRCND33"; case 28: return "STRCND34"; case 29: return "STRCND31";
	case 30: return "STRCND32"; case 31: return "STRCND35";
	case 32: return "STRCND36"; case 33: return "STRCND37"; case 34: return "STRCND38";
	case 35: return "STRCND39"; case 36: return "STRCND40"; case 37: return "STRCND41";
	case 38: return "STRCND42"; case 39: return "STRCND43"; case 40: return "STRCND44";
	case 41: return "STRCND_PSP_BLUEWARNING"; case 42: return "STRCND_PSP_REDWARNING";
	case 43: return "STRCND_PSP_BLUETAKEN";   case 44: return "STRCND_PSP_REDTAKEN";
	case 45: return "STRCND45"; case 48: return "STRCND46"; case 49: return "STRCND47";
	case 54: return "STRCND_LFP_BLUEWARNING"; case 55: return "STRCND_LFP_REDWARNING";
	case 56: return "STRCND_LFP_BLUETAKEN";   case 57: return "STRCND_LFP_REDTAKEN";
	case 59: return "STRCND_FULLYCAMPED";     case 60: return "STRCND_LOSTCAMP";
	default: return nullptr;
	}
}

void feed_event_rows(const FeedEventInput *events, std::size_t count,
                     const FeedContext &context,
                     const FeedActorLookup &actor_of, std::vector<FeedRow> &out) {
	const uint16_t self_handle = context.self_handle;
	const bool mp_verbose = context.mp_verbose;
	const auto actor = [&actor_of](uint8_t index) -> FeedActor {
		return index == 0xFF ? FeedActor{} : actor_of(index);
	};
	const auto name = [&actor](uint8_t index) { return actor(index).name; };
	for (std::size_t i = 0; i < count; ++i) {
		const FeedEventInput &ev = events[i];
		if (feed_event_suppressed(ev.event_type)) continue;
		// Camp events reuse the slots: attacker is the LEVEL index and victim
		// is the TEAM byte, and their key gets a client-side team suffix
		// [orig: case 59 @0x4272D7 / case 60 @0x4273DC].
		const bool camp = feed_event_is_camp(ev.event_type);
		const bool own = !camp && self_handle != 0xFFFF &&
				(static_cast<uint16_t>(ev.attacker_index) == self_handle ||
				 static_cast<uint16_t>(ev.victim_index) == self_handle);
		if (!own && !mp_verbose && feed_event_verbose_only(ev.event_type)) continue;
		const std::string camp_key =
				camp ? feed_camp_key(ev.event_type, ev.victim_index) : std::string();
		if (camp && camp_key.empty()) continue;   // team outside 1/2 draws nothing
		const uint8_t team = camp ? ev.victim_index : actor(ev.attacker_index).team;
		const char *key = camp ? camp_key.c_str() : game_event_strcnd_key(ev.event_type);
		// [orig: NetPacket_HandleGameEvent @ 0x426270, cases 19/20/21/46/47]
		switch (ev.event_type) {
		case 19:
			if (context.game_type == 8) key = "STRCND49";
			else if (team == 1) key = "STRCND14";
			else if (team == 2) key = "STRCND13";
			else if (team == 3) key = "STRCND25";
			else if (team == 4) key = "STRCND26";
			break;
		case 20:
			if (context.game_type == 8 || context.game_type == 65544) key = "STRCND27";
			else if (team == 1) key = "STRCND16";
			else if (team == 2) key = "STRCND15";
			break;
		case 21:
			if (context.game_type == 8) key = "STRCND50";
			else if (team == 1) key = "STRCND18";
			else if (team == 2) key = "STRCND17";
			break;
		case 46: key = ev.pos_x <= 1 ? "STRCND_SSKB1" : "STRCND_SSKBX"; break;
		case 47: key = ev.pos_x <= 1 ? "STRCND_YRSSKB1" : "STRCND_YRSSKBX"; break;
		default: break;
		}
		if (key == nullptr) continue;
		FeedRow row;
		row.event_type = ev.event_type;
		row.kind = ev.kind;
		row.camp = camp;
		row.own = own;
		row.announce = own && ((ev.event_type >= 1 && ev.event_type <= 18) ||
				(ev.event_type >= 22 && ev.event_type <= 26) ||
				(ev.event_type >= 32 && ev.event_type <= 34) || ev.event_type == 49);
		row.key = key;
		if (camp) {
			// The camp template's %s takes the WPNames string of the level
			// slot — index PLUS ONE [orig: sprintf @0x4272EC/@0x4273F1].
			row.wpname_key = feed_camp_wpname_key(ev.attacker_index);
		} else {
			row.attacker = name(ev.attacker_index);
			row.victim = name(ev.victim_index);
			// The aux slot carries the bonus-credited player; only when that is
			// the LOCAL player does retail re-compose the line through STRCND48
			// "%s - Bonus for %s" with their name [orig: the 4th
			// HUD_FormatKillEventMessage arg @0x422F5F -> the sprintf @0x422CA2].
			if (self_handle != 0xFFFF && ev.aux_index != 0xFF &&
					static_cast<uint16_t>(ev.aux_index) == self_handle) {
				row.extra = name(ev.aux_index);
			}
		}
		if (ev.event_type >= 19 && ev.event_type <= 21) {
			row.victim.clear();
			row.extra.clear();
		}
		if (ev.event_type == 46 || ev.event_type == 47) {
			row.extra.clear();
			row.victim_is_value = true;
			if (ev.event_type == 46) row.victim = std::to_string(ev.pos_x);
			else {
				row.attacker_is_value = true;
				row.attacker = std::to_string(ev.pos_x);
				row.victim.clear();
			}
		}
		row.color = feed_event_color(ev.event_type, own, team);
		if (ev.event_type == 20 && context.game_type == 65544) {
			switch (team) {
			case 1: row.color = kFeedColorBlue; break;
			case 2: row.color = kFeedColorRed; break;
			case 3: row.color = 0xFFFFFF00u; break;
			case 4: row.color = 0xFFFF027Fu; break;
			default: break;
			}
		}
		out.push_back(std::move(row));
	}
}

std::string feed_format_row(const FeedRow &row, const std::string &tmpl,
        const std::string &unknown, const std::string &bonus_tmpl, const std::string &wpname) {
	if (row.camp) return feed_format_camp_line(tmpl, wpname);
	return feed_format_line(tmpl,
			row.attacker.empty() && !row.attacker_is_value ? unknown : row.attacker,
			row.victim.empty() && !row.victim_is_value ? unknown : row.victim,
			row.extra, bonus_tmpl);
}

std::string feed_row_line(const FeedRow &row, const GameTextLookup &gametext) {
	const std::string tmpl = game_text(gametext, "Canned Msg", row.key.c_str(), "");
	if (tmpl.empty()) return std::string();
	const std::string wpname = row.camp ? game_text(gametext, "WPNames", row.wpname_key.c_str(), "")
										: std::string();
	const std::string bonus_tmpl = row.extra.empty() ? std::string()
													  : game_text(gametext, "Canned Msg", "STRCND48", "");
	// A missing actor formats as the Client fallback string
	// [orig: @0x422DDA/@0x422E91 -> GameText_GetString("Client", "STRCLI01")].
	const std::string unknown = game_text(gametext, "Client", "STRCLI01", "");
	return feed_format_row(row, tmpl, unknown, bonus_tmpl, wpname);
}

void KillAnnouncement::record(const std::string &line, uint32_t now) {
	text = line.substr(0, 255);
	tick = now;
}
bool KillAnnouncement::visible(uint32_t now) const {
	return tick != 0 && !text.empty() && static_cast<int32_t>(now - tick) <= 186;
}
void KillAnnouncement::expire(uint32_t now) {
	if (tick != 0 && !text.empty() && static_cast<int32_t>(now - tick) > 186) tick = 0;
}

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
	if (event_type == kFeedEventFullyCamped) base = "STRCND_FULLYCAMPED";
	else if (event_type == kFeedEventLostCamp) base = "STRCND_LOSTCAMP";
	if (base == nullptr) return std::string();
	// Retail emits for team 1/2 only — no else branch [orig: the team tests
	// @0x4272F4/@0x427338 (case 59) and @0x4273F9/@0x42743D (case 60)].
	if (team == 1) return std::string(base) + "_BLUE";
	if (team == 2) return std::string(base) + "_RED";
	return std::string();
}

std::string strip_inline_tags(const std::string &text) {
    // [orig: Chat_StripHtmlTags @0x4983f0]: on '<' (0x3C) skip to the next
    // '>' (0x3E) and drop the span; an unterminated tag runs to the end and
    // is dropped with it; every other byte copies through.
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const char ch = text[i];
        if (ch == '<') {
            const size_t close = text.find('>', i + 1);
            if (close == std::string::npos) break;
            i = close + 1;
            continue;
        }
        out += ch;
        ++i;
    }
    return out;
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
	//  to Chat_AddMessageChannel2 @0x42bb0c; 8 -> CMessageQueue_Enqueue @0x42bab8;
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
