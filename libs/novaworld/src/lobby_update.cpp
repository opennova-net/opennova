#include <novaworld/lobby_update.h>

#include <cstdio>
#include <cstring>

namespace opennova {

namespace {

// Append "<lead>key = value" to `out`. Matches the binary's
// sprintf(&v81[strlen(v81)], " %s = %s", key, value) pattern inside the
// delete-path blob builder, which is reused for update-path packets too.
void append_kv(std::string &out, const char *key, const std::string &value) {
	out += ' ';
	out += key;
	out += " = ";
	out += value;
}

} // namespace

std::string lobby_update_build(const LobbyServerInfo &info, bool is_delete) {
	// Blob starts with "<lobbyName> " and immediately "HostKey = <key>"
	// (the only key NOT prefixed with a space in the original, since it's
	// the first pair after the LobbyName prefix).
	std::string out;
	out.reserve(512);
	out += info.lobby_name;
	out += ' ';

	// HostKey is always in position 2 (witnessed in the decomp pre-loop).
	out += "HostKey = ";
	out += info.host_key;

	if (is_delete) {
		// Delete path: short form. Witnessed:
		//   sprintf(&v81[strlen(v81)], " Port = -1 DELETE");
		out += " Port = -1 DELETE";
		return out;
	}

	// Update path: the full set of keys.
	append_kv(out, "ServerName", info.server_name);
	append_kv(out, "GameType", info.game_type);
	append_kv(out, "MissionName", info.mission_name);
	append_kv(out, "Region", info.region);
	append_kv(out, "HostDID", info.host_did);

	auto itoa = [](int v) { char buf[32]; std::snprintf(buf, sizeof(buf), "%d", v); return std::string(buf); };
	auto yes_no = [](bool b) -> std::string { return b ? "Yes" : "No"; };

	append_kv(out, "Players", itoa(info.players));
	append_kv(out, "MaxPlayers", itoa(info.max_players));
	append_kv(out, "MI1", itoa(info.mi1));
	append_kv(out, "MI2", itoa(info.mi2));
	append_kv(out, "MI3", itoa(info.mi3));
	append_kv(out, "Dedicated", yes_no(info.dedicated));
	append_kv(out, "Locked", yes_no(info.locked));
	append_kv(out, "Skins", yes_no(info.skins_allowed));
	append_kv(out, "TimeLeft", info.time_left);
	append_kv(out, "Password", yes_no(info.password_protected));
	append_kv(out, "Tracers", yes_no(!info.tracers_disabled));
	append_kv(out, "Country", info.country);
	append_kv(out, "Port", info.port);
	append_kv(out, "AllowPing", itoa(info.allow_ping));
	append_kv(out, "Uptime", info.uptime); // binary uses a string key here; name witnessed as off_748680

	append_kv(out, "TimeOfDay", info.tod);
	append_kv(out, "AccessCodeList", info.access_code_list);
	append_kv(out, "AppID", itoa(info.app_id));
	append_kv(out, "PCIDKey", itoa(info.pcid_key));
	append_kv(out, "GameServerBaffleKey", itoa(info.game_server_baffle_key));
	append_kv(out, "Stat", info.stat);
	if (!info.level_range.empty()) {
		append_kv(out, "LevelRange", info.level_range);
	}
	append_kv(out, "BBMode", itoa(info.bb_mode));
	append_kv(out, "GV", info.gv);
	append_kv(out, "Version", info.version);
	append_kv(out, "CountryName", info.country_name);
	append_kv(out, "Lang", info.lang);
	append_kv(out, "TimezoneBias", itoa(info.timezone_bias));
	append_kv(out, "Ver1", info.ver1);
	append_kv(out, "Ver2", info.ver2);
	append_kv(out, "PBServer", info.pb_server ? "1" : "0");

	for (const auto &p : info.extra_pairs) {
		append_kv(out, p.first.c_str(), p.second);
	}

	return out;
}

} // namespace opennova
