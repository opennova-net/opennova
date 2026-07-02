// Phase D — LobbySession dispatch tests.
//
// Validates the five lobby message handlers ported from
// onnet/onnw/novaworldudp.py against the same input/output expectations.
// Uses NapiMessage directly (= onnet's Container) so we exercise the same
// wire structure the standalone server's nw_udp_listener will see after
// decode_protocol_packet.

#include <novaworld/lobby_session.h>

#include "../common/test_expect.h"

#include <cstdio>
#include <string>
#include <vector>

using opennova::extract_var_lists;
using opennova::LobbyDispatchResult;
using opennova::LobbySession;
using opennova::LobbyState;
using opennova::NapiField;
using opennova::NapiMessage;

namespace {

// Helpers to build the nested ClientVarList structures incoming messages
// carry.

NapiMessage make_client_var(const std::string &name, const std::string &value) {
	NapiMessage v;
	v.name = "ClientVar";
	v.fields.push_back({"VarName",  std::vector<uint8_t>(name.begin(),  name.end())});
	v.fields.push_back({"VarValue", std::vector<uint8_t>(value.begin(), value.end())});
	return v;
}

NapiMessage make_client_var_list(const std::string &list_name,
                                 const std::vector<std::pair<std::string, std::string>> &entries) {
	NapiMessage l;
	l.name = "ClientVarList";
	l.fields.push_back({"VarList", std::vector<uint8_t>(list_name.begin(), list_name.end())});
	for (const auto &[k, v] : entries) {
		l.children.push_back(make_client_var(k, v));
	}
	return l;
}

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

int test_extract_var_lists() {
	NapiMessage outer;
	outer.name = "ClientHostRequest";
	outer.children.push_back(make_client_var_list("HostSetup", {
		{"AppId", "1234"},
		{"LobbyName", "jop_2_consumer"},
		{"MaxPlayers", "16"},
	}));
	outer.children.push_back(make_client_var_list("Host", {
		{"ServerName", "MyServer"},
		{"Players", "3"},
		{"Region", "us"},
	}));
	auto vl = extract_var_lists(outer);
	TEST_EXPECT(vl.size() == 2);
	TEST_EXPECT(vl["HostSetup"]["AppId"] == "1234");
	TEST_EXPECT(vl["HostSetup"]["LobbyName"] == "jop_2_consumer");
	TEST_EXPECT(vl["Host"]["ServerName"] == "MyServer");
	TEST_EXPECT(vl["Host"]["Players"] == "3");
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

int test_client_host_request_returns_server_host_result_with_gsid() {
	LobbySession sess;
	sess.set_gsid_generator([](const std::string &app) {
		return std::string("GSID-10-") + app + "-FIXED";
	});
	sess.set_rid_generator([](const std::string &) { return uint32_t{0x0A001234u}; });

	LobbyState state;
	NapiMessage in;
	in.name = "ClientHostRequest";
	in.children.push_back(make_client_var_list("HostSetup", {
		{"AppId", "1234"},
		{"LobbyName", "jop_2_consumer"},
		{"MaxPlayers", "16"},
	}));
	in.children.push_back(make_client_var_list("Host", {
		{"ServerName", "MyServer"},
		{"ServerIP", "192.168.1.42"},
		{"ServerPortNumber", "17475"},
		{"Players", "3"},
		{"Region", "us"},
	}));

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
	TEST_EXPECT(state.host_ip == "192.168.1.42");
	TEST_EXPECT(state.host_port == 17475);
	TEST_EXPECT(state.server_name == "MyServer");
	TEST_EXPECT(state.player_count == 3);
	TEST_EXPECT(state.max_players == 16);
	TEST_EXPECT(state.region == "us");
	return 0;
}

int test_client_host_request_falls_back_to_remote_addr_for_host_ip() {
	LobbySession sess;
	sess.set_gsid_generator([](const std::string &) { return std::string("FIXED"); });
	sess.set_rid_generator([](const std::string &) { return uint32_t{0x0A000001u}; });
	LobbyState state;
	NapiMessage in;
	in.name = "ClientHostRequest";
	in.children.push_back(make_client_var_list("HostSetup", {
		{"AppId", "999"},
		{"LobbyName", "jop_2_consumer"},
	}));
	in.children.push_back(make_client_var_list("Host", {
		// ServerIP/ServerPortNumber omitted.
	}));
	auto r = sess.dispatch(in, state, "10.0.0.1", 64500);
	TEST_EXPECT(r.reply_containers.size() == 1);
	TEST_EXPECT(state.host_ip == "10.0.0.1");
	TEST_EXPECT(state.host_port == 64500);
	return 0;
}

int test_client_host_request_extracts_gsb_fields() {
	LobbySession sess;
	sess.set_gsid_generator([](const std::string &) { return std::string("FIXED"); });
	sess.set_rid_generator([](const std::string &) { return uint32_t{0x0A000002u}; });
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
	state.host_port = 0;
	NapiMessage in;
	in.name = "ClientHostUpdate";
	in.children.push_back(make_client_var_list("Host", {
		{"HostKey", "ABC123"},
		{"PCIDKey", "PC456"},
		{"ServerIP", "10.10.10.10"},
		{"ServerPortNumber", "17500"},
		{"ServerName", "Renamed"},
		{"Players", "5"},
		{"MaxPlayers", "32"},
		{"Region", "eu"},
	}));
	auto r = sess.dispatch(in, state, "1.2.3.4", 99);
	TEST_EXPECT(r.label == "ClientHostUpdate");
	TEST_EXPECT(r.reply_containers.empty()); // silent
	TEST_EXPECT(state.host_key == "ABC123");
	TEST_EXPECT(state.pcid_key == "PC456");
	TEST_EXPECT(state.host_ip == "10.10.10.10");
	TEST_EXPECT(state.host_port == 17500);
	TEST_EXPECT(state.server_name == "Renamed");
	TEST_EXPECT(state.player_count == 5);
	TEST_EXPECT(state.max_players == 32);
	TEST_EXPECT(state.region == "eu");
	TEST_EXPECT(state.last_host_update["Host"]["HostKey"] == "ABC123");
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

	state.play_state["PlaySetup"]["GSID"] = "fixed";
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
	TEST_EXPECT(state.play_state["PlaySetup"]["GSID"] == "fixed-gsid");
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
	if (test_extract_var_lists() != 0) return 1;
	if (test_client_connected_returns_server_start_verify() != 0) return 1;
	if (test_client_request_verify_result_returns_server_verify_result() != 0) return 1;
	if (test_client_host_request_returns_server_host_result_with_gsid() != 0) return 1;
	if (test_client_host_request_falls_back_to_remote_addr_for_host_ip() != 0) return 1;
	if (test_client_host_request_extracts_gsb_fields() != 0) return 1;
	if (test_client_host_update_silent_with_state_refresh() != 0) return 1;
	if (test_client_player_enter_request_returns_result() != 0) return 1;
	if (test_stop_hosting_and_stop_playing_are_silent_lifecycle_messages() != 0) return 1;
	if (test_client_play_request_returns_server_play_result() != 0) return 1;
	if (test_unknown_message_returns_no_reply_with_label() != 0) return 1;
	std::printf("OK: LobbySession dispatch (lifecycle, GSB fields, extract_var_lists)\n");
	return 0;
}
