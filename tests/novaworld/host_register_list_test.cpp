// End-to-end: register hosts through the real LobbySession dispatch against
// a real (migrated, per-run) database, then read them back through host_repository —
// the exact path /api/lobbies, /api/hosts, and /jop_2.gsb all query.
//
// Guards:
//   * the row must actually land in active_hosts (set_database must reach the
//     lobby session, and upsert_host must run), and list_hosts_by_game must
//     scope the per-game GSB feeds (jop vs dfx2);
//   * two retail hosts whose per-session AppIds collide in their low bits
//     (1000 and 5096) must yield two rows;
//   * the VarFNum-indexed PlayerList lands in host_roster and reaches the GSB
//     row tail together with every host-reported column;
//   * the POST status blob (Lobby_UpdateServerInfo heartbeat) refreshes the
//     row that owns its HostKey.

#include <net/novaworld/db/database.h>
#include <net/novaworld/gsb.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_session.h>
#include <net/novaworld/lobby_update.h>

#include "common/test_database.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "client_var_fixture.h"
#include "common/test_expect.h"

using opennova::LobbySession;
using opennova::LobbyState;
using opennova::NapiMessage;
using test_novaworld::make_client_var_list;
using test_novaworld::make_indexed_var_list;
using test_novaworld::player_slot_vars;

namespace {

NapiMessage make_host_request(const std::string &app_id, const std::string &server_name,
                              const std::string &host_key,
                              const std::vector<test_novaworld::IndexedVar> &players) {
	NapiMessage in;
	in.name = "ClientHostRequest";
	in.children.push_back(make_client_var_list("HostSetup", {
		{"AppId", app_id},
		{"LobbyName", "jop_2_consumer"},
		{"MaxPlayers", "16"},
	}));
	in.children.push_back(make_client_var_list("Host", {
		{"HostKey", host_key},
		{"PCIDKey", "16777216"},
		{"ServerName", server_name},
		{"GameType", "TDM"},
		{"MissionName", "ASH_G11A"},
		{"Players", "2"},
		{"Region", "us"},
		{"TimeLeft", "45"},
		{"TimeOfDay", "Dawn"},
		{"Msg", "welcome"},
		{"Age", "0 00:12:34"},
		{"PBServer", "1"},
		{"Skins", "Y"},
		{"Tracers", "N"},
		{"Port", "-1"},
	}));
	in.children.push_back(make_indexed_var_list("PlayerList", players));
	return in;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	// The real migrations, so the schema (FKs, indexes, cascade) matches
	// production exactly.
	test_db::TestDatabase scratch;
	opennova::db::Database db = scratch.open();

	LobbySession sess;
	sess.set_database(&db);
	sess.set_gsid_generator([](const std::string &app) { return "GSID-TEST-" + app; });

	std::vector<test_novaworld::IndexedVar> players;
	for (const auto &v : player_slot_vars(0, "alice", "10.0.0.7:40000", "00000002", "1", "0")) players.push_back(v);
	for (const auto &v : player_slot_vars(1, "bob",   "10.0.0.8:40000", "00000003", "2", "0")) players.push_back(v);

	LobbyState a;
	auto ra = sess.dispatch(make_host_request("1000", "EndToEnd Srv", "HK-A", players), a, "10.0.0.7", 40000);
	TEST_EXPECT(ra.label == "ClientHostRequest");
	LobbyState b;
	auto rb = sess.dispatch(make_host_request("5096", "Second Srv", "HK-B", {}), b, "10.0.0.8", 40000);
	TEST_EXPECT(rb.label == "ClientHostRequest");

	// Both rows land; AppId 1000 and 5096 (same low twelve bits) never collide.
	auto all = opennova::hostdb::list_hosts(db);
	TEST_EXPECT(all.size() == 2);
	TEST_EXPECT(all[0].rid != all[1].rid);
	TEST_EXPECT(a.rid != 0 && b.rid != 0 && a.rid != b.rid);
	auto row_a = opennova::hostdb::find_host_by_rid(db, a.rid);
	TEST_EXPECT(row_a.has_value());
	TEST_EXPECT(row_a->game == "jop_2_consumer");
	TEST_EXPECT(row_a->server_name == "EndToEnd Srv");
	TEST_EXPECT(row_a->player_count == 2);
	// host_ip/host_port are the observed UDP peer (Port = -1).
	TEST_EXPECT(row_a->host_ip == "10.0.0.7");
	TEST_EXPECT(row_a->host_port == 40000);
	// PCIDKey is ingested from the request, not only the update.
	TEST_EXPECT(row_a->pcid_key == "16777216");
	TEST_EXPECT(row_a->host_key == "HK-A");
	// The host-reported browser columns are stored.
	TEST_EXPECT(row_a->time_left == "45");
	TEST_EXPECT(row_a->time_of_day == "Dawn");
	TEST_EXPECT(row_a->msg == "welcome");
	TEST_EXPECT(row_a->age == "0 00:12:34");
	TEST_EXPECT(row_a->pb_server == "1");
	TEST_EXPECT(row_a->skins == "Y");
	TEST_EXPECT(row_a->tracers == "N");

	// Per-game GSB filter: the JO lobby returns both; the DFX2 lobby does not.
	TEST_EXPECT(opennova::hostdb::list_hosts_by_game(db, "jop_2_consumer").size() == 2);
	TEST_EXPECT(opennova::hostdb::list_hosts_by_game(db, "dfx2_0_consumer").empty());

	// The indexed PlayerList lands in host_roster, one row per slot.
	auto roster = opennova::hostdb::list_roster(db, a.rid);
	TEST_EXPECT(roster.size() == 2);
	TEST_EXPECT(roster[0].slot == 0 && roster[0].player_name == "alice" && roster[0].pcid == "00000002");
	TEST_EXPECT(roster[1].slot == 1 && roster[1].player_name == "bob" && roster[1].team == "2");
	TEST_EXPECT(opennova::hostdb::list_roster(db, b.rid).empty());

	// The GSB projection carries the stored columns and the roster names, and
	// the parsed blob's player total counts them (the retail browser adds the
	// row's u16 name count to its total). The host reported no VER1 or
	// Expbits, so those are its game's (the games row the route passes).
	auto entry = opennova::hostdb::gsb_entry_from_host(*row_a, roster, { "3", "3" });
	TEST_EXPECT(entry.ver1 == "3" && entry.exp_bits == "3");
	{
		const auto dfx2 = opennova::hostdb::gsb_entry_from_host(*row_a, roster, { "1", "1" });
		TEST_EXPECT(dfx2.ver1 == "1" && dfx2.exp_bits == "1");
		auto reported = *row_a;
		reported.ver1 = "7";
		reported.exp_bits = "2";
		const auto own = opennova::hostdb::gsb_entry_from_host(reported, roster, { "1", "1" });
		TEST_EXPECT(own.ver1 == "7" && own.exp_bits == "2");
	}
	TEST_EXPECT(entry.rid == a.rid);
	TEST_EXPECT(entry.ip == "10.0.0.7");
	TEST_EXPECT(entry.time_left == "45");
	TEST_EXPECT(entry.time_of_day == "Dawn");
	TEST_EXPECT(entry.msg == "welcome");
	TEST_EXPECT(entry.age == "0 00:12:34");
	TEST_EXPECT(entry.pb_server == "1");
	TEST_EXPECT(entry.skins == "Y");
	TEST_EXPECT(entry.tracers == "N");
	TEST_EXPECT(entry.player_names.size() == 2);
	TEST_EXPECT(entry.player_names[0] == "alice" && entry.player_names[1] == "bob");
	{
		const auto blob = opennova::gsb_build_response({entry});
		opennova::GsbResponse parsed;
		TEST_EXPECT(opennova::gsb_parse_response(blob.data(), blob.size(), parsed));
		TEST_EXPECT(parsed.total_servers == 1);
		TEST_EXPECT(parsed.total_players == 2);
		TEST_EXPECT(parsed.servers[0].time_left == "45");
		TEST_EXPECT(parsed.servers[0].msg == "welcome");
		TEST_EXPECT(parsed.servers[0].player_names.size() == 2);
	}

	// A player leaves the retail way: ClientHostPlayerRemoved at the disconnect,
	// then a ClientHostUpdate carrying only the changed vars (a rotated PCIDKey,
	// the Players count, bob's slot). The left slot is gone and the rest kept.
	{
		NapiMessage removed;
		removed.name = "ClientHostPlayerRemoved";
		removed.fields.push_back({"PlayerNumber", std::vector<uint8_t>{'0'}});
		auto rr = sess.dispatch(removed, a, "10.0.0.7", 40000);
		TEST_EXPECT(rr.label == "ClientHostPlayerRemoved");
		NapiMessage upd;
		upd.name = "ClientHostUpdate";
		upd.children.push_back(make_client_var_list("Host", {
			{"HostKey", "HK-A"},
			{"PCIDKey", "16777217"},
			{"Players", "1"},
			{"TimeLeft", "30"},
		}));
		std::vector<test_novaworld::IndexedVar> one;
		for (const auto &v : player_slot_vars(1, "bob", "10.0.0.8:40000", "00000003", "1", "0")) one.push_back(v);
		upd.children.push_back(make_indexed_var_list("PlayerList", one));
		auto ru = sess.dispatch(upd, a, "10.0.0.7", 40000);
		TEST_EXPECT(ru.label == "ClientHostUpdate");
		auto row = opennova::hostdb::find_host_by_rid(db, a.rid);
		TEST_EXPECT(row.has_value());
		TEST_EXPECT(row->pcid_key == "16777217");
		TEST_EXPECT(row->player_count == 1);
		TEST_EXPECT(row->time_left == "30");
		auto r2 = opennova::hostdb::list_roster(db, a.rid);
		TEST_EXPECT(r2.size() == 1 && r2[0].player_name == "bob" && r2[0].team == "1");
		TEST_EXPECT(a.last_host_update["PlayerList"].size() == 5); // bob's five vars only
	}

	// The POST status blob refreshes the row owning its HostKey.
	{
		opennova::LobbyStatusBlob blob;
		blob.lobby_name = "jop_2_consumer";
		blob.host_key = "HK-A";
		blob.host_vars = {{"ServerName", "Posted Name"}, {"Players", "4"}, {"TimeLeft", "7"},
		                  {"TimeOfDay", "Night"}, {"PCIDKey", "16777218"}};
		blob.player_names = {"alice", "bob", "carol", "dave"};
		const std::string text = opennova::lobby_update_build(blob);
		opennova::LobbyStatusBlob parsed;
		TEST_EXPECT(opennova::lobby_update_parse(text, parsed));
		TEST_EXPECT(opennova::hostdb::apply_status_blob(db, parsed));
		auto row = opennova::hostdb::find_host_by_rid(db, a.rid);
		TEST_EXPECT(row.has_value());
		TEST_EXPECT(row->server_name == "Posted+Name");   // sanitized on the wire, stored as received
		TEST_EXPECT(row->player_count == 4);
		TEST_EXPECT(row->time_left == "7");
		TEST_EXPECT(row->time_of_day == "Night");
		TEST_EXPECT(row->pcid_key == "16777218");
		TEST_EXPECT(opennova::hostdb::list_roster(db, a.rid).size() == 4);
		// An unknown HostKey is ignored.
		parsed.host_key = "HK-NOBODY";
		TEST_EXPECT(!opennova::hostdb::apply_status_blob(db, parsed));

		// A count that is no number, or out of int's range, keeps the stored one
		// (a result, not a caught throw: strutil::parse_int).
		const int max_before = row->max_players;
		opennova::LobbyStatusBlob bad;
		bad.host_key = "HK-A";
		bad.send_player_names = false;
		bad.host_vars = {{"Players", "lots"}, {"MaxPlayers", "99999999999"}};
		TEST_EXPECT(opennova::hostdb::apply_status_blob(db, bad));
		auto kept = opennova::hostdb::find_host_by_rid(db, a.rid);
		TEST_EXPECT(kept.has_value());
		TEST_EXPECT(kept->player_count == 4);
		TEST_EXPECT(kept->max_players == max_before);
		// A numeric prefix reads as std::stoi read it.
		bad.host_vars = {{"Players", "6 players"}, {"MaxPlayers", " 24"}};
		TEST_EXPECT(opennova::hostdb::apply_status_blob(db, bad));
		kept = opennova::hostdb::find_host_by_rid(db, a.rid);
		TEST_EXPECT(kept.has_value() && kept->player_count == 6 && kept->max_players == 24);
		TEST_EXPECT(opennova::hostdb::list_roster(db, a.rid).size() == 4);

		// The blob's names stay inside the roster's bounds (lobby_session.h): with
		// MaxPlayers 24 a 40-name list keeps its first 25.
		opennova::LobbyStatusBlob crowd;
		crowd.host_key = "HK-A";
		for (int i = 0; i < 40; ++i) crowd.player_names.push_back("p" + std::to_string(i));
		TEST_EXPECT(opennova::hostdb::apply_status_blob(db, crowd));
		const auto capped = opennova::hostdb::list_roster(db, a.rid);
		TEST_EXPECT(capped.size() == 25 && capped.back().slot == 24);
		TEST_EXPECT(capped.back().player_name == "p24");
	}

	// A re-sent ClientHostRequest updates the row in place: the player /NWJoin.dll
	// recorded stays in host_players (a delete-and-reinsert cascaded it away),
	// and the columns and the roster are the new ones.
	{
		opennova::hostdb::PlayerRow joined;
		joined.host_rid = a.rid;
		joined.user_id = db.query("INSERT INTO players (username, password_hash, pcid, nwh, "
								  "nwhandle) VALUES ('resend_joiner', 'x', '0000d200', '1', "
								  "'joiner') RETURNING id;")
								 .front()
								 .as_int(0)
								 .value_or(0);
		joined.nwhandle = "joiner";
		joined.peer_ip = "10.0.0.9";
		TEST_EXPECT(joined.user_id > 0);
		opennova::hostdb::add_player(db, joined);
		TEST_EXPECT(opennova::hostdb::list_players(db, a.rid).size() == 1);

		std::vector<test_novaworld::IndexedVar> resent;
		for (const auto &v : player_slot_vars(0, "carol", "10.0.0.9:40000", "00000004", "1", "0")) resent.push_back(v);
		auto rr = sess.dispatch(make_host_request("1000", "Resent Srv", "HK-A", resent), a, "10.0.0.7", 40000);
		TEST_EXPECT(rr.label == "ClientHostRequest");
		const auto players = opennova::hostdb::list_players(db, a.rid);
		TEST_EXPECT(players.size() == 1 && players[0].nwhandle == "joiner");
		TEST_EXPECT(opennova::hostdb::list_hosts(db).size() == 2);
		const auto row = opennova::hostdb::find_host_by_rid(db, a.rid);
		TEST_EXPECT(row.has_value() && row->server_name == "Resent Srv");
		const auto roster = opennova::hostdb::list_roster(db, a.rid);
		TEST_EXPECT(roster.size() == 1 && roster[0].player_name == "carol");
	}

	// Removing the host cascades the roster and the joined players.
	opennova::hostdb::remove_host_by_rid(db, a.rid);
	TEST_EXPECT(opennova::hostdb::list_roster(db, a.rid).empty());
	TEST_EXPECT(opennova::hostdb::list_players(db, a.rid).empty());
	TEST_EXPECT(opennova::hostdb::list_hosts(db).size() == 1);

	// A DFX2 host's lists: DFXCATEGORY1 after LAN on both, and PBServer always
	// "0" (dfx2-vs-jo-re.md DFX2-7) [orig: @0x4d0e8a..0x4d0eb1, @0x4d0faf..0x4d0fd5,
	// @0x4fc4db (dfx2.exe)]. The var no browser column names is ignored, as
	// any unknown var is; the listing lands under its LobbyName, dfx2_0_consumer,
	// and the status blob carrying the same var still refreshes it.
	{
		NapiMessage in;
		in.name = "ClientHostRequest";
		in.children.push_back(make_client_var_list("HostSetup", {
			{"LobbyName", "dfx2_0_consumer"}, {"ServerName", "DFX2 Srv"}, {"Msg", ""},
			{"MaxPlayers", "100"}, {"Password", "0"}, {"Dedicated", "1"}, {"AppId", "4100"},
			{"AccessCodeList", ""}, {"PLoad", ""}, {"Exp", ""}, {"LAN", "0"},
			{"DFXCATEGORY1", "0"}, {"ReconnectCounter", "0"},
		}));
		in.children.push_back(make_client_var_list("Host", {
			{"LobbyName", "dfx2_0_consumer"}, {"ServerName", "DFX2 Srv"}, {"Msg", ""},
			{"MaxPlayers", "100"}, {"AppId", "4100"}, {"PLoad", ""}, {"Exp", ""}, {"LAN", "0"},
			{"DFXCATEGORY1", "0"}, {"HostKey", "HK-DFX2"}, {"PCIDKey", "16777300"},
			{"Players", "0"}, {"PBServer", "0"}, {"Port", "-1"},
		}));
		in.children.push_back(make_indexed_var_list("PlayerList", {}));
		LobbyState d;
		const auto rd = sess.dispatch(in, d, "10.0.0.20", 40000);
		TEST_EXPECT(rd.label == "ClientHostRequest");
		TEST_EXPECT(d.rid != 0);
		const auto row = opennova::hostdb::find_host_by_rid(db, d.rid);
		TEST_EXPECT(row.has_value());
		TEST_EXPECT(row->game == "dfx2_0_consumer");
		TEST_EXPECT(row->server_name == "DFX2 Srv");
		TEST_EXPECT(row->max_players == 100);
		TEST_EXPECT(row->pb_server == "0");
		const auto dfx2 = opennova::hostdb::list_hosts_by_game(db, "dfx2_0_consumer");
		TEST_EXPECT(dfx2.size() == 1 && dfx2[0].rid == d.rid);
		TEST_EXPECT(opennova::hostdb::list_hosts_by_game(db, "jop_2_consumer").size() == 1);
		const auto entry = opennova::hostdb::gsb_entry_from_host(*row, {}, { "1", "1" });
		TEST_EXPECT(entry.pb_server == "0" && entry.ver1 == "1" && entry.exp_bits == "1");

		opennova::LobbyStatusBlob blob;
		blob.lobby_name = "dfx2_0_consumer";
		blob.host_key = "HK-DFX2";
		blob.host_vars = { { "LobbyName", "dfx2_0_consumer" }, { "ServerName", "DFX2 Posted" },
			{ "LAN", "0" }, { "DFXCATEGORY1", "0" }, { "Players", "3" }, { "PBServer", "0" } };
		blob.send_player_names = false;
		opennova::LobbyStatusBlob parsed;
		TEST_EXPECT(opennova::lobby_update_parse(opennova::lobby_update_build(blob), parsed));
		TEST_EXPECT(opennova::hostdb::apply_status_blob(db, parsed));
		const auto posted = opennova::hostdb::find_host_by_rid(db, d.rid);
		TEST_EXPECT(posted.has_value() && posted->server_name == "DFX2+Posted");
		TEST_EXPECT(posted->player_count == 3 && posted->pb_server == "0");
		opennova::hostdb::remove_host_by_rid(db, d.rid);
	}

	// A joiner's lobby session (service policy): its Cookie list's retail login pins the account
	// it signed in as, which stays when that login ends; its play request names the listing at
	// its PlaySetup's address (the .joi's NK, the listing's host_ip:host_port); leaving it (a
	// play request for another server, its ClientStopPlaying) takes the account's /NWJoin.dll
	// row off that listing's roster, and no one else's. An address no listing has (a LAN
	// server's) plays on no RID.
	{
		using opennova::db::BindValue;
		const auto player = [&db](const char *name, const char *pcid) {
			return db.query("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
							"VALUES ($1, 'x', $2, '1', $1) RETURNING id;",
							{ BindValue(std::string(name)), BindValue(std::string(pcid)) })
					.front()
					.as_int(0)
					.value_or(0);
		};
		const int64_t joiner = player("roster_joiner", "0000d201");
		const int64_t stayer = player("roster_stayer", "0000d202");
		TEST_EXPECT(joiner > 0 && stayer > 0);
		db.exec("INSERT INTO active_user_sessions (user_id, username, session_tag) "
				"VALUES ($1, 'roster_joiner', 'tag-roster-joiner');",
				{ BindValue(joiner) });
		LobbyState east;
		TEST_EXPECT(sess.dispatch(make_host_request("2001", "Roster East", "HK-EAST", {}), east,
				"10.0.0.31", 40000).label == "ClientHostRequest");
		LobbyState west;
		TEST_EXPECT(sess.dispatch(make_host_request("2002", "Roster West", "HK-WEST", {}), west,
				"10.0.0.32", 40000).label == "ClientHostRequest");
		TEST_EXPECT(east.rid != 0 && west.rid != 0 && east.rid != west.rid);
		TEST_EXPECT(opennova::hostdb::rid_at(db, "10.0.0.31", 40000) == east.rid);
		TEST_EXPECT(opennova::hostdb::rid_at(db, "10.0.0.31", 40001) == 0);
		// The joins /NWJoin.dll recorded: the joiner on both, another account on the east.
		const auto joined = [&db](uint32_t rid, int64_t user, const char *handle) {
			opennova::hostdb::PlayerRow p;
			p.host_rid = rid;
			p.user_id = user;
			p.nwhandle = handle;
			p.peer_ip = "10.0.0.40";
			opennova::hostdb::add_player(db, p);
		};
		joined(east.rid, joiner, "roster_joiner");
		joined(west.rid, joiner, "roster_joiner");
		{
			opennova::hostdb::PlayerRow p;
			p.host_rid = east.rid;
			p.user_id = stayer;
			p.nwhandle = "roster_stayer";
			p.peer_ip = "10.0.0.41";
			opennova::hostdb::add_player(db, p);
		}
		const auto roster = [&db](uint32_t rid) {
			std::vector<std::string> names;
			for (const auto &p : opennova::hostdb::list_players(db, rid)) names.push_back(p.nwhandle);
			return names;
		};
		const auto play = [](const std::string &ip, const std::string &port) {
			NapiMessage in;
			in.name = "ClientPlayRequest";
			in.children.push_back(make_client_var_list("Cookie", {{"LOGINSESSIONTAG", "tag-roster-joiner"}}));
			in.children.push_back(make_client_var_list("PlaySetup", {
				{"ServerName", "Roster"}, {"IpAddress", ip}, {"PortNumber", port}, {"AppId", "20"}, {"Lan", "0"},
			}));
			return in;
		};

		LobbyState c;
		NapiMessage verify;
		verify.name = "ClientRequestVerifyResult";
		verify.children.push_back(make_client_var_list("Cookie", {{"LOGINSESSIONTAG", "tag-roster-joiner"}}));
		sess.dispatch(verify, c, "10.0.0.40", 32768);
		TEST_EXPECT(c.user_id == joiner);
		TEST_EXPECT(c.playing_since_ms == 0 && c.playing_rid == 0 && c.play_addr.empty());
		// The login ends (a logout, the idle sweep): the account stays the one it signed in as.
		db.exec("DELETE FROM active_user_sessions WHERE user_id = $1;", { BindValue(joiner) });
		TEST_EXPECT(sess.dispatch(play("10.0.0.31", "40000"), c, "10.0.0.40", 32768).label ==
				"ClientPlayRequest");
		TEST_EXPECT(c.user_id == joiner);
		TEST_EXPECT(c.playing_rid == east.rid && c.play_addr == "10.0.0.31:40000" &&
				c.playing_since_ms > 0);
		const int64_t since = c.playing_since_ms;
		// A reconnect's replay of the same request keeps when it began, and both rosters.
		sess.dispatch(play("10.0.0.31", "40000"), c, "10.0.0.40", 32768);
		TEST_EXPECT(c.playing_rid == east.rid && c.playing_since_ms == since);
		TEST_EXPECT(roster(east.rid).size() == 2 && roster(west.rid).size() == 1);
		// It plays on the west server: off the east one's roster, the other account still on it.
		sess.dispatch(play("10.0.0.32", "40000"), c, "10.0.0.40", 32768);
		TEST_EXPECT(c.playing_rid == west.rid && c.play_addr == "10.0.0.32:40000");
		TEST_EXPECT(roster(east.rid) == std::vector<std::string>{ "roster_stayer" });
		TEST_EXPECT(roster(west.rid) == std::vector<std::string>{ "roster_joiner" });
		// Its ClientStopPlaying: off the west one's too, and it plays on nothing.
		NapiMessage stop;
		stop.name = "ClientStopPlaying";
		TEST_EXPECT(sess.dispatch(stop, c, "10.0.0.40", 32768).label == "ClientStopPlaying");
		TEST_EXPECT(roster(west.rid).empty() && roster(east.rid).size() == 1);
		TEST_EXPECT(c.playing_rid == 0 && c.play_addr.empty() && c.playing_since_ms == 0);
		// A server no listing names: it plays at the address, on no RID.
		sess.dispatch(play("192.168.1.20", "32768"), c, "10.0.0.40", 32768);
		TEST_EXPECT(c.playing_rid == 0 && c.play_addr == "192.168.1.20:32768" &&
				c.playing_since_ms > 0);
		// Another account's live login named by a later tag moves the pin; one that names no
		// live login leaves it.
		db.exec("INSERT INTO active_user_sessions (user_id, username, session_tag) "
				"VALUES ($1, 'roster_stayer', 'tag-roster-stayer');",
				{ BindValue(stayer) });
		verify.children.clear();
		verify.children.push_back(make_client_var_list("Cookie", {{"LOGINSESSIONTAG", "tag-roster-stale"}}));
		sess.dispatch(verify, c, "10.0.0.40", 32768);
		TEST_EXPECT(c.user_id == joiner);
		verify.children.clear();
		verify.children.push_back(make_client_var_list("Cookie", {{"LOGINSESSIONTAG", "tag-roster-stayer"}}));
		sess.dispatch(verify, c, "10.0.0.40", 32768);
		TEST_EXPECT(c.user_id == stayer);
		opennova::hostdb::remove_host_by_rid(db, east.rid);
		opennova::hostdb::remove_host_by_rid(db, west.rid);
	}

	std::printf("OK: host register -> list round-trip (rid minting, roster, GSB projection, status blob, re-send, DFX2 lists, a joiner's account and play)\n");
	return 0;
}
