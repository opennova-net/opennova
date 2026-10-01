// The NovaWorld lobby var-builders (engine/net/novaworld/lobby_vars): the HostSetup / Host /
// PlayerList lists in retail's insertion order with retail's value sources, the dirty delta, the
// ClientHostRequest shape, the NW-S5 identity set, and the host:port split. Server-free, Godot-free.
// [orig: CNapiGameSession_BuildHostVarLists @0x4d0b50; Lobby_UpdateServerInfo @0x4fe8c0;
//  Server_PlayerAdd @0x51d421; CNapiVarEntry_SetValue @0x630590; CNapiGameSession_SendHostRequest @0x4d3700]

#include <net/novaworld/lobby_vars.h>

#include <net/napi/session.h>
#include <net/napi/tlv.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nw = opennova;

static int g_fail = 0;
static bool expect(bool cond, const char *msg) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		++g_fail;
	}
	return cond;
}

static std::string value_of(const std::vector<nw::ClientVar> &vars, const char *name, int fnum = 0) {
	for (const nw::ClientVar &v : vars) {
		if (v.fnum == fnum && v.name == name) return v.value;
	}
	return "<absent>";
}

static bool names_are(const std::vector<nw::ClientVar> &vars, const std::vector<std::string> &names) {
	if (vars.size() != names.size()) return false;
	for (std::size_t i = 0; i < names.size(); ++i) {
		if (vars[i].name != names[i]) return false;
	}
	return true;
}

static std::string field_str(const nw::NapiMessage &m, const char *name) {
	for (const auto &f : m.fields) {
		if (f.name == name) return std::string(f.data.begin(), f.data.end());
	}
	return {};
}

int main() {
	nw::HostRegistration cfg;
	cfg.lobby_name = "jop_2_consumer";
	cfg.server_name = "Taylor's Game";
	cfg.server_message = "welcome";
	cfg.max_players = 16;
	cfg.password = true;
	cfg.listen_host = true;
	cfg.app_id = 4321;
	cfg.access_code_list = "A1";
	cfg.expansion = "jox01";
	cfg.lan_only = 0;
	cfg.host_key = "HK-1";
	cfg.game_type = "TDM";
	cfg.mission_name = "ASH_G11A";
	cfg.region_index = 1;
	cfg.player_count = 3;
	cfg.mi1 = 7; cfg.mi2 = 8; cfg.mi3 = 9;
	cfg.locked = false;
	cfg.skins = true;
	cfg.round_time_remaining_ticks = 3720 * 25;
	cfg.tracers = false;
	cfg.country = "US";
	cfg.allow_ping = true;
	cfg.uptime_ms = ((1 * 24 + 2) * 3600 + 3 * 60 + 4) * 1000u;
	cfg.time_of_day = 3;
	cfg.pcid_key = 0x01ABCDEFu;
	cfg.game_server_baffle_key = 77;
	cfg.bb_mode = 1;
	cfg.gcc = "gcc";
	cfg.version = "1.7.5.7";
	cfg.pb_server = true;

	nw::HostLobbyText text;
	text.yes = "Yes";
	text.no = "No";
	text.no_time_limit = "None";
	text.region = {"North America", "Europe", "Asia"};

	// 1. HostSetup: the eleven BuildHostVarLists vars in insertion order, then the
	//    ReconnectCounter InitHeapsAndSerializeCounter appends (0 on a fresh host).
	{
		const auto setup = nw::make_host_setup_var_list(cfg);
		expect(names_are(setup, {"LobbyName", "ServerName", "Msg", "MaxPlayers", "Password", "Dedicated",
		                         "AppId", "AccessCodeList", "PLoad", "Exp", "LAN", "ReconnectCounter"}),
		       "HostSetup carries the eleven retail vars and ReconnectCounter, in order");
		expect(value_of(setup, "ReconnectCounter") == "0", "a fresh host's ReconnectCounter is 0");
		nw::HostRegistration rehost = cfg;
		rehost.reconnect_counter = 2;
		expect(value_of(nw::make_host_setup_var_list(rehost), "ReconnectCounter") == "2",
		       "ReconnectCounter prints the counter with %ld");
		expect(value_of(setup, "Password") == "1", "HostSetup Password is 1/0");
		expect(value_of(setup, "Dedicated") == "0", "HostSetup Dedicated is 0 for a listen host");
		expect(value_of(setup, "AppId") == "4321", "HostSetup AppId is the session random");
		expect(value_of(setup, "PLoad").empty(), "HostSetup PLoad is empty");
		expect(value_of(setup, "Exp") == "jox01", "HostSetup Exp is the expansion");
		expect(value_of(setup, "LAN") == "0", "HostSetup LAN is the lan-only flag");
		expect(value_of(setup, "ServerPortNumber") == "<absent>", "no ServerPortNumber var exists in retail");
	}

	// 2. The initial Host list (the ClientHostRequest's): BuildHostVarLists' eight.
	{
		const auto host = nw::make_host_var_list(cfg, text, /*full=*/false);
		expect(names_are(host, {"LobbyName", "ServerName", "Msg", "MaxPlayers", "AppId", "PLoad", "Exp", "LAN"}),
		       "initial Host list is the eight BuildHostVarLists vars");
	}

	// 3. The full Host list: Lobby_UpdateServerInfo's columns appended in its order, existing
	//    names updated in place, AppID folding onto AppId.
	{
		const auto host = nw::make_host_var_list(cfg, text, /*full=*/true);
		expect(names_are(host, {"LobbyName", "ServerName", "Msg", "MaxPlayers", "AppId", "PLoad", "Exp", "LAN",
		                        "HostKey", "GameType", "MissionName", "Region", "Players", "MI1", "MI2", "MI3",
		                        "Dedicated", "Locked", "Skins", "TimeLeft", "Password", "Tracers", "Mod",
		                        "Country", "Port", "AllowPing", "Age", "TimeOfDay", "PCIDKey",
		                        "GameServerBaffleKey", "Stat", "LevelRange", "BBMode", "GCC", "GV", "Version",
		                        "Ver1", "Ver2", "PBServer"}),
		       "full Host list = the eight + Lobby_UpdateServerInfo's appended columns");
		expect(value_of(host, "HostKey") == "HK-1", "HostKey passes through");
		expect(value_of(host, "GameType") == "TDM", "GameType is the abbreviation");
		expect(value_of(host, "Region") == "Europe", "Region index 1 -> STRNOVA08");
		expect(value_of(host, "Players") == "3", "Players is the live count");
		expect(value_of(host, "MI2") == "8", "MI2 passes through");
		expect(value_of(host, "Dedicated") == "No", "listen host -> Dedicated No");
		expect(value_of(host, "Locked") == "No", "Locked No");
		expect(value_of(host, "Skins") == "Yes", "Skins Yes");
		expect(value_of(host, "TimeLeft") == "25", "TimeLeft is ticks / 3720 minutes");
		expect(value_of(host, "Password") == "Yes", "Host Password is the gametext token");
		expect(value_of(host, "Tracers") == "No", "tracers off -> No");
		expect(value_of(host, "Mod") == "jox01", "Mod is the expansion");
		expect(value_of(host, "Country") == "US", "Country passes through");
		expect(value_of(host, "Port") == "-1", "Port is the literal -1");
		expect(value_of(host, "AllowPing") == "y", "AllowPing is y/n");
		expect(value_of(host, "Age") == "1 02:03:04", "Age is the %ld %2.2ld:%2.2ld:%2.2ld uptime");
		expect(value_of(host, "TimeOfDay") == "Dusk", "TimeOfDay 3 -> Dusk");
		expect(value_of(host, "AppId") == "4321", "AppID folds onto AppId (case-insensitive lookup)");
		expect(value_of(host, "PCIDKey") == std::to_string(0x01ABCDEFu), "PCIDKey is the ring key");
		expect(value_of(host, "GameServerBaffleKey") == "77", "GameServerBaffleKey passes through");
		expect(value_of(host, "Stat") == "N", "Stat N");
		expect(value_of(host, "LevelRange") == " ", "LevelRange is the folded space");
		expect(value_of(host, "BBMode") == "1", "BBMode passes through");
		expect(value_of(host, "GV") == "1.7.5.7" && value_of(host, "Version") == "1.7.5.7",
		       "GV and Version are the game version");
		expect(value_of(host, "Ver1") == "3" && value_of(host, "Ver2") == "2345", "Ver1/Ver2 literals");
		expect(value_of(host, "PBServer") == "1", "PBServer 1/0");
		expect(value_of(host, "ServerIP") == "<absent>", "no ServerIP var exists in retail");

		// The alternate value arms.
		nw::HostRegistration alt = cfg;
		alt.listen_host = false;
		alt.locked = true;
		alt.round_time_remaining_ticks = -1;
		alt.expansion.clear();
		alt.country = "XX";
		alt.allow_ping = false;
		alt.time_of_day = 9;
		alt.dedicated_server = true;
		alt.country_name = "United States";
		alt.language = "English";
		alt.tz_bias = 480;
		const auto host2 = nw::make_host_var_list(alt, text, /*full=*/true);
		expect(value_of(host2, "Dedicated") == "Yes", "dedicated -> Yes");
		expect(value_of(host2, "Locked") == "Yes", "locked -> Yes");
		expect(value_of(host2, "Country") == "XX", "locked -> Country XX");
		expect(value_of(host2, "TimeLeft") == "None", "no time limit -> STRNOVA10");
		expect(value_of(host2, "Mod") == " ", "no expansion -> Mod is a space");
		expect(value_of(host2, "AllowPing") == "n", "ping off -> n");
		expect(value_of(host2, "TimeOfDay") == "Unknown", "out-of-range time of day -> Unknown");
		expect(value_of(host2, "CountryName") == "United States" && value_of(host2, "Lang") == "English" &&
		               value_of(host2, "TZB") == "480",
		       "dedicated server appends CountryName/Lang/TZB before Ver1");
		std::size_t ver1 = 0, tzb = 0;
		for (std::size_t i = 0; i < host2.size(); ++i) {
			if (host2[i].name == "Ver1") ver1 = i;
			if (host2[i].name == "TZB") tzb = i;
		}
		expect(tzb + 1 == ver1, "TZB sits right before Ver1");
		alt.locked = false;
		alt.country = "";
		expect(value_of(nw::make_host_var_list(alt, text, true), "Country") == " ",
		       "empty country -> a space");
		alt.region_index = 5;
		expect(value_of(nw::make_host_var_list(alt, text, true), "Region") == "?",
		       "unknown region index -> ?");

		// A stock registration advertises AllowPing 'y': game.cfg `ping`
		// defaults to 1 [orig: Config_SetDefaults @0x54D324 -> g_NWAllowPing
		// @0x551D4F; read @0x4FEF72].
		expect(value_of(nw::make_host_var_list(nw::HostRegistration{}, text, true), "AllowPing") == "y",
		       "a default registration advertises AllowPing y");
	}

	// 4. PlayerList: five VarFNum-keyed vars per slot.
	{
		std::vector<nw::HostPlayerSlot> players(2);
		players[0].slot = 0; players[0].player_name = "Host"; players[0].ip_and_port = "10.0.0.1:32768";
		players[0].pcid = "P0"; players[0].team = "1"; players[0].type = "0";
		players[1].slot = 3; players[1].player_name = "Joiner"; players[1].ip_and_port = "10.0.0.2:32768";
		players[1].pcid = "P3"; players[1].team = "2"; players[1].type = "0";
		const auto list = nw::make_player_list(players);
		expect(list.size() == 10, "two slots -> ten vars");
		expect(value_of(list, "PlayerName", 0) == "Host" && value_of(list, "PlayerName", 3) == "Joiner",
		       "PlayerName is keyed by the slot");
		expect(value_of(list, "PlayerIpAndPort", 3) == "10.0.0.2:32768", "PlayerIpAndPort per slot");
		expect(value_of(list, "PlayerTeam", 3) == "2" && value_of(list, "PlayerType", 3) == "0",
		       "PlayerTeam/PlayerType per slot");
		expect(list[0].name == "PlayerName" && list[1].name == "PlayerIpAndPort" && list[2].name == "PlayerPCID" &&
		               list[3].name == "PlayerTeam" && list[4].name == "PlayerType",
		       "the five per-slot vars keep Server_PlayerAdd's order");
	}

	// 4b. The POST status blob: the full Host list in order (LobbyName/HostKey repeat after the
	//     preamble), every PlayerName, and the sendplayerlist gate; round-trips the codec.
	{
		std::vector<nw::HostPlayerSlot> players(2);
		players[0].slot = 0; players[0].player_name = "Host";
		players[1].slot = 3; players[1].player_name = "Joiner Two";
		const nw::LobbyStatusBlob blob = nw::make_host_status_blob(cfg, text, players);
		expect(blob.lobby_name == "jop_2_consumer" && blob.host_key == "HK-1", "blob preamble");
		const auto host = nw::make_host_var_list(cfg, text, true);
		expect(blob.host_vars.size() == host.size() && blob.host_vars[0].first == "LobbyName" &&
		               blob.host_vars[8].first == "HostKey",
		       "blob carries the full Host list in order (LobbyName/HostKey repeat)");
		expect(blob.player_names.size() == 2 && blob.player_names[1] == "Joiner Two",
		       "blob carries every PlayerName");
		expect(blob.send_player_names, "sendplayerlist defaults on");
		const std::string text_form = nw::lobby_update_build(blob);
		nw::LobbyStatusBlob parsed;
		expect(nw::lobby_update_parse(text_form, parsed), "the built blob parses");
		expect(parsed.host_key == "HK-1" && nw::lobby_status_value(parsed, "MissionName") == "ASH_G11A",
		       "the parsed blob keeps the Host columns");
		expect(parsed.player_names.size() == 2 && parsed.player_names[1] == "Joiner+Two",
		       "player names arrive lobby-sanitized");
		nw::HostRegistration quiet = cfg;
		quiet.send_player_list = false;
		expect(!nw::make_host_status_blob(quiet, text, players).send_player_names,
		       "sendplayerlist off drops the p= suffix");
	}

	// 5. The dirty delta: changed values and new entries only.
	{
		const auto before = nw::make_host_var_list(cfg, text, true);
		nw::HostRegistration next = cfg;
		next.player_count = 4;
		next.mission_name = "ASH_G12A";
		const auto after = nw::make_host_var_list(next, text, true);
		const auto dirty = nw::dirty_client_vars(before, after);
		expect(names_are(dirty, {"MissionName", "Players"}), "only the changed columns are dirty");
		expect(nw::dirty_client_vars(after, after).empty(), "an unchanged list has no dirty vars");
		expect(nw::dirty_client_vars({}, after).size() == after.size(), "everything is dirty on the first send");
	}

	// 6. ClientHostRequest: CurrentlyHosting 0 on a fresh host, the Cookie, HostSetup, the
	//    initial Host list, and an empty PlayerList.
	{
		const std::vector<nw::ClientVar> cookie = {{0, "CountryName", "United States"}, {0, "NWUID", "u-1"}};
		const nw::NapiMessage req = nw::make_host_request(cfg, cookie, /*currently_hosting=*/0);
		expect(req.name == "ClientHostRequest", "ClientHostRequest name");
		expect(field_str(req, "CurrentlyHosting") == "0", "a fresh host sends CurrentlyHosting 0");
		expect(field_str(req, "VarCheck") == "1", "VarCheck 1");
		expect(req.children.size() == 4, "four var-lists");
		expect(field_str(req.children[0], "VarList") == "Cookie" && req.children[0].children.size() == 2,
		       "Cookie carries the cookie jar");
		expect(field_str(req.children[1], "VarList") == "HostSetup" && req.children[1].children.size() == 12,
		       "HostSetup carries twelve vars (ReconnectCounter last)");
		expect(field_str(req.children[2], "VarList") == "Host" && req.children[2].children.size() == 8,
		       "the request's Host list is the initial eight");
		expect(field_str(req.children[3], "VarList") == "PlayerList" && req.children[3].children.empty(),
		       "the request's PlayerList is empty");
	}

	// 7. The NW-S5 identity set: names + order + the three empties + the fingerprint seed math.
	{
		nw::LobbyIdentityParams p;
		p.client_index = 0x11112222u;
		p.client_key = 0x33334444u;
		p.tz_bias = "-480";
		p.country = "Canada";
		p.language = "French";
		p.my_installed_exp_bits = "5";
		p.nwpssk = "ENVIRONMENTNWPSSKTOKEN";
		p.nwusid = "ENVIRONMENTUSID";
		p.nwhwi = "Fixture GPU$256$8192$1280x720$1920x1080";
		const auto vars = nw::make_lobby_identity_vars(p);
		expect(vars.size() == 10, "identity set has 10 vars");
		const char *names[] = {"CountryName", "Language",     "TimeZoneBias", "MyInstalledExpBits",
		                       "NWUID",       "NWCDKIID",     "NWCDKIIDEXP1", "NWPSSK",
		                       "NWUSID",      "NWHWI"};
		bool order_ok = vars.size() == 10;
		for (std::size_t i = 0; i < vars.size() && order_ok; ++i) {
			if (vars[i].first != names[i]) order_ok = false;
		}
		expect(order_ok, "identity set names + order match the NW-S5 list");
		expect(vars[0].second == p.country, "CountryName passes through");
		expect(vars[1].second == p.language, "Language passes through");
		expect(vars[2].second == "-480", "TimeZoneBias passes through");
		expect(vars[3].second == p.my_installed_exp_bits, "MyInstalledExpBits passes through");
		expect(vars[4].second.empty() && vars[5].second.empty() && vars[6].second.empty(),
		       "NWUID / NWCDKIID / NWCDKIIDEXP1 are empty");
		expect(vars[7].second == p.nwpssk, "NWPSSK passes through from the client environment");
		expect(vars[8].second == p.nwusid, "NWUSID passes through from the client environment");
		expect(vars[9].second == p.nwhwi, "NWHWI passes through");

		// Non-binding callers retain the deterministic fallback when stable machine tokens are absent.
		p.nwpssk.clear();
		p.nwusid.clear();
		const auto fallback = nw::make_lobby_identity_vars(p);
		expect(fallback[7].second == nw::az_fingerprint(p.client_index ^ 0x5053534Bu, 23),
		       "NWPSSK fallback remains deterministic");
		expect(fallback[8].second == nw::az_fingerprint(p.client_key ^ 0x55534944u, 16),
		       "NWUSID fallback remains deterministic");
		// az_fingerprint is deterministic and length-exact.
		expect(nw::az_fingerprint(123, 23).size() == 23, "az_fingerprint length 23");
		expect(nw::az_fingerprint(123, 16) == nw::az_fingerprint(123, 16), "az_fingerprint deterministic");
	}

	// 8. Retail's stable volume/MAC identity transforms, pinned independently
	// from the binding that gathers the platform inputs.
	{
		nw::RetailMachineInputs in;
		in.volume_serial = 0x12345678u;
		in.maximum_component_length = 255;
		in.filesystem_flags = 0xA5A5A5A5u;
		in.volume_name = "SYSTEM";
		in.filesystem_name = "NTFS";
		in.ethernet_address = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
		in.has_ethernet_address = true;
		const nw::LobbyMachineTokens tokens = nw::make_retail_machine_tokens(in);
		expect(tokens.nwpssk == "ELHCDHDCSNDFDJMOGMJROAE",
		       "NWPSSK matches CDKey_GenerateHardwareFingerprint transform");
		expect(tokens.nwusid == "NEBBDDFFHHJJLLQR",
		       "NWUSID matches generate_hardware_fingerprint transform");
	}

	// 9. parse_host_port — good / no-colon / empty-port / non-numeric-port.
	{
		std::string host;
		uint16_t port = 0;
		expect(nw::parse_host_port("1.2.3.4:7597", host, port) && host == "1.2.3.4" && port == 7597,
		       "parse_host_port good case");
		host.clear();
		port = 123;
		expect(!nw::parse_host_port("nohostport", host, port), "parse_host_port rejects no colon");
		expect(!nw::parse_host_port("1.2.3.4:", host, port), "parse_host_port rejects empty port");
		expect(!nw::parse_host_port("1.2.3.4:abc", host, port), "parse_host_port rejects non-numeric port");
	}

	if (g_fail == 0) {
		std::printf("lobby_vars_test: all checks passed\n");
	}
	return g_fail == 0 ? 0 : 1;
}
