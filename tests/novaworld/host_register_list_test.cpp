// End-to-end: register a host through the real LobbySession dispatch against
// a real (in-memory) sqlite DB, then read it back through host_repository —
// the exact path /api/lobbies, /api/hosts, and /jop_2.gsb all query.
//
// Guards two regressions that shipped silently because nothing exercised
// register -> read-back with a live DB:
//   * the row must actually land in active_hosts (set_database must reach the
//     lobby session, and upsert_host must run), and
//   * list_hosts_by_game must scope the per-game GSB feeds (jop vs dfx2).

#include <novaworld/db/sqlite.h>
#include <novaworld/host_repository.h>
#include <novaworld/lobby_session.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "common/test_expect.h"

using opennova::LobbySession;
using opennova::LobbyState;
using opennova::NapiMessage;

namespace {

// Apply the real migrations so the schema (FKs, indexes, cascade) matches
// production exactly — same approach as host_prune_test.
void apply_migrations(opennova::db::Database &db) {
	const std::string dir = std::string(OPENNOVA_SOURCE_DIR) + "/backend/migrations";
	for (const char *f : {"0001_initial.sql", "0002_active_hosts.sql",
	                      "0003_novaworld_status_and_gsb.sql",
	                      "0004_unknown_messages.sql"}) {
		std::ifstream in(dir + "/" + f, std::ios::binary);
		std::ostringstream os;
		os << in.rdbuf();
		db.exec_script(os.str());
	}
}

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
	for (const auto &[k, v] : entries) l.children.push_back(make_client_var(k, v));
	return l;
}

} // namespace

int main() {
	opennova::db::Database db(":memory:");
	apply_migrations(db);

	LobbySession sess;
	sess.set_database(&db);
	sess.set_gsid_generator([](const std::string &) { return std::string("GSID-TEST"); });
	sess.set_rid_generator([](const std::string &) { return uint32_t{0x0A001234}; });

	LobbyState state;
	NapiMessage in;
	in.name = "ClientHostRequest";
	in.children.push_back(make_client_var_list("HostSetup", {
		{"AppId", "1234"},
		{"LobbyName", "jop_2_consumer"},
		{"MaxPlayers", "16"},
	}));
	in.children.push_back(make_client_var_list("Host", {
		{"ServerName", "EndToEnd Srv"},
		{"Players", "2"},
		{"Region", "us"},
	}));

	auto r = sess.dispatch(in, state, "10.0.0.7", 40000);
	TEST_EXPECT(r.label == "ClientHostRequest");

	// The row must be readable back through host_repository — without
	// set_database reaching the lobby session, dispatch still returns a reply
	// but writes nothing, and every browse endpoint stays empty.
	auto all = opennova::hostdb::list_hosts(db);
	TEST_EXPECT(all.size() == 1);
	TEST_EXPECT(all[0].game == "jop_2_consumer");
	TEST_EXPECT(all[0].server_name == "EndToEnd Srv");
	TEST_EXPECT(all[0].player_count == 2);
	TEST_EXPECT(all[0].rid == 0x0A001234u);
	// host_ip/host_port fall back to the UDP peer when the client omits them.
	TEST_EXPECT(all[0].host_ip == "10.0.0.7");
	TEST_EXPECT(all[0].host_port == 40000);

	// Per-game GSB filter: the JO slug returns it; the DFX2 slug does not.
	TEST_EXPECT(opennova::hostdb::list_hosts_by_game(db, "jop_2_consumer").size() == 1);
	TEST_EXPECT(opennova::hostdb::list_hosts_by_game(db, "dfx2_consumer").empty());

	std::printf("OK: host register -> list round-trip\n");
	return 0;
}
