// The HUD's game-text compositions — see hud_game_text.h for the witnesses.
// [orig: HUD_GetWaypointName @0x594630; HUD_DisplayTriggeredText @0x51f190]

#include <runtime/hud/hud_game_text.h>

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdio>

namespace opennova::hud {

std::string waypoint_display_name(int name_id, const GameTextLookup &mission,
		const GameTextLookup &gametext) {
	char key[32];
	std::snprintf(key, sizeof(key), "STRWPNAME%03d", name_id);
	const std::string name = game_text(mission, "WPNames", key, "");
	// Empty or the literal "null" falls back to the gametext default
	// [orig: @0x59477b].
	if (name.empty() || strutil::iequals(name.c_str(), "null"))
		return game_text(gametext, kGameTextWPNames, "STRWPNAMEDEFAULT", "");
	return name;
}

std::string subgoal_message(bool lost, int header_id, const GameTextLookup &mission) {
	char key[32];
	std::snprintf(key, sizeof(key), lost ? "STRLOSEMSG%03d" : "STRWINMSG%03d", header_id);
	return game_text(mission, lost ? "LoseConditions" : "WinConditions", key, "");
}

std::string objective_header(const GameTextLookup &gametext) {
	return game_text(gametext, kGameTextMisc, "STRMISC_NEWOBJECTIVE", "");
}

std::string objective_directive(bool win, int header_id, const GameTextLookup &mission) {
	char key[32];
	std::snprintf(key, sizeof(key), win ? "STRWINDIRECTIVE%03d" : "STRLOSEDIRECTIVE%03d",
			header_id);
	std::string line = game_text(mission, win ? "WinConditions" : "LoseConditions", key, "");
	if (line.size() <= 1) line.clear();
	return line;
}

std::string triggered_text(int text_id, const GameTextLookup &mission) {
	char key[32];
	std::snprintf(key, sizeof(key), "ID%03d", text_id);
	return game_text(mission, "Triggered Text", key, "");
}

std::string weapon_display_name(const std::string &weapon_id, const GameTextLookup &gametext) {
	if (weapon_id.empty()) return std::string();
	return game_text(gametext, kGameTextWepDes, weapon_id.c_str(), "");
}

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// One conversion's formatted text through the host snprintf, bounded by the
// buffer where retail's fixed stack buffer would overrun.
std::string format_one(const std::string &spec, int32_t value, char size, char conversion) {
	char buf[512];
	int n = 0;
	const bool is_signed = conversion == 'd' || conversion == 'i';
	if (size == 'h') {
		n = is_signed || conversion == 'c'
				? std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<int>(static_cast<int16_t>(value)))
				: std::snprintf(buf, sizeof(buf), spec.c_str(),
						  static_cast<unsigned>(static_cast<uint16_t>(value)));
	} else {
		n = is_signed || conversion == 'c'
				? std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<int>(value))
				: std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<unsigned>(value));
	}
	if (n < 0) return std::string();
	return std::string(buf, std::min<size_t>(static_cast<size_t>(n), sizeof(buf) - 1));
}

} // namespace

std::string hud_sprintf(const std::string &format, const std::vector<HudTextArg> &args) {
	std::string out;
	size_t next = 0;
	for (size_t i = 0; i < format.size(); ++i) {
		if (format[i] != '%') {
			out += format[i];
			continue;
		}
		if (i + 1 < format.size() && format[i + 1] == '%') {
			out += '%';
			++i;
			continue;
		}
		size_t j = i + 1;
		std::string flags;
		while (j < format.size() && (format[j] == '-' || format[j] == '+' || format[j] == ' ' ||
				format[j] == '#' || format[j] == '0'))
			flags += format[j++];
		std::string width;
		while (j < format.size() && is_digit(format[j])) width += format[j++];
		std::string precision;
		if (j < format.size() && format[j] == '.') {
			precision += format[j++];
			while (j < format.size() && is_digit(format[j])) precision += format[j++];
		}
		char size = 0;
		if (j < format.size() && (format[j] == 'h' || format[j] == 'l')) {
			size = format[j++];
		} else if (format.compare(j, 3, "I32") == 0) {
			size = 'I';
			j += 3;
		}
		if (j >= format.size()) { // an unterminated specification is plain text
			out += format.substr(i);
			break;
		}
		const char conversion = format[j];
		const bool integer = conversion == 'd' || conversion == 'i' || conversion == 'o' ||
				conversion == 'u' || conversion == 'x' || conversion == 'X' || conversion == 'c';
		const bool text_conversion = conversion == 's' && (size == 0 || size == 'h');
		const std::string literal = format.substr(i, j - i + 1);
		i = j;
		if ((!integer && !text_conversion) || next >= args.size()) {
			out += literal;
			continue;
		}
		const HudTextArg &arg = args[next++];
		// The alternate form means nothing to d/i/u/c/s, and a zero pad nothing
		// to c/s: the host printf leaves them undefined, so drop them there.
		std::string kept;
		for (const char f : flags) {
			if (f == '#' && conversion != 'o' && conversion != 'x' && conversion != 'X') continue;
			if (f == '0' && (conversion == 'c' || conversion == 's')) continue;
			kept += f;
		}
		const std::string spec = "%" + kept + width + precision + conversion;
		if (integer && arg.is_number) {
			out += format_one(spec, arg.number, size, conversion);
		} else if (text_conversion && !arg.is_number) {
			char buf[512];
			const int n = std::snprintf(buf, sizeof(buf), spec.c_str(), arg.text.c_str());
			if (n > 0) out.append(buf, std::min<size_t>(static_cast<size_t>(n), sizeof(buf) - 1));
		} else {
			out += literal;
		}
	}
	const size_t nul = out.find('\0');
	if (nul != std::string::npos) out.resize(nul);
	return out;
}

std::string hud_sprintf(const std::string &format) {
	return hud_sprintf(format, std::vector<HudTextArg>{});
}

std::string hud_sprintf(const std::string &format, int32_t value) {
	HudTextArg arg;
	arg.number = value;
	arg.is_number = true;
	return hud_sprintf(format, std::vector<HudTextArg>{arg});
}

std::string service_prompt_text(int prompt, const std::string &use_key, int32_t wait_seconds,
		const GameTextLookup &gametext) {
	HudTextArg key;
	key.text = use_key;
	switch (prompt) {
		case 1: // [orig: @0x5BDF2D GameText_GetString -> sprintf(buf, fmt, keyname) @0x5BDF45]
			return hud_sprintf(game_text(gametext, kGameTextOverlays, "STROVER_ARMORY_INFO", ""), {key});
		case 2: // [orig: @0x5BDFAC..0x5BDFBB GameText_GetStringWithFallback -> @0x5BDFC9]
			return hud_sprintf(game_text(gametext, kGameTextOverlays, "STROVER_VEHICLEBAY_INFO",
					"!Press '%s' to activate vehicle bay menu"), {key});
		case 3: // [orig: @0x5BE084 GameText_GetString -> sprintf(buf, fmt, wait) @0x5BE09C]
			return hud_sprintf(game_text(gametext, kGameTextOverlays, "STROVER_FARP_WAIT", ""), wait_seconds);
		case 4: // [orig: @0x5BE0D4 GameText_GetString -> sprintf(buf, fmt) @0x5BE0E9]
			return hud_sprintf(game_text(gametext, kGameTextOverlays, "STROVER_FARP_RELOADING", ""));
		default:
			return std::string();
	}
}

} // namespace opennova::hud
