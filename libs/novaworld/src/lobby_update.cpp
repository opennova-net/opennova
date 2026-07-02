#include <novaworld/lobby_update.h>

#include <cstdio>
#include <string_view>

namespace opennova {

namespace {

std::string sanitize_lobby_value(std::string_view value) {
	if (value.empty()) {
		return "---";
	}
	std::string out(value);
	for (char &ch : out) {
		if (ch == ' ' || ch == '?' || ch == '@' || ch == '=') {
			ch = '+';
		}
	}
	return out;
}

std::string itoa(int v) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%d", v);
	return std::string(buf);
}

std::string yn(bool v) {
	return v ? "Y" : "N";
}

void append_kv(std::string &out, std::string_view key, std::string_view value) {
	out += ' ';
	out += sanitize_lobby_value(key);
	out += " = ";
	out += sanitize_lobby_value(value);
}

void append_kv_raw_key(std::string &out, std::string_view key, std::string_view value) {
	out += ' ';
	out += key;
	out += " = ";
	out += sanitize_lobby_value(value);
}

void append_kv_literal(std::string &out, std::string_view key, std::string_view value) {
	out += ' ';
	out += key;
	out += " = ";
	out += value;
}

} // namespace

std::string lobby_update_build(const LobbyServerInfo &info) {
	std::string out;
	out.reserve(768);

	out += info.lobby_name;
	out += "  HostKey = ";
	out += sanitize_lobby_value(info.host_key);

	append_kv_raw_key(out, "ServerName", info.server_name);
	append_kv_raw_key(out, "GameType", info.game_type);
	append_kv_raw_key(out, "MissionName", info.mission_name);
	append_kv_raw_key(out, "Region", info.region);
	append_kv_raw_key(out, "Players", itoa(info.players));
	append_kv_raw_key(out, "MaxPlayers", itoa(info.max_players));
	append_kv_raw_key(out, "Dedicated", yn(info.dedicated));
	append_kv_raw_key(out, "TimeLeft", info.time_left);
	append_kv_raw_key(out, "Password", yn(info.password_protected));
	append_kv_raw_key(out, "Country", info.country);
	append_kv_raw_key(out, "Msg", info.msg);
	append_kv_raw_key(out, "Age", info.uptime);
	append_kv_raw_key(out, "TimeOfDay", info.tod);
	append_kv_raw_key(out, "Stat", "N");
	append_kv_literal(out, "LevelRange", " ");
	append_kv_raw_key(out, "Locked", yn(info.locked));
	append_kv_raw_key(out, "Tracers", yn(!info.tracers_disabled));
	append_kv_raw_key(out, "Skins", yn(info.skins_allowed));
	append_kv_raw_key(out, "BBMode", itoa(info.bb_mode));
	append_kv_raw_key(out, "Mod", info.mod);
	append_kv_raw_key(out, "PIX", info.pix);
	append_kv_raw_key(out, "PBServer", info.pb_server ? "1" : "0");
	append_kv_raw_key(out, "Ver1", info.ver1);
	append_kv_raw_key(out, "Ver2", info.ver2);
	append_kv_raw_key(out, "Exp", info.exp);
	append_kv_raw_key(out, "Expbits", info.expbits);
	append_kv_raw_key(out, "Joicon2", info.joicon2);
	append_kv_raw_key(out, "GCC", info.gcc);
	append_kv_raw_key(out, "Port", info.port);
	append_kv_raw_key(out, "AllowPing", itoa(info.allow_ping));

	if (!info.country_name.empty() || !info.lang.empty() || info.timezone_bias != 0) {
		append_kv_raw_key(out, "CountryName", info.country_name);
		append_kv_raw_key(out, "Lang", info.lang);
		append_kv_raw_key(out, "TZB", itoa(info.timezone_bias));
	}

	for (const auto &p : info.extra_pairs) {
		append_kv(out, p.first, p.second);
	}

	if (info.player_names.empty()) {
		out += " p=";
	} else {
		for (const std::string &player : info.player_names) {
			out += " p=";
			out += sanitize_lobby_value(player);
		}
	}

	return out;
}

} // namespace opennova
