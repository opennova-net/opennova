// A Serve Only host's NovaWorld registration against the witnessed retail var
// lists (ADR 0051 d6; net-re "A Serve Only host's registration"): what
// opennova-serve's live source and the lister make of a host file, built into
// HostSetup, the Host list and the PlayerList by the engine's builders, name
// for name and value for value where retail's dedicated arm decides it.
// Server-free, socket-free, no retail data.
// [orig: CNapiGameSession_BuildHostVarLists @0x4d0b50; CNapiGameSession_ConnectOrHost
//  @0x4d5046..0x4d507b; Lobby_UpdateServerInfo @0x4fe8c0; Server_PlayerAdd @0x51d441..0x51d4aa]

#include "serve_listing.h"

#include <net/novaworld/connect_or_host.h>
#include <net/novaworld/lobby_vars.h>
#include <runtime/inmatch/host_settings.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

std::vector<std::string> names(const std::vector<ClientVar> &vars) {
	std::vector<std::string> out;
	for (const ClientVar &v : vars) out.push_back(v.name);
	return out;
}

std::string value(const std::vector<ClientVar> &vars, const char *name, int fnum = 0) {
	for (const ClientVar &v : vars)
		if (v.fnum == fnum && v.name == name) return v.value;
	return "<absent>";
}

// The retail strings, in retail's insertion order.
// HostSetup: BuildHostVarLists' +460 run, then InitHeapsAndSerializeCounter's ReconnectCounter
// [orig: @0x4d0b50..0x4d0e08; @0x4ce36d].
const std::vector<std::string> kHostSetup = {"LobbyName", "ServerName", "Msg", "MaxPlayers", "Password",
		"Dedicated", "AppId", "AccessCodeList", "PLoad", "Exp", "LAN", "ReconnectCounter"};
// The Host list a ClientHostRequest carries: BuildHostVarLists' +532 run.
const std::vector<std::string> kHostInitial = {"LobbyName", "ServerName", "Msg", "MaxPlayers", "AppId",
		"PLoad", "Exp", "LAN"};
// The refreshed Host list: the initial eight, then Lobby_UpdateServerInfo's SetOrCreate run (an
// existing name updates in place). CountryName / Lang / TZB follow the gate's METEXT.
std::vector<std::string> host_full(bool met_ext) {
	std::vector<std::string> out = kHostInitial;
	for (const char *name : {"HostKey", "GameType", "MissionName", "Region", "Players", "MI1", "MI2", "MI3",
				"Dedicated", "Locked", "Skins", "TimeLeft", "Password", "Tracers", "Mod", "Country", "Port",
				"AllowPing", "Age", "TimeOfDay", "PCIDKey", "GameServerBaffleKey", "Stat", "LevelRange",
				"BBMode", "GCC", "GV", "Version"})
		out.push_back(name);
	if (met_ext)
		for (const char *name : {"CountryName", "Lang", "TZB"}) out.push_back(name);
	for (const char *name : {"Ver1", "Ver2", "PBServer"}) out.push_back(name);
	return out;
}

// What the server hands the live source for a host file with `MaxPlayers player_limit`
// (Server::listing_columns), then what the lister and the host role lay over it.
HostRegistration serve_only_registration(int player_limit, bool met_ext, int joiners) {
	HostRegistration base;
	base.server_name = "Serve NW";
	base.server_message = "hello";
	base.max_players = player_limit;
	base.published_cap = static_cast<int>(inmatch::host_player_slot_limit(player_limit, false));
	base.listen_host = false;
	serve::ServeListing source(base);
	HostRegistration r = source.registration();
	// The lister: the gate's lobby, the leg's clamp (1..65 for a dedicated host).
	r.lobby_name = "jop_2_consumer";
	r.max_players = host_leg_max_players(r.max_players, !r.listen_host);
	// The host role: the gate's METEXT, the Players count (the roster: no host slot).
	r.met_ext = met_ext;
	r.player_count = joiners;
	r.app_id = 4242;
	return r;
}

} // namespace

int main() {
	HostLobbyText text = make_host_lobby_text([](const char *section, const char *key, std::string &out) {
		const std::string s = section, k = key;
		if (s == "NovaWorld" && k == "STRNOVA11") out = "Yes";
		else if (s == "NovaWorld" && k == "STRNOVA12") out = "No";
		else if (s == "NovaWorld" && k == "STRNOVA10") out = "None";
		else return false;
		return true;
	});
	CHECK(text.yes == "Yes" && text.no == "No" && text.no_time_limit == "None");
	CHECK(text.region[0].empty()); // STRNOVA07 missing: no fallback
	CHECK(text.time_of_day[2] == "Day"); // TimeOfDay missing: the witnessed literal

	{
		const HostRegistration r = serve_only_registration(8, /*met_ext=*/false, 0);
		const std::vector<ClientVar> setup = make_host_setup_var_list(r);
		CHECK(names(setup) == kHostSetup);
		// Dedicated "1" for a host that is not a peer [orig: @0x4d0c4b..0x4d0c60].
		CHECK(value(setup, "Dedicated") == "1");
		// The host file's cap, never the +1 dedicated slot.
		CHECK(value(setup, "MaxPlayers") == "8");
		CHECK(value(setup, "PLoad").empty());
		CHECK(value(setup, "ReconnectCounter") == "0");

		CHECK(names(make_host_var_list(r, text, /*full=*/false)) == kHostInitial);

		const std::vector<ClientVar> host = make_host_var_list(r, text, /*full=*/true);
		CHECK(names(host) == host_full(false));
		// Dedicated is STRNOVA11 for a non-peer [orig: @0x4febe2..0x4fec52].
		CHECK(value(host, "Dedicated") == "Yes");
		// MaxPlayers is the published cap (9) less the dedicated slot [orig: @0x4feb03..0x4feb0a].
		CHECK(value(host, "MaxPlayers") == "8");
		// Players skips the host's own slot [orig: @0x4feaa6..0x4feacb].
		CHECK(value(host, "Players") == "0");
		// No address of the host's own: Port is the literal "-1" [orig: @0x4fef6d].
		CHECK(value(host, "Port") == "-1");
		CHECK(value(host, "ServerIP") == "<absent>" && value(host, "ServerPortNumber") == "<absent>");
		CHECK(value(host, "Msg") == "hello");
	}
	{
		// The ConnectOrHost clamp: 1..65 for a dedicated host; the Host list's cap is the in-game
		// published cap (the 65 ceiling with the dedicated slot) less that slot.
		const HostRegistration r = serve_only_registration(70, /*met_ext=*/true, 2);
		CHECK(value(make_host_setup_var_list(r), "MaxPlayers") == "65");
		const std::vector<ClientVar> host = make_host_var_list(r, text, /*full=*/true);
		CHECK(names(host) == host_full(true));
		CHECK(value(host, "MaxPlayers") == "64");
		CHECK(value(host, "Players") == "2");
	}
	{
		// The PlayerList: five vars per published slot, the joiner's UDP source as inet_ntoa:port,
		// and no slot 0 (a mode-1 host builds no local connection) [orig: Server_PlayerAdd
		// @0x51d3d1..0x51d4aa; CNapiGameSession_CreateSession @0x4c9b73].
		HostPlayerSlot joiner;
		joiner.slot = 1;
		joiner.player_name = "ListedJoiner";
		joiner.ip_and_port = "10.0.0.7:32800";
		joiner.team = "1";
		joiner.type = "0";
		const std::vector<ClientVar> players = make_player_list({joiner});
		CHECK(players.size() == 5);
		CHECK(value(players, "PlayerName", 0) == "<absent>");
		CHECK(value(players, "PlayerIpAndPort", 1) == "10.0.0.7:32800");
		const std::vector<std::string> kSlot = {"PlayerName", "PlayerIpAndPort", "PlayerPCID", "PlayerTeam",
				"PlayerType"};
		CHECK(names(players) == kSlot);
	}

	if (failures != 0) {
		std::printf("serve_registration_parity: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("serve_registration_parity: ok\n");
	return 0;
}
