// LobbySession dispatch tests.
//
// Validates the lobby message handlers against the container shapes the
// standalone server's nw_udp_listener sees after napi_stream_decode: the
// retail ClientHostRequest / ClientHostUpdate var lists
// [orig: CNapiGameSession_SendHostRequest @0x4d37b4..0x4d37f6], the indexed
// PlayerList [orig: Server_PlayerAdd @0x51d441..0x51d4aa] and the GLSVSS leg.

#include <net/novaworld/lobby_session.h>

#include "../common/test_expect.h"
#include "client_var_fixture.h"

#include <cstdio>
#include <string>
#include <vector>

using opennova::extract_var_lists;
using opennova::LobbyDispatchResult;
using opennova::LobbySession;
using opennova::LobbyState;
using opennova::NapiField;
using opennova::NapiMessage;
using opennova::var_has;
using opennova::var_value;
using test_novaworld::make_client_var_list;
using test_novaworld::make_indexed_var_list;
using test_novaworld::player_slot_vars;

namespace {

const NapiField *find_field(const NapiMessage &m, const std::string &name) {
	for (const auto &f : m.fields) if (f.name == name) return &f;
	return nullptr;
}

std::string field_str(const NapiField *f) {
	if (!f) return {};
	std::string s(f->data.begin(), f->data.end());
	while (!s.empty() && s.back() == '\0') s.pop_back();
	return s;
}

const NapiMessage *find_child(const NapiMessage &m, const std::string &name) {
	for (const auto &c : m.children) if (c.name == name) return &c;
	return nullptr;
}

// A ClientHostRequest in the retail shape: Cookie (the login cookie jar +
// locale, no NWUID), HostSetup, Host, PlayerList, CurrentlyHosting=0.
NapiMessage make_retail_host_request(const std::string &app_id,
                                     const std::vector<test_novaworld::IndexedVar> &players = {}) {
	NapiMessage in;
	in.name = "ClientHostRequest";
	in.fields.push_back({"CurrentlyHosting", std::vector<uint8_t>{'0'}});
	in.fields.push_back({"VarCheck", std::vector<uint8_t>{'1'}});
	in.children.push_back(make_client_var_list("Cookie", {
		{"NWHANDLE", "Host"}, {"PCID", "00000002"}, {"NWH", "1"},
		{"CountryName", "United States"}, {"Language", "English"}, {"TimeZoneBias", "300"},
	}));
	in.children.push_back(make_client_var_list("HostSetup", {
		{"LobbyName", "jop_2_consumer"},
		{"ServerName", "MyServer"},
		{"Msg", "hello"},
		{"MaxPlayers", "16"},
		{"Password", "0"},
		{"Dedicated", "1"},
		{"AppId", app_id},
		{"AccessCodeList", ""},
		{"PLoad", ""},
		{"Exp", ""},
		{"LAN", "0"},
	}));
	in.children.push_back(make_client_var_list("Host", {
		{"LobbyName", "jop_2_consumer"},
		{"HostKey", "HK-" + app_id},
		{"ServerName", "MyServer"},
		{"GameType", "TDM"},
		{"MissionName", "ASH_G11A"},
		{"Region", "North America"},
		{"Players", "3"},
		{"MaxPlayers", "16"},
		{"MI1", "0"}, {"MI2", "0"}, {"MI3", "0"},
		{"Dedicated", "Yes"},
		{"Locked", "No"},
		{"Skins", "Yes"},
		{"TimeLeft", "45"},
		{"Password", "No"},
		{"Tracers", "Yes"},
		{"Mod", " "},
		{"Country", "US"},
		{"Msg", "hello"},
		{"Port", "-1"},
		{"AllowPing", "y"},
		{"Age", "0 00:12:34"},
		{"TimeOfDay", "Dawn"},
		{"AppID", app_id},
		{"PCIDKey", "16777216"},
		{"GameServerBaffleKey", "0"},
		{"Stat", "N"},
		{"LevelRange", " "},
		{"BBMode", "0"},
		{"GCC", ""},
		{"GV", "1.7.5.7"},
		{"Version", "1.7.5.7"},
		{"Ver1", "3"},
		{"Ver2", "2345"},
		{"PBServer", "0"},
	}));
	in.children.push_back(make_indexed_var_list("PlayerList", players));
	return in;
}

int test_extract_var_lists_keeps_indexed_entries() {
	NapiMessage outer;
	outer.name = "ClientHostRequest";
	outer.children.push_back(make_client_var_list("HostSetup", {
		{"AppId", "1234"},
		{"LobbyName", "jop_2_consumer"},
		{"MaxPlayers", "16"},
	}));
	std::vector<test_novaworld::IndexedVar> players;
	for (const auto &v : player_slot_vars(0, "alice", "10.0.0.2:32768", "00000002", "1", "0")) players.push_back(v);
	for (const auto &v : player_slot_vars(1, "bob",   "10.0.0.3:32768", "00000003", "2", "0")) players.push_back(v);
	outer.children.push_back(make_indexed_var_list("PlayerList", players));
	auto vl = extract_var_lists(outer);
	TEST_EXPECT(vl.size() == 2);
	TEST_EXPECT(var_value(vl["HostSetup"], "AppId") == "1234");
	TEST_EXPECT(var_value(vl["HostSetup"], "appid") == "1234"); // case-insensitive
	TEST_EXPECT(var_value(vl["HostSetup"], "LobbyName") == "jop_2_consumer");
	// Two players keep two (fnum, PlayerName) entries instead of collapsing.
	TEST_EXPECT(vl["PlayerList"].size() == 10);
	TEST_EXPECT(var_value(vl["PlayerList"], "PlayerName", 0) == "alice");
	TEST_EXPECT(var_value(vl["PlayerList"], "PlayerName", 1) == "bob");
	TEST_EXPECT(var_value(vl["PlayerList"], "PlayerPCID", 1) == "00000003");
	TEST_EXPECT(!var_has(vl["PlayerList"], "PlayerName", 2));
	auto roster = opennova::roster_from_player_list(vl["PlayerList"]);
	TEST_EXPECT(roster.size() == 2);
	TEST_EXPECT(roster[0].slot == 0 && roster[0].player_name == "alice" && roster[0].team == "1");
	TEST_EXPECT(roster[1].slot == 1 && roster[1].player_name == "bob" && roster[1].ip_and_port == "10.0.0.3:32768");
	return 0;
}

int test_client_connected_returns_server_start_verify() {
	LobbySession sess;
	LobbyState state;
	NapiMessage in;
	in.name = "ClientConnected";
	auto r = sess.dispatch(in, state, "127.0.0.1", 32768);
	TEST_EXPECT(r.label == "ClientConnected");
	TEST_EXPECT(r.reply_containers.size() == 1);
	TEST_EXPECT(r.reply_containers[0].name == "ServerStartVerify");
	TEST_EXPECT(r.reply_containers[0].fields.empty());
	TEST_EXPECT(r.reply_containers[0].children.empty());
	return 0;
}

int test_client_request_verify_result_returns_server_verify_result() {
	LobbySession sess;
	sess.set_sess_id_generator([] { return std::string("deadbeef00000000feedface00000000"); });
	LobbyState state;
	NapiMessage in;
	in.name = "ClientRequestVerifyResult";
	auto r = sess.dispatch(in, state, "127.0.0.1", 32768);
	TEST_EXPECT(r.label == "ClientRequestVerifyResult");
	TEST_EXPECT(r.reply_containers.size() == 1);
	const auto &reply = r.reply_containers[0];
	TEST_EXPECT(reply.name == "ServerVerifyResult");
	TEST_EXPECT(field_str(find_field(reply, "Success")) == "1");
	TEST_EXPECT(field_str(find_field(reply, "SessIdString")) == "deadbeef00000000feedface00000000");

	const auto *var_list = find_child(reply, "ServerVarList");
	TEST_EXPECT(var_list != nullptr);
	TEST_EXPECT(field_str(find_field(*var_list, "VarList")) == "ConnectCommands");

	// State should remember the SessIdString so a re-issued
	// ClientRequestVerifyResult returns the same id.
	TEST_EXPECT(state.sess_id_string == "deadbeef00000000feedface00000000");
	return 0;
}

int test_retail_host_request_returns_server_host_result_with_gsid() {
	LobbySession sess;
	sess.set_gsid_generator([](const std::string &app) {
		return std::string("GSID-10-") + app + "-FIXED";
	});
	sess.set_rid_generator([] { return uint32_t{0x0A001234u}; });

	LobbyState state;
	std::vector<test_novaworld::IndexedVar> players;
	for (const auto &v : player_slot_vars(0, "Host", "10.0.0.1:32768", "00000002", "1", "0")) players.push_back(v);
	for (const auto &v : player_slot_vars(3, "carol", "10.0.0.9:32768", "00000009", "2", "0")) players.push_back(v);
	auto in = make_retail_host_request("1234", players);

	auto r = sess.dispatch(in, state, "10.0.0.1", 32768);
	TEST_EXPECT(r.label == "ClientHostRequest");
	TEST_EXPECT(r.reply_containers.size() == 1);
	const auto &reply = r.reply_containers[0];
	TEST_EXPECT(reply.name == "ServerHostResult");
	TEST_EXPECT(field_str(find_field(reply, "Success"))   == "1");
	TEST_EXPECT(field_str(find_field(reply, "MsgCode"))   == "0");
	TEST_EXPECT(field_str(find_field(reply, "MsgParam2")) == "17");
	TEST_EXPECT(field_str(find_field(reply, "Rid"))       == std::to_string(0x0A001234u));

	const auto *cmds = find_child(reply, "ServerVarList");
	TEST_EXPECT(cmds != nullptr);
	TEST_EXPECT(field_str(find_field(*cmds, "VarList")) == "HostCommands");
	TEST_EXPECT(cmds->children.size() == 2);
	TEST_EXPECT(cmds->children[0].name == "ServerVar");
	TEST_EXPECT(field_str(find_field(cmds->children[0], "VarName"))  == "HostRequiresJoinTicket");
	TEST_EXPECT(field_str(find_field(cmds->children[0], "VarValue")) == "0");
	TEST_EXPECT(cmds->children[1].name == "ServerVar");
	TEST_EXPECT(field_str(find_field(cmds->children[1], "VarName"))  == "GSID");
	TEST_EXPECT(field_str(find_field(cmds->children[1], "VarValue")) == "GSID-10-1234-FIXED");

	// Per-connection state should reflect the host registration.
	TEST_EXPECT(state.hosting);
	TEST_EXPECT(state.gsid == "GSID-10-1234-FIXED");
	TEST_EXPECT(state.rid == 0x0A001234u);
	TEST_EXPECT(state.game == "jop_2_consumer");
	TEST_EXPECT(state.app_id == "1234");
	// Port = "-1": the joinable endpoint is the observed UDP source.
	TEST_EXPECT(state.host_ip == "10.0.0.1");
	TEST_EXPECT(state.host_port == 32768);
	TEST_EXPECT(state.server_name == "MyServer");
	TEST_EXPECT(state.player_count == 3);
	TEST_EXPECT(state.max_players == 16);
	TEST_EXPECT(state.region == "North America");
	// The host's keys ride the request's Host list, not only the update.
	TEST_EXPECT(state.host_key == "HK-1234");
	TEST_EXPECT(state.pcid_key == "16777216");
	// The browser columns.
	TEST_EXPECT(state.game_type == "TDM");
	TEST_EXPECT(state.mission_name == "ASH_G11A");
	TEST_EXPECT(state.time_left == "45");
	TEST_EXPECT(state.time_of_day == "Dawn");
	TEST_EXPECT(state.msg == "hello");
	TEST_EXPECT(state.age == "0 00:12:34");
	TEST_EXPECT(state.pb_server == "0");
	TEST_EXPECT(state.dedicated == "Yes");   // the localized token is stored as sent
	TEST_EXPECT(state.skins == "Yes");
	TEST_EXPECT(state.level_range == " ");
	// The roster keeps one slot per VarFNum.
	TEST_EXPECT(state.roster.size() == 2);
	TEST_EXPECT(state.roster[0].slot == 0 && state.roster[0].player_name == "Host");
	TEST_EXPECT(state.roster[1].slot == 3 && state.roster[1].player_name == "carol");
	return 0;
}

int test_host_port_override_when_positive() {
	LobbySession sess;
	sess.set_gsid_generator([](const std::string &) { return std::string("FIXED"); });
	LobbyState state;
	auto in = make_retail_host_request("999");
	// Replace Port = -1 with a positive value: it overrides the observed port.
	for (auto &child : in.children) {
		if (field_str(find_field(child, "VarList")) != "Host") continue;
		for (auto &var : child.children) {
			if (field_str(find_field(var, "VarName")) == "Port") {
				var.fields.clear();
				var.fields.push_back({"VarFNum", std::vector<uint8_t>{'0'}});
				var.fields.push_back({"VarName", std::vector<uint8_t>{'P','o','r','t'}});
				var.fields.push_back({"VarValue", std::vector<uint8_t>{'1','7','4','7','5'}});
			}
		}
	}
	auto r = sess.dispatch(in, state, "10.0.0.1", 64500);
	TEST_EXPECT(r.reply_containers.size() == 1);
	TEST_EXPECT(state.host_ip == "10.0.0.1");
	TEST_EXPECT(state.host_port == 17475);
	return 0;
}

// Retail's Host list carries no address (Port = "-1"); the host player's own
// slot-0 PlayerIpAndPort is the game endpoint [orig: Server_PlayerAdd
// @0x51d45c]. Service policy: slot 0 first, then a positive Port, then the
// observed source; an empty ip half (an OpenNova host with no advertised
// address) takes the observed source address.
int test_slot0_ip_and_port_selects_game_endpoint() {
	LobbySession sess;
	sess.set_gsid_generator([](const std::string &) { return std::string("FIXED"); });
	{
		LobbyState state;
		std::vector<test_novaworld::IndexedVar> players;
		for (const auto &v : player_slot_vars(0, "Host", "10.0.0.5:32780", "", "1", "0")) players.push_back(v);
		for (const auto &v : player_slot_vars(1, "bob",  "10.0.0.6:32768", "", "2", "0")) players.push_back(v);
		auto r = sess.dispatch(make_retail_host_request("777", players), state, "10.0.0.1", 64500);
		TEST_EXPECT(r.reply_containers.size() == 1);
		TEST_EXPECT(state.host_ip == "10.0.0.5");
		TEST_EXPECT(state.host_port == 32780);
	}
	{
		// No advertised address: ":port" keeps the observed source address.
		LobbyState state;
		std::vector<test_novaworld::IndexedVar> players;
		for (const auto &v : player_slot_vars(0, "Host", ":32780", "", "1", "0")) players.push_back(v);
		auto r = sess.dispatch(make_retail_host_request("778", players), state, "10.0.0.1", 64500);
		TEST_EXPECT(r.reply_containers.size() == 1);
		TEST_EXPECT(state.host_ip == "10.0.0.1");
		TEST_EXPECT(state.host_port == 32780);
	}
	{
		// A registration without a roster (the pre-mission ClientHostRequest)
		// falls back to the observed source; the host's own ClientHostPlayerAdded
		// for slot 0 then resolves the endpoint and the roster follows the deltas.
		LobbyState state;
		auto r = sess.dispatch(make_retail_host_request("779"), state, "10.0.0.1", 64500);
		TEST_EXPECT(r.reply_containers.size() == 1);
		TEST_EXPECT(state.host_port == 64500);
		NapiMessage added;
		added.name = "ClientHostPlayerAdded";
		auto put = [&added](const char *k, const std::string &v) {
			added.fields.push_back({k, std::vector<uint8_t>(v.begin(), v.end())});
		};
		put("PlayerNumber", "0"); put("PlayerName", "Host"); put("PlayerIpAndPort", "10.0.0.5:32780");
		put("PlayerPCID", ""); put("PlayerTeam", "1"); put("PlayerType", "0");
		auto ra = sess.dispatch(added, state, "10.0.0.1", 64500);
		TEST_EXPECT(ra.label == "ClientHostPlayerAdded" && ra.reply_containers.empty());
		TEST_EXPECT(state.host_ip == "10.0.0.5" && state.host_port == 32780);
		TEST_EXPECT(state.roster.size() == 1 && state.player_count == 1);
		added.fields.clear();
		put("PlayerNumber", "1"); put("PlayerName", "carol"); put("PlayerIpAndPort", "10.0.0.7:32768");
		put("PlayerPCID", ""); put("PlayerTeam", "2"); put("PlayerType", "0");
		sess.dispatch(added, state, "10.0.0.1", 64500);
		TEST_EXPECT(state.roster.size() == 2 && state.player_count == 2);
		TEST_EXPECT(state.roster[1].slot == 1 && state.roster[1].player_name == "carol");
		NapiMessage removed;
		removed.name = "ClientHostPlayerRemoved";
		removed.fields.push_back({"PlayerNumber", std::vector<uint8_t>{'1'}});
		auto rr = sess.dispatch(removed, state, "10.0.0.1", 64500);
		TEST_EXPECT(rr.label == "ClientHostPlayerRemoved");
		TEST_EXPECT(state.roster.size() == 1 && state.player_count == 1);
		// The host endpoint survives a joiner leaving.
		TEST_EXPECT(state.host_ip == "10.0.0.5" && state.host_port == 32780);
	}
	return 0;
}

int test_two_hosts_with_colliding_app_ids_get_distinct_rids() {
	// Retail AppId is a per-session random in [1000, 9999]
	// [orig: CNapiNetwork_RandomizeTimeout @0x4c4d9a]: 1000 and 5096 share
	// their low twelve bits, so the RID must not be derived from it.
	LobbySession sess;
	LobbyState a, b;
	auto ra = sess.dispatch(make_retail_host_request("1000"), a, "10.0.0.1", 32768);
	auto rb = sess.dispatch(make_retail_host_request("5096"), b, "10.0.0.2", 32768);
	TEST_EXPECT(ra.reply_containers.size() == 1 && rb.reply_containers.size() == 1);
	TEST_EXPECT(a.rid != 0 && b.rid != 0);
	TEST_EXPECT(a.rid != b.rid);
	TEST_EXPECT((a.rid & 0xFF000000u) == 0x0A000000u);
	TEST_EXPECT((b.rid & 0xFF000000u) == 0x0A000000u);
	// A re-sent request keeps the rid it was given.
	auto ra2 = sess.dispatch(make_retail_host_request("1000"), a, "10.0.0.1", 32768);
	TEST_EXPECT(field_str(find_field(ra2.reply_containers[0], "Rid")) == std::to_string(a.rid));
	return 0;
}

int test_legacy_host_request_extracts_gsb_fields() {
	// The pre-retail-shape OpenNova host request (alternate spellings) still
	// registers.
	LobbySession sess;
	sess.set_gsid_generator([](const std::string &) { return std::string("FIXED"); });
	sess.set_rid_generator([] { return uint32_t{0x0A000002u}; });
	LobbyState state;
	NapiMessage in;
	in.name = "ClientHostRequest";
	in.children.push_back(make_client_var_list("HostSetup", {
		{"AppId", "1234"},
		{"LobbyName", "jop_2_consumer"},
		{"MaxPlayers", "16"},
		{"Dedicated", "0"},
		{"ExpBits", "3"},
		{"VER1", "3"},
	}));
	in.children.push_back(make_client_var_list("Host", {
		{"ServerName", "GSB Fields"},
		{"GameType", "COOP"},
		{"MissionName", "ASH_G11A"},
		{"Country", "US"},
		{"Passworded", "1"},
		{"Locked", "1"},
		{"StatsEnabled", "1"},
		{"Exp", "JO"},
		{"Joicon2", "4000"},
	}));

	auto r = sess.dispatch(in, state, "10.0.0.1", 64500);
	TEST_EXPECT(r.reply_containers.size() == 1);
	TEST_EXPECT(state.host_ip == "10.0.0.1");
	TEST_EXPECT(state.host_port == 64500);
	TEST_EXPECT(state.game_type == "COOP");
	TEST_EXPECT(state.mission_name == "ASH_G11A");
	TEST_EXPECT(state.country == "US");
	TEST_EXPECT(state.password == "Y");
	TEST_EXPECT(state.locked == "Y");
	TEST_EXPECT(state.dedicated == "N");
	TEST_EXPECT(state.stat == "Y");
	TEST_EXPECT(state.exp == "JO");
	TEST_EXPECT(state.exp_bits == "3");
	TEST_EXPECT(state.ver1 == "3");
	TEST_EXPECT(state.joicon2 == "4000");
	return 0;
}

int test_client_host_update_silent_with_state_refresh() {
	LobbySession sess;
	LobbyState state;
	state.rid = 0x0A000005u;
	state.host_ip = "10.10.10.10";
	state.host_port = 17500;
	state.pcid_key = "16777216";
	NapiMessage in;
	in.name = "ClientHostUpdate";
	in.children.push_back(make_client_var_list("Host", {
		{"HostKey", "ABC123"},
		{"PCIDKey", "16777217"},   // rotated since the request
		{"ServerName", "Renamed"},
		{"Players", "5"},
		{"MaxPlayers", "32"},
		{"Region", "eu"},
		{"TimeLeft", "12"},
		{"TimeOfDay", "Night"},
		{"Port", "-1"},
	}));
	std::vector<test_novaworld::IndexedVar> players;
	for (const auto &v : player_slot_vars(2, "dave", "10.0.0.4:32768", "00000004", "1", "0")) players.push_back(v);
	in.children.push_back(make_indexed_var_list("PlayerList", players));
	auto r = sess.dispatch(in, state, "1.2.3.4", 99);
	TEST_EXPECT(r.label == "ClientHostUpdate");
	TEST_EXPECT(r.reply_containers.empty()); // silent
	TEST_EXPECT(state.host_key == "ABC123");
	TEST_EXPECT(state.pcid_key == "16777217");
	// Port = -1 leaves the stored endpoint alone.
	TEST_EXPECT(state.host_ip == "10.10.10.10");
	TEST_EXPECT(state.host_port == 17500);
	TEST_EXPECT(state.server_name == "Renamed");
	TEST_EXPECT(state.player_count == 5);
	TEST_EXPECT(state.max_players == 32);
	TEST_EXPECT(state.region == "eu");
	TEST_EXPECT(state.time_left == "12");
	TEST_EXPECT(state.time_of_day == "Night");
	TEST_EXPECT(var_value(state.last_host_update["Host"], "HostKey") == "ABC123");
	TEST_EXPECT(state.roster.size() == 1);
	TEST_EXPECT(state.roster[0].slot == 2 && state.roster[0].player_name == "dave");
	return 0;
}

int test_client_player_enter_request_returns_result() {
	LobbySession sess;
	LobbyState state;
	NapiMessage in;
	in.name = "ClientPlayerEnterRequest";
	in.fields.push_back({"ConnectionId", std::vector<uint8_t>{'4','2'}});
	in.fields.push_back({"IpAddress", std::vector<uint8_t>{'2','0','8','6','5','3','4','4','8','0'}});
	in.fields.push_back({"PortNumber", std::vector<uint8_t>{'6','2','6','0','0'}});
	in.fields.push_back({"Pcid", std::vector<uint8_t>{'0','0','0','0','0','0','0','2'}});
	auto r = sess.dispatch(in, state, "127.0.0.1", 32768);
	TEST_EXPECT(r.label == "ClientPlayerEnterRequest");
	TEST_EXPECT(r.reply_containers.size() == 1);
	const auto &reply = r.reply_containers[0];
	TEST_EXPECT(reply.name == "ServerPlayerEnterResult");
	TEST_EXPECT(field_str(find_field(reply, "ConnectionID")) == "42");
	TEST_EXPECT(field_str(find_field(reply, "Pcid")) == "00000002");
	TEST_EXPECT(field_str(find_field(reply, "Success")) == "1");
	TEST_EXPECT(field_str(find_field(reply, "MsgCode")) == "0");
	// The joiner's reported dcb + game endpoint are surfaced for the host to
	// stamp into the in-match 0x0C entity_flags (correlate by game port).
	TEST_EXPECT(r.has_player_enter);
	TEST_EXPECT(r.player_connection_id == 42);
	TEST_EXPECT(r.player_game_port == 62600);
	TEST_EXPECT(r.player_ip_field == "2086534480");
	return 0;
}

int test_stop_hosting_and_stop_playing_are_silent_lifecycle_messages() {
	LobbySession sess;
	LobbyState state;
	state.hosting = true;
	state.rid = 0x0A000099u;
	state.player_count = 3;
	NapiMessage stop_hosting;
	stop_hosting.name = "ClientStopHosting";
	auto r1 = sess.dispatch(stop_hosting, state, "127.0.0.1", 32768);
	TEST_EXPECT(r1.label == "ClientStopHosting");
	TEST_EXPECT(r1.reply_containers.empty());
	TEST_EXPECT(!state.hosting);
	TEST_EXPECT(state.player_count == 0);

	state.play_state["PlaySetup"].push_back({0, "GSID", "fixed"});
	NapiMessage stop_playing;
	stop_playing.name = "ClientStopPlaying";
	auto r2 = sess.dispatch(stop_playing, state, "127.0.0.1", 32768);
	TEST_EXPECT(r2.label == "ClientStopPlaying");
	TEST_EXPECT(r2.reply_containers.empty());
	TEST_EXPECT(state.play_state.empty());
	return 0;
}

int test_client_play_request_returns_server_play_result() {
	LobbySession sess;
	LobbyState state;
	NapiMessage in;
	in.name = "ClientPlayRequest";
	in.children.push_back(make_client_var_list("PlaySetup", {
		{"GSID", "fixed-gsid"},
	}));
	auto r = sess.dispatch(in, state, "127.0.0.1", 32768);
	TEST_EXPECT(r.label == "ClientPlayRequest");
	TEST_EXPECT(r.reply_containers.size() == 1);
	const auto &reply = r.reply_containers[0];
	TEST_EXPECT(reply.name == "ServerPlayResult");
	TEST_EXPECT(field_str(find_field(reply, "Success")) == "1");
	TEST_EXPECT(field_str(find_field(reply, "MsgCode")) == "0");
	TEST_EXPECT(field_str(find_field(reply, "MsgParam2")) == "34");
	const auto *cmds = find_child(reply, "ServerVarList");
	TEST_EXPECT(cmds != nullptr);
	TEST_EXPECT(field_str(find_field(*cmds, "VarList")) == "PlayCommands");
	TEST_EXPECT(var_value(state.play_state["PlaySetup"], "GSID") == "fixed-gsid");
	return 0;
}

int test_glsvss_request_answers_with_results_only_when_configured() {
	// [orig: CNapiGameSession_SendGLSVSSRequest @0x4d3a70 / HandleGLSVSSResults @0x4d33be]
	LobbySession sess;
	LobbyState state;
	NapiMessage in;
	in.name = "ClientGLSVSSRequest";
	in.fields.push_back({"GLSVSSRequest", std::vector<uint8_t>{'a','b','c'}});
	in.children.push_back(make_client_var_list("Cookie", {{"NWHANDLE", "Host"}}));

	auto r = sess.dispatch(in, state, "127.0.0.1", 32768);
	TEST_EXPECT(r.label == "ClientGLSVSSRequest");
	TEST_EXPECT(r.reply_containers.size() == 1);
	TEST_EXPECT(r.reply_containers[0].name == "ServerGLSVSSResults");
	// Nothing configured: the param the consumer keys on is absent (no-op).
	TEST_EXPECT(find_field(r.reply_containers[0], "GLSVSSResults") == nullptr);

	sess.set_glsvss_results("CHARDATA=1");
	auto r2 = sess.dispatch(in, state, "127.0.0.1", 32768);
	TEST_EXPECT(r2.reply_containers.size() == 1);
	TEST_EXPECT(field_str(find_field(r2.reply_containers[0], "GLSVSSResults")) == "CHARDATA=1");
	return 0;
}

int test_update_vars_is_accepted_without_reply() {
	LobbySession sess;
	LobbyState state;
	NapiMessage in;
	in.name = "ClientUpdateVars";
	auto r = sess.dispatch(in, state, "127.0.0.1", 32768);
	TEST_EXPECT(r.label == "ClientUpdateVars");
	TEST_EXPECT(r.reply_containers.empty());
	return 0;
}

// Rewrite one var of a request's named ClientVarList.
void set_list_var(NapiMessage &request, const std::string &list, const std::string &name,
                  const std::string &value) {
	for (auto &child : request.children) {
		if (field_str(find_field(child, "VarList")) != list) continue;
		for (auto &var : child.children) {
			if (field_str(find_field(var, "VarName")) != name) continue;
			for (auto &f : var.fields)
				if (f.name == "VarValue") f.data.assign(value.begin(), value.end());
		}
	}
}

// The numbers a host sends are read without exceptions (strutil::parse_int /
// parse_ulong; ADR 0049 d5): what std::stoi / stoul refused reads as 0 (an
// endpoint port falls through to the next source), a numeric prefix as its
// number, a VarFNum that is no number as 0.
int test_bad_numbers_are_results() {
	LobbySession sess; // the default GSID generator, whose app field is the AppId
	LobbyState state;
	std::vector<test_novaworld::IndexedVar> players;
	for (const auto &v : player_slot_vars(0, "Host", "10.0.0.5:notaport", "", "1", "0")) players.push_back(v);
	NapiMessage in = make_retail_host_request("notanumber", players);
	set_list_var(in, "Host", "Players", "many");
	set_list_var(in, "HostSetup", "MaxPlayers", "99999999999");
	auto r = sess.dispatch(in, state, "10.0.0.1", 64500);
	TEST_EXPECT(r.reply_containers.size() == 1);
	TEST_EXPECT(state.gsid.rfind("GSID-10-00000000-", 0) == 0);
	TEST_EXPECT(state.player_count == 1); // 0, then the roster's size
	TEST_EXPECT(state.max_players == 0);
	TEST_EXPECT(state.host_ip == "10.0.0.1" && state.host_port == 64500);

	LobbyState prefixed;
	std::vector<test_novaworld::IndexedVar> slot0;
	for (const auto &v : player_slot_vars(0, "Host", "10.0.0.5:32780xyz", "", "1", "0")) slot0.push_back(v);
	NapiMessage request = make_retail_host_request("1234abc", slot0);
	set_list_var(request, "Host", "Players", " 5 players");
	set_list_var(request, "HostSetup", "MaxPlayers", "12/16");
	sess.dispatch(request, prefixed, "10.0.0.1", 64500);
	TEST_EXPECT(prefixed.gsid.rfind("GSID-10-000004d2-", 0) == 0);
	TEST_EXPECT(prefixed.player_count == 5);
	TEST_EXPECT(prefixed.max_players == 12);
	TEST_EXPECT(prefixed.host_ip == "10.0.0.5" && prefixed.host_port == 32780);

	NapiMessage list;
	list.name = "ClientHostUpdate";
	NapiMessage vars = make_client_var_list("Host", {{"ServerName", "x"}});
	vars.children[0].fields[0].data = {'x'}; // VarFNum "x"
	list.children.push_back(vars);
	auto lists = extract_var_lists(list);
	TEST_EXPECT(lists["Host"].size() == 1 && lists["Host"][0].fnum == 0);

	NapiMessage removed;
	removed.name = "ClientHostPlayerRemoved";
	removed.fields.push_back({"PlayerNumber", std::vector<uint8_t>{'?'}});
	auto rr = sess.dispatch(removed, prefixed, "10.0.0.1", 64500);
	TEST_EXPECT(rr.label == "ClientHostPlayerRemoved");
	TEST_EXPECT(prefixed.roster.empty()); // slot "?" reads as 0, the host's own
	return 0;
}

int test_unknown_message_returns_no_reply_with_label() {
	LobbySession sess;
	LobbyState state;
	NapiMessage in;
	in.name = "ClientWeirdThing";
	auto r = sess.dispatch(in, state, "127.0.0.1", 1);
	TEST_EXPECT(r.reply_containers.empty());
	TEST_EXPECT(r.label == "unknown:ClientWeirdThing");
	return 0;
}

} // namespace

int main() {
	if (test_extract_var_lists_keeps_indexed_entries() != 0) return 1;
	if (test_client_connected_returns_server_start_verify() != 0) return 1;
	if (test_client_request_verify_result_returns_server_verify_result() != 0) return 1;
	if (test_retail_host_request_returns_server_host_result_with_gsid() != 0) return 1;
	if (test_host_port_override_when_positive() != 0) return 1;
	if (test_slot0_ip_and_port_selects_game_endpoint() != 0) return 1;
	if (test_two_hosts_with_colliding_app_ids_get_distinct_rids() != 0) return 1;
	if (test_legacy_host_request_extracts_gsb_fields() != 0) return 1;
	if (test_client_host_update_silent_with_state_refresh() != 0) return 1;
	if (test_client_player_enter_request_returns_result() != 0) return 1;
	if (test_stop_hosting_and_stop_playing_are_silent_lifecycle_messages() != 0) return 1;
	if (test_client_play_request_returns_server_play_result() != 0) return 1;
	if (test_glsvss_request_answers_with_results_only_when_configured() != 0) return 1;
	if (test_update_vars_is_accepted_without_reply() != 0) return 1;
	if (test_bad_numbers_are_results() != 0) return 1;
	if (test_unknown_message_returns_no_reply_with_label() != 0) return 1;
	std::printf("OK: LobbySession dispatch (lifecycle, retail Host/PlayerList lists, RID minting, GLSVSS)\n");
	return 0;
}
