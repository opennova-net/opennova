#include "hud/feed_format.h"

#include <cstring>

namespace opennova::hud {

bool feed_event_suppressed(uint8_t event_type) {
	switch (event_type) {
		// The LFP result lines format their WPNames string and return — they
		// call neither the feed sink nor the tip system
		// [orig: 50/51/52/53 @0x62051-0x62084].
		case 50:
		case 51:
		case 52:
		case 53:
			return true;
		// Posts to the tip system only, never the feed
		// [orig: 58 -> CTipSystem_HandleEvent 17 @0x62147].
		case 58:
			return true;
		default:
			return false;
	}
}

uint32_t feed_event_color(uint8_t event_type, const char *strcnd_key, bool own,
                          uint8_t team) {
	// The medic pair first — its color is unconditional [orig: @0x426270].
	if (event_type == 38 || event_type == 45) return kFeedColorMedic;
	// Camp events carry literal colors keyed on the team byte
	// [orig: case 59/60 @0x62172..0x62204].
	if (event_type == 59 || event_type == 60)
		return team == 1 ? kFeedColorCampBlue : kFeedColorCampRed;
	// A team-named canned key takes the team palette.
	if (strcnd_key != nullptr) {
		if (std::strstr(strcnd_key, "BLUE") != nullptr) return kFeedColorTeamBlue;
		if (std::strstr(strcnd_key, "RED") != nullptr) return kFeedColorTeamRed;
	}
	return own ? kFeedColorOwnKill : kFeedColorOtherKill;
}

std::string feed_format_line(const std::string &tmpl, const std::string &attacker,
                             const std::string &victim) {
	// [orig: Chat_FormatMessage @0x422C60] — `$A` attacker, `$B` victim. All
	// spacing rides the template; nothing is inserted around the names.
	std::string out;
	out.reserve(tmpl.size() + attacker.size() + victim.size());
	for (size_t i = 0; i < tmpl.size(); ++i) {
		if (tmpl[i] == '$' && i + 1 < tmpl.size()) {
			if (tmpl[i + 1] == 'A') {
				out += attacker;
				++i;
				continue;
			}
			if (tmpl[i + 1] == 'B') {
				out += victim;
				++i;
				continue;
			}
		}
		out.push_back(tmpl[i]);
	}
	return out;
}

std::string feed_camp_key(uint8_t event_type, uint8_t team) {
	const char *base = nullptr;
	if (event_type == 59) base = "STRCND_FULLYCAMPED";
	else if (event_type == 60) base = "STRCND_LOSTCAMP";
	if (base == nullptr) return std::string();
	// Retail emits for team 1/2 only — no else branch [orig: @0x62165/@0x62190].
	if (team == 1) return std::string(base) + "_BLUE";
	if (team == 2) return std::string(base) + "_RED";
	return std::string();
}

} // namespace opennova::hud
