// End-to-end: register hosts through the real LobbySession dispatch against
// a real (in-memory) sqlite DB, then read them back through host_repository —
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

#include <net/novaworld/db/sqlite.h>
#include <net/novaworld/gsb.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_session.h>
#include <net/novaworld/lobby_update.h>

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

int main() {
	opennova::db::Database db(":memory:");
	// The real migrations, so the schema (FKs, indexes, cascade) matches
	// production exactly.
	opennova::db::run_migrations(db, OPENNOVA_SOURCE_DIR "/backend/migrations");

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

	// Per-game GSB filter: the JO slug returns both; the DFX2 slug does not.
	TEST_EXPECT(opennova::hostdb::list_hosts_by_game(db, "jop_2_consumer").size() == 2);
	TEST_EXPECT(opennova::hostdb::list_hosts_by_game(db, "dfx2_consumer").empty());

	// The indexed PlayerList lands in host_roster, one row per slot.
	auto roster = opennova::hostdb::list_roster(db, a.rid);
	TEST_EXPECT(roster.size() == 2);
	TEST_EXPECT(roster[0].slot == 0 && roster[0].player_name == "alice" && roster[0].pcid == "00000002");
	TEST_EXPECT(roster[1].slot == 1 && roster[1].player_name == "bob" && roster[1].team == "2");
	TEST_EXPECT(opennova::hostdb::list_roster(db, b.rid).empty());

	// The GSB projection carries the stored columns and the roster names, and
	// the parsed blob's player total counts them (the retail browser adds the
	// row's u16 name count to its total).
	auto entry = opennova::hostdb::gsb_entry_from_host(*row_a, roster);
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
	}

	// Removing the host cascades the roster.
	opennova::hostdb::remove_host_by_rid(db, a.rid);
	TEST_EXPECT(opennova::hostdb::list_roster(db, a.rid).empty());
	TEST_EXPECT(opennova::hostdb::list_hosts(db).size() == 1);

	std::printf("OK: host register -> list round-trip (rid minting, roster, GSB projection, status blob)\n");
	return 0;
}
