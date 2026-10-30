// The NovaWorld lobby var-builders (engine/net/novaworld/lobby_vars): the HostSetup / Host /
// PlayerList lists in retail's insertion order with retail's value sources, the
// ClientHostRequest shape, the NW-S5 identity set, and the host:port split. Server-free,
// Godot-free.
// [orig: CNapiGameSession_BuildHostVarLists @0x4d0b50; Lobby_UpdateServerInfo @0x4fe8c0;
//  Server_PlayerAdd @0x51d421; CNapiGameSession_SendHostRequest @0x4d3700]

#include <net/novaworld/lobby_identity.h> // fallback_machine_inputs / fallback_machine_tokens
#include <net/novaworld/lobby_vars.h>

#include <net/napi/session.h>
#include <net/napi/tlv.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nw = opennova;

// The executables' NWPSSK serial terms (games/jo/jo_protocol.cpp's and
// games/dfx2/dfx2_protocol.cpp's Protocol::nwpssk_mix). [orig: CDKey_GenerateHardwareFingerprint
// @0x4a4a00 (@0x4a4b1f)] [orig: CDKey_GenerateHardwareFingerprint @0x4a4a60 (@0x4a4b89) (dfx2.exe)]
static const nw::FingerprintSerialMix kJoMix{ 0x0B24B390u, /*shift_first=*/false };
static const nw::FingerprintSerialMix kDfx2Mix{ 0x32944845u, /*shift_first=*/true };

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
	// JO's version pair, the title's (Protocol::ver1 / ver2; NwuHostRole sets it).
	cfg.ver1 = "3";
	cfg.ver2 = "2345";

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

	// The LAN-only host's admitted joiner (D-GAMECFG-8): LANIP as inet_ntoa prints the reported
	// address (PeerAddr's packing), LANPORT the reported port "%ld", a signed print.
	// [orig: NapiNPServerMsg_0x002 @0x51315E / @0x51317A / @0x51318C; the "%ld" @0x631981]
	{
		const auto lan = nw::make_host_lan_vars(0x0A00A8C0u, 2302u);
		expect(names_are(lan, {"LANIP", "LANPORT"}) && lan[0].fnum == 0 && lan[1].fnum == 0,
		       "the LAN vars are LANIP then LANPORT, no slot");
		expect(value_of(lan, "LANIP") == "192.168.0.10", "LANIP is the reported address's dotted quad");
		expect(value_of(lan, "LANPORT") == "2302", "LANPORT is the reported port");
		expect(value_of(nw::make_host_lan_vars(0u, 0xFFFFFFFFu), "LANPORT") == "-1",
		       "LANPORT prints the dword with %ld");
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
		expect(value_of(host, "Ver1") == "3" && value_of(host, "Ver2") == "2345", "Ver1/Ver2: the title's version pair");
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
		alt.met_ext = true;
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
		       "the gate's METEXT appends CountryName/Lang/TZB before Ver1");
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
		// A registration's ServerName is the session name the host dialog left, nothing made
		// up: the dialog copies GAME_NAME as it stands and the host lists register it as it is,
		// so a cleared name advertises "" (D-MNU-77).
		// [orig: HostDialog_ReadSettings @0x555940 - GAME_NAME into serverName_3A5, cap 0x20,
		//  @0x555946..0x555967; CNapiGameSession_BuildHostVarLists @0x4D0BDB / @0x4D0D1B]
		expect(nw::HostRegistration{}.server_name.empty(), "a default registration names none");
		const auto unnamed = nw::make_host_var_list(nw::HostRegistration{}, text, true);
		expect(value_of(unnamed, "ServerName").empty(),
		       "an unnamed registration advertises an empty ServerName");
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
	//     preamble), every PlayerName, and the sendplayerlist gate; its text form.
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
		expect(text_form.rfind("jop_2_consumer  HostKey = HK-1 ", 0) == 0 &&
		               text_form.find(" MissionName = ASH_G11A") != std::string::npos,
		       "the text form keeps the Host columns");
		const std::string names = " p=Host p=Joiner+Two";
		expect(text_form.size() > names.size() &&
		               text_form.compare(text_form.size() - names.size(), names.size(), names) == 0,
		       "player names go lobby-sanitized");
		nw::HostRegistration quiet = cfg;
		quiet.send_player_list = false;
		expect(!nw::make_host_status_blob(quiet, text, players).send_player_names,
		       "sendplayerlist off drops the p= suffix");
	}

	// 5. ClientHostRequest: CurrentlyHosting 0 on a fresh host, the Cookie, HostSetup, the
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

	// 6. The NW-S5 identity set: names + order + the three empties + the embedder's tokens.
	{
		nw::LobbyIdentityParams p;
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
		// dfx2.exe sets its record's two ids there. [orig: CNapiGameSession_OnNovaWorldConnected
		//  @0x4d1760 - @0x4d18d3..0x4d18f2 (dfx2.exe)]
		nw::LobbyIdentityParams keyed = p;
		keyed.nwcdkiid = "BMCCDKDKDLFFNGFDJHMFMMALJCLEFPADLPHHDHEEDHOGCIDLKC";
		const auto keyed_vars = nw::make_lobby_identity_vars(keyed);
		expect(keyed_vars[5].first == "NWCDKIID" && keyed_vars[5].second == keyed.nwcdkiid &&
						keyed_vars[6].first == "NWCDKIIDEXP1" && keyed_vars[6].second.empty(),
				"NWCDKIID carries the client's CDKIID, NWCDKIIDEXP1 its (empty) CDKIIDEXP1");
		expect(vars[7].second == p.nwpssk, "NWPSSK passes through from the client environment");
		expect(vars[8].second == p.nwusid, "NWUSID passes through from the client environment");
		expect(vars[9].second == p.nwhwi, "NWHWI passes through");
	}

	// 6b. D-NET-981: the off-Windows fallback runs retail's two encoders over the words the
	// stable id's hash supplies by position, so its tokens are the Windows encoder's over the same
	// words: NWPSSK's 23 letters banded A..P / C..R / E..T in turn, NWUSID's pair i both in
	// 'A'+i..'A'+i+15. [orig: CDKey_GenerateHardwareFingerprint @0x4A4A00 - 'A' + nibble @0x4A4BBA,
	//  'C' + nibble @0x4A4BE3, 'E' + the xor's high nibble @0x4A4BCD;
	//  CDKey_GenerateHardwareFingerprint_0 @0x4A4D00 - 'A' + i + nibble @0x4A4E46..0x4A4E4B]
	{
		for (const char *id : {"", "machine-a", "machine-b|Fixture GPU", "\x01\x02"}) {
			const nw::RetailMachineInputs in = nw::fallback_machine_inputs(id);
			const nw::LobbyMachineTokens fallback = nw::fallback_machine_tokens(id, kJoMix);
			const nw::LobbyMachineTokens windows = nw::make_retail_machine_tokens(in, kJoMix);
			expect(fallback.nwpssk == windows.nwpssk && fallback.nwusid == windows.nwusid,
			       "the fallback is the Windows encoders over the hash's words");
			expect(in.has_ethernet_address && in.volume_name.empty() && in.filesystem_name.empty(),
			       "the fallback supplies an adapter and no names");
			expect(fallback.nwpssk.size() == 23 && fallback.nwusid.size() == 16,
			       "the fallback's lengths are retail's (23 / 16)");
			bool banded = fallback.nwpssk.size() == 23;
			for (std::size_t k = 0; banded && k < fallback.nwpssk.size(); ++k) {
				const char base = "ACEACE"[k % 6];
				banded = fallback.nwpssk[k] >= base && fallback.nwpssk[k] <= base + 15;
			}
			expect(banded, "NWPSSK's letters are position-banded A..P / C..R / E..T");
			bool paired = fallback.nwusid.size() == 16;
			for (std::size_t k = 0; paired && k < fallback.nwusid.size(); ++k) {
				const char base = static_cast<char>('A' + k / 2);
				paired = fallback.nwusid[k] >= base && fallback.nwusid[k] <= base + 15;
			}
			expect(paired, "NWUSID's pair i is in 'A'+i..'A'+i+15");
			// The hashed adapter makes NWUSID's lead pair the Ethernet tag 77 (0x4D: 'A'+13,
			// 'A'+4).
			expect(fallback.nwusid.substr(0, 2) == "NE", "NWUSID leads with the adapter tag");
			expect(nw::fallback_machine_tokens(id, kJoMix).nwpssk == fallback.nwpssk,
					"the fallback is deterministic");
		}
		expect(nw::fallback_machine_tokens("machine-a", kJoMix).nwpssk !=
						nw::fallback_machine_tokens("machine-b", kJoMix).nwpssk,
				"different ids give different tokens");
	}

	// 7. Retail's stable volume/MAC identity transforms, pinned independently
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
		const nw::LobbyMachineTokens tokens = nw::make_retail_machine_tokens(in, kJoMix);
		expect(tokens.nwpssk == "ELHCDHDCSNDFDJMOGMJROAE",
		       "NWPSSK matches CDKey_GenerateHardwareFingerprint transform");
		expect(tokens.nwusid == "NEBBDDFFHHJJLLQR",
		       "NWUSID matches generate_hardware_fingerprint transform");
		// dfx2.exe's fingerprint differs in its fourth dword alone: (serial >> 3) ^ 0x32944845,
		// where JO's is (serial ^ 0x0B24B390) >> 3, so the last group's letters differ and NWUSID
		// does not. [orig: CDKey_GenerateHardwareFingerprint @0x4a4a60 - @0x4a4b7f, @0x4a4b89
		//  (dfx2.exe)]
		const nw::LobbyMachineTokens dfx2 = nw::make_retail_machine_tokens(in, kDfx2Mix);
		expect(dfx2.nwpssk == "ELHCDHDCSNDFDJMOGMODIAD", "DFX2's NWPSSK takes its own serial term");
		expect(dfx2.nwusid == tokens.nwusid, "the serial term is NWPSSK's alone");
	}

	// 8. D-NET-469: parse_host_and_port is retail's host-and-port split: the host before the first
	// colon (all of it without one), the port the napi literal's value after it, else 0; only a
	// string over 511 bytes fails. [orig: Network_ParseHostAndPort @0x62E200 - @0x62E260,
	// @0x62E297, @0x62E2B1, @0x62E2BD]
	{
		struct Case {
			const char *in;
			const char *host;
			uint32_t port;
		};
		const Case cases[] = {
				{"1.2.3.4:7597", "1.2.3.4", 7597},
				{"1.2.3.4:$7D00", "1.2.3.4", 32000},
				{"1.2.3.4:0x7D00", "1.2.3.4", 32000},
				{"1.2.3.4:076400", "1.2.3.4", 32000},
				{"1.2.3.4:32000d", "1.2.3.4", 32000},
				{"1.2.3.4:0o5", "1.2.3.4", 0},
				{"1.2.3.4:65536", "1.2.3.4", 65536},
				{"nohostport", "nohostport", 0},
				{"1.2.3.4:", "1.2.3.4", 0},
				{"1.2.3.4:abc", "1.2.3.4", 0},
				{"1.2.3.4:7597:9", "1.2.3.4", 7597},
				{"nw.example.com:1", "nw.example.com", 1},
				{"", "", 0},
		};
		for (const Case &c : cases) {
			std::string host = "unset";
			uint32_t port = 123;
			expect(nw::parse_host_and_port(c.in, host, port) && host == c.host && port == c.port,
			       c.in);
		}
		std::string host;
		uint32_t port = 0;
		const std::string nul_tail = std::string("5.6.7.8:9") + std::string(1, '\0') + "9";
		expect(nw::parse_host_and_port(nul_tail, host, port) && host == "5.6.7.8" && port == 9,
		       "parse_host_and_port reads to the first NUL");
		expect(nw::parse_host_and_port(std::string(511, 'a'), host, port) && port == 0,
		       "parse_host_and_port takes 511 bytes");
		host = "kept";
		port = 7;
		expect(!nw::parse_host_and_port(std::string(512, 'a'), host, port) && host == "kept" && port == 7,
		       "parse_host_and_port fails past 511 bytes, writing nothing");
	}

	// 9. D-NET-1334: NWHWI is "%s$%ld$%ld$%ldx%ld$%ldx%ld" over adapter 0's description (the
	// CRT's "(null)" with none), the display memory field (the MB count shifted into bytes in 32
	// bits, kept only when not negative, shifted back), the physical memory field
	// ((min(total, 0x7FFFFFFF) + 0xC0000) >> 20 in 32 bits), game.cfg's display mode and the
	// renderer's desktop. Values derived from the cited arithmetic.
	// [orig: CNapiGameSession_OnNovaWorldConnected @0x4D1570 - sprintf called @0x4D178F, `shr`
	//  @0x4D1771; CGfxDevice_QueryDxDiagDisplayMemory @0x67C820 - `shl` @0x67CA5C;
	//  CGfxDevice_CreateDevice @0x67E5D0 - @0x67EBFB..0x67EC25; Game_ParseCommandLineAndInit
	//  @0x4A7310 - @0x4A7DF2..0x4A7E15]
	{
		expect(nw::nwhwi_display_memory_mb(0) == 0, "display memory 0 prints 0");
		expect(nw::nwhwi_display_memory_mb(512) == 512, "display memory 512 MB prints 512");
		expect(nw::nwhwi_display_memory_mb(2047) == 2047, "2047 MB stays positive in 32 bits");
		expect(nw::nwhwi_display_memory_mb(2048) == 0,
		       "2048 MB is negative in 32 bits: the device keeps 0");
		expect(nw::nwhwi_display_memory_mb(4096) == 0, "4096 MB shifts out to 0");
		expect(nw::nwhwi_display_memory_mb(4608) == 512, "4608 MB keeps its low 11 bits, 512");
		expect(nw::nwhwi_display_memory_mb(8704) == 512, "8704 MB keeps its low 11 bits, 512");
		expect(nw::nwhwi_display_memory_mb(16271) == 0, "16271 MB has bit 11 set: 0");
		expect(nw::nwhwi_display_memory_mb(-1) == 0, "a failed query's -1 is negative: 0");

		// D-NET-1350: retail's 32-bit view of the count, the 64-bit count's minimum with
		// 0xFFFF0000 >> 20 (the 32-bit DXGI clamps the segment sizes' sum to 0xFFFF0000; the
		// witness is the platform's, lobby_vars.h), so any adapter past 4 GiB prints 0.
		expect(nw::kWow64DisplayMemoryCapMb == (0xFFFF0000ll >> 20), "the cap is the clamp's MB");
		expect(nw::wow64_display_memory_mb(0) == 0 && nw::wow64_display_memory_mb(1536) == 1536 &&
		               nw::wow64_display_memory_mb(4095) == 4095,
		       "a count at or under the clamp is the 64-bit count");
		expect(nw::wow64_display_memory_mb(4096) == 4095 && nw::wow64_display_memory_mb(16384) == 4095,
		       "a count past the clamp reads 4095 in 32 bits");
		expect(nw::nwhwi_display_memory_mb(nw::wow64_display_memory_mb(16384)) == 0 &&
		               nw::nwhwi_display_memory_mb(nw::wow64_display_memory_mb(3000)) == 0 &&
		               nw::nwhwi_display_memory_mb(nw::wow64_display_memory_mb(2047)) == 2047,
		       "NWHWI prints 0 for any 32-bit count with bit 11 set, 4095 included");

		expect(nw::nwhwi_physical_memory_mb(0) == 0, "no memory prints 0");
		expect(nw::nwhwi_physical_memory_mb(0x20000000u) == 512, "512 MiB prints 512");
		expect(nw::nwhwi_physical_memory_mb(0x3FF3FFFFu) == 1023, "under 1023.25 MiB rounds down");
		expect(nw::nwhwi_physical_memory_mb(0x3FF40000u) == 1024, "from 1023.25 MiB rounds up");
		expect(nw::nwhwi_physical_memory_mb(0x7FFFFFFFu) == 2048, "2 GiB less a byte prints 2048");
		expect(nw::nwhwi_physical_memory_mb(0x400000000ull) == 2048,
		       "16 GiB is capped at 0x7FFFFFFF for the non-LAA image: 2048");

		nw::HardwareIdentity hw;
		hw.has_adapter = true;
		hw.adapter_description = "Fixture $Adapter";
		hw.display_memory_mb = 256;
		hw.total_phys_bytes = 0x10000000u;
		hw.display_width = 1024;
		hw.display_height = 768;
		hw.desktop_width = 1920;
		hw.desktop_height = 1080;
		expect(nw::make_nwhwi(hw) == "Fixture $Adapter$256$256$1024x768$1920x1080",
		       "NWHWI prints the five fields in order, the description as it is");
		nw::HardwareIdentity none;
		expect(nw::make_nwhwi(none) == "(null)$0$0$0x0$0x0", "no adapter prints the CRT's (null)");
	}

	if (g_fail == 0) {
		std::printf("lobby_vars_test: all checks passed\n");
	}
	return g_fail == 0 ? 0 : 1;
}
