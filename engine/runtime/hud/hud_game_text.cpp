// The HUD's game-text compositions — see hud_game_text.h for the witnesses.
// [orig: HUD_GetWaypointName @0x594630; HUD_DisplayTriggeredText @0x51f190]

#include <runtime/hud/hud_game_text.h>

#include <runtime/hud/hud_frame.h> // HudSessionText

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdio>

namespace opennova::hud {

std::string waypoint_display_name(const WaypointNameKey &key, bool in_session,
		uint32_t game_type, const GameTextLookup &mission, const GameTextLookup &gametext) {
	int32_t id = key.name_id; // entity+672 [orig: @0x594650]
	char buf[32];
	if (in_session) { // [orig: `g_NapiNPCtx.is_in_session` @0x594668]
		if ((game_type & 0x20000u) == 0u) ++id; // [orig: @0x594678..0x59467A]
		if (key.has_def) { // entity+32 [orig: @0x59467D..0x594682]
			buf[0] = 0;
			if ((key.def_attrib & 0x80000u) != 0u) {
				std::snprintf(buf, sizeof(buf), "STRWPNAMEARMORY"); // @0x594698
			} else if ((key.def_attrib & 0x8000u) != 0u) {
				std::snprintf(buf, sizeof(buf), "STRWPNAMETARGET"); // @0x5946AC
			} else {
				const int32_t t = key.def_type; // def+80 @0x5946AE
				if (t == 4091 || t == 4093 || t == 4095 || t == 4096 || t == 4097)
					std::snprintf(buf, sizeof(buf), "STRWPNAMEFLAG"); // @0x59470D
				else if (t == 4098 || t == 4100 || t == 4101 || t == 4102 || t == 4103)
					std::snprintf(buf, sizeof(buf), "STRWPNAMEFLAGBAY"); // @0x594701
			}
			// An empty key misses like any other [orig: GameText_GetString
			// @0x59471F; the empty test @0x59472D].
			std::string special = buf[0] != 0 ? game_text(gametext, "WPNames", buf, "") : std::string();
			if (!special.empty()) return special;
		}
	}
	std::snprintf(buf, sizeof(buf), "STRWPNAME%03d", id); // [orig: @0x59473D]
	const std::string name = game_text(mission, "WPNames", buf, "");
	// Empty or the literal "null" falls back to the gametext default
	// [orig: @0x59476F..0x59477B].
	if (name.empty() || strutil::iequals(name.c_str(), "null"))
		return game_text(gametext, "WPNames", "STRWPNAMEDEFAULT", "");
	return name;
}

std::string waypoint_label_text(const std::string &name, const WaypointNameKey &key,
		uint32_t game_type, const GameTextLookup &gametext) {
	// "m to" is gametext hud/mto [orig: GameText_GetString(off_7C4B24 "hud",
	// off_7D8F74 "mto") @0x59492B / @0x59490B / @0x59494B / @0x5949AD].
	const std::string mto = game_text(gametext, "hud", "mto", "");
	std::string label;
	const char *format = "%s %s"; // [orig: @0x594956]
	if (key.has_def && game_type == 0x10004u) { // [orig: @0x5948C2..0x5948D3]
		if (key.def_type == 4091 || key.def_type == 4098)
			format = "%s <c4050FF>%s<co>"; // [orig: @0x594936]
		else if (key.def_type == 4093 || key.def_type == 4100)
			format = "%s <cFF3535>%s<co>"; // [orig: @0x594916]
	}
	HudTextArg a;
	a.text = mto;
	HudTextArg b;
	b.text = name;
	label = hud_sprintf(format, {a, b});
	// The LFP override replaces the whole label [orig: @0x59495B..0x5949C0 —
	// def+84 & 0x40000 and entity+538 nonzero; "%s %s %c-%d" with
	// Overlays/LFP (off_7D8F48), (b & 0x1F) + 64, b >> 5].
	if (key.has_def && (key.def_attrib & 0x40000u) != 0u && key.zone_number != 0) {
		char buf[256];
		std::snprintf(buf, sizeof(buf), "%s %s %c-%d", mto.c_str(),
				game_text(gametext, "Overlays", "LFP", "").c_str(),
				static_cast<char>((key.zone_number & 0x1F) + 64), key.zone_number >> 5);
		label = buf;
	}
	return label;
}

void hud_session_text(const GameTextLookup &gametext, HudSessionText &out) {
	out.timer = game_text(gametext, "Overlays", "STROVER50", "");            // @0x593E00
	out.players_remaining = game_text(gametext, "Client", "STRCLI25", "");  // @0x593ED9
	out.players = game_text(gametext, "Client", "STRCLI04", "");            // @0x593F04
	out.spectators = game_text(gametext, "Client", "STRCLI23", "");         // @0x593F28
	out.in_the_zone = game_text(gametext, "Overlays", "STROVER53", "");     // @0x59CE2D
	static const char *const kTeamKeys[6] = {"strcli19", "strcli05", "strcli06", "strcli17",
			"strcli18", "strcli01"}; // [orig: @0x59AAED..0x59ABDF]
	for (size_t i = 0; i < out.team_names.size(); ++i)
		out.team_names[i] = game_text(gametext, "client", kTeamKeys[i], "");
	out.attacking = game_text(gametext, "client", "strcli20", ""); // @0x59AC86
	out.defending = game_text(gametext, "client", "strcli21", ""); // @0x59AC96
}

std::string subgoal_message(bool lost, int header_id, const GameTextLookup &mission) {
	char key[32];
	std::snprintf(key, sizeof(key), lost ? "STRLOSEMSG%03d" : "STRWINMSG%03d", header_id);
	return game_text(mission, lost ? "LoseConditions" : "WinConditions", key, "");
}

std::string objective_header(const GameTextLookup &gametext) {
	return game_text(gametext, "Misc", "STRMISC_NEWOBJECTIVE", "");
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
	return game_text(gametext, "WepDes", weapon_id.c_str(), "");
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
			return hud_sprintf(game_text(gametext, "Overlays", "STROVER_ARMORY_INFO", ""), {key});
		case 2: // [orig: @0x5BDFAC..0x5BDFBB GameText_GetStringWithFallback -> @0x5BDFC9]
			return hud_sprintf(game_text(gametext, "Overlays", "STROVER_VEHICLEBAY_INFO",
					"!Press '%s' to activate vehicle bay menu"), {key});
		case 3: // [orig: @0x5BE084 GameText_GetString -> sprintf(buf, fmt, wait) @0x5BE09C]
			return hud_sprintf(game_text(gametext, "Overlays", "STROVER_FARP_WAIT", ""), wait_seconds);
		case 4: // [orig: @0x5BE0D4 GameText_GetString -> sprintf(buf, fmt) @0x5BE0E9]
			return hud_sprintf(game_text(gametext, "Overlays", "STROVER_FARP_RELOADING", ""));
		default:
			return std::string();
	}
}

} // namespace opennova::hud
