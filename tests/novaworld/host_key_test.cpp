// The listing's owner, below the HTTP layer (host_keys): the
// hosting-key store in host_repository and the lobby session's use of it, on a
// migrated database per case. Runs on every service build (the HTTP layer's halves,
// /NWHost.dll recording the key and the owner routes, are the route harness's;
// the whole flow over the wire is nw_host_ownership_e2e's).
//
//   * the store keeps only a key's SHA-256, never the key, and records only a
//     48-letter A..P key, for an active account still at the credentials
//     generation its login read; an account keeps its newest kHostKeysPerUser keys and
//     every key a listed server holds;
//   * a claim names the account for a live key of an active account; a key
//     backs one listed RID at a time, a gone listing frees it, an idle or
//     revoked key and a banned account claim nothing, a boot's clear_all
//     releases every claim, the sweep keeps a listed server's key, and a
//     claim releases the account's other keys on its RID (a connection that
//     stops and hosts again under its RID with a fresh key stays owned);
//   * listing_owned_by (the owner's push routes) holds only for a listed RID
//     with one of the account's keys claimed for it, read live: a row naming
//     the account with no claim, or after its keys are revoked, is not its;
//   * a HostKey links the row where the connection presents it: a stock host's
//     fresh ClientHostRequest carries none (listed with no owner) and its first
//     ClientHostUpdate carrying one links it; a request carrying one (a
//     re-host's kept Host list) links at once; the updates and the roster
//     statements keep the link, a parked connection's re-host keeps it, another
//     connection presenting the key gets none, and a deleted owner leaves the
//     row refreshing with no owner;
//   * a row names its owner only while the owner's key stands claimed for its
//     RID: a revoked key drops the owner at the next write, and a deleted
//     account's listing names no one, whatever id the lobby still holds;
//   * ONNET_REQUIRE_HOST_KEY refuses a request carrying a key that names no
//     account with ServerHostResult Success=0 / MsgCode 200 (NWEC58), holds a
//     keyless request (a listed connection's too, its row withdrawn) out of the
//     browser until an update's key names its owner, and ends a held host whose
//     key names none with ServerStopHosting (NWUSERVERMSGCODE_NEEDTOLOGIN).

#include <base/io/sha256.h>
#include <net/napi/session.h>
#include <net/novaworld/db/database.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_session.h>

#include "client_var_fixture.h"
#include "common/test_database.h"
#include "common/test_expect.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace hostdb = opennova::hostdb;
using opennova::LobbySession;
using opennova::LobbyState;
using opennova::NapiMessage;
using opennova::db::BindValue;
using opennova::db::Database;
using test_novaworld::make_client_var_list;

namespace {

// A HOSTKEY of one repeated letter (each a distinct key).
std::string key_of(char letter) { return std::string(hostdb::kHostKeyChars, letter); }

int64_t make_player(Database &db, const std::string &name) {
	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
	        "VALUES ($1, 'x', $2, '1', $3);",
	        {BindValue(name), BindValue("pc-" + name), BindValue(name)});
	return db.query("SELECT id FROM players WHERE username = $1;", {BindValue(name)})
	        .front()
	        .as_int(0)
	        .value();
}

// A listed RID, the way the lobby's upsert leaves one.
void list_rid(Database &db, uint32_t rid) {
	hostdb::HostRow row;
	row.rid = rid;
	row.gsid = "GSID-" + std::to_string(rid);
	row.game = "jop_2_consumer";
	row.server_name = "Listed " + std::to_string(rid);
	row.host_ip = "10.0.0.1";
	row.peer_ip = "10.0.0.1";
	hostdb::upsert_host(db, row);
}

void age_key(Database &db, const std::string &key, const char *modifier) {
	db.exec("UPDATE host_keys SET last_used_at = now() + $1::interval WHERE key_hash = $2;",
	        {BindValue(std::string(modifier)), BindValue(hostdb::host_key_hash(key))});
}

std::optional<int64_t> claimed_rid(Database &db, const std::string &key) {
	const auto rows = db.query("SELECT claimed_rid FROM host_keys WHERE key_hash = $1;",
	                           {BindValue(hostdb::host_key_hash(key))});
	if (rows.empty()) return std::nullopt;
	return rows.front().as_int(0);
}

bool key_stored(Database &db, const std::string &key) {
	return !db.query("SELECT 1 FROM host_keys WHERE key_hash = $1;",
	                 {BindValue(hostdb::host_key_hash(key))})
	                .empty();
}

std::string field(const NapiMessage &msg, const char *name) {
	for (const auto &f : msg.fields) {
		if (f.name == name) return opennova::field_to_string(f);
	}
	return "<absent>";
}

// A ClientHostRequest whose Host list carries `host_key`: a re-host's kept
// list (or a client that sends the key at once).
NapiMessage host_request(const std::string &server_name, const std::string &host_key,
                         const char *currently_hosting = "0") {
	NapiMessage in;
	in.name = "ClientHostRequest";
	const std::string hosting(currently_hosting);
	in.fields.push_back({"CurrentlyHosting", std::vector<uint8_t>(hosting.begin(), hosting.end())});
	in.children.push_back(make_client_var_list("HostSetup", {
		{"AppId", "4321"},
		{"LobbyName", "jop_2_consumer"},
		{"MaxPlayers", "16"},
	}));
	in.children.push_back(make_client_var_list("Host", {
		{"HostKey", host_key},
		{"PCIDKey", "16777216"},
		{"ServerName", server_name},
		{"MissionName", "ASH_G11A"},
		{"Players", "0"},
		{"Port", "-1"},
	}));
	return in;
}

// A stock host's fresh ClientHostRequest: the Host list BuildHostVarLists
// rebuilds, with no HostKey [orig: CNapiGameSession_BuildHostVarLists
// @0x4d0b50, the +532 run].
NapiMessage fresh_request(const std::string &server_name) {
	NapiMessage in;
	in.name = "ClientHostRequest";
	in.fields.push_back({"CurrentlyHosting", {'0'}});
	in.children.push_back(make_client_var_list("HostSetup", {
		{"LobbyName", "jop_2_consumer"},
		{"ServerName", server_name},
		{"MaxPlayers", "16"},
		{"AppId", "4321"},
	}));
	in.children.push_back(make_client_var_list("Host", {
		{"LobbyName", "jop_2_consumer"},
		{"ServerName", server_name},
		{"MaxPlayers", "16"},
		{"AppId", "4321"},
	}));
	return in;
}

NapiMessage host_update(const std::string &server_name) {
	NapiMessage in;
	in.name = "ClientHostUpdate";
	in.children.push_back(make_client_var_list("Host", {{"ServerName", server_name}}));
	return in;
}

// The update that first carries the HostKey (Lobby_UpdateServerInfo's, at the
// mission's start).
NapiMessage key_update(const std::string &host_key) {
	NapiMessage in;
	in.name = "ClientHostUpdate";
	in.children.push_back(make_client_var_list("Host", {
		{"HostKey", host_key},
		{"MissionName", "ASH_G11A"},
		{"Players", "1"},
	}));
	return in;
}

NapiMessage player_added(int slot, const std::string &name) {
	NapiMessage in;
	in.name = "ClientHostPlayerAdded";
	auto add = [&in](const char *field_name, const std::string &value) {
		in.fields.push_back({field_name, std::vector<uint8_t>(value.begin(), value.end())});
	};
	add("PlayerNumber", std::to_string(slot));
	add("PlayerName", name);
	add("PlayerIpAndPort", "10.0.0.9:40000");
	add("PlayerPCID", "00000009");
	add("PlayerTeam", "1");
	add("PlayerType", "0");
	return in;
}

std::optional<int64_t> row_owner(Database &db, uint32_t rid) {
	const auto row = hostdb::find_host_by_rid(db, rid);
	return row ? row->host_user_id : std::nullopt;
}

// ServerHostResult Success=0, MsgCode 200, no Rid.
bool refused_unknown_key(const opennova::LobbyDispatchResult &r) {
	return r.label == "ClientHostRequest:unknown-host-key" && r.reply_containers.size() == 1 &&
	       r.reply_containers[0].name == "ServerHostResult" &&
	       field(r.reply_containers[0], "Success") == "0" &&
	       field(r.reply_containers[0], "MsgCode") ==
	               std::to_string(opennova::SERVER_MSG_CODE_NEED_TO_LOGIN) &&
	       field(r.reply_containers[0], "Rid") == "<absent>" && r.reply_containers[0].children.empty();
}

// ServerStopHosting MsgCode 200 in answer to an update.
bool stopped_unknown_key(const opennova::LobbyDispatchResult &r) {
	return r.label == "ClientHostUpdate:unknown-host-key" && r.reply_containers.size() == 1 &&
	       r.reply_containers[0].name == "ServerStopHosting" &&
	       field(r.reply_containers[0], "MsgCode") ==
	               std::to_string(opennova::SERVER_MSG_CODE_NEED_TO_LOGIN);
}

bool answered_hosting(const opennova::LobbyDispatchResult &r) {
	return r.label == "ClientHostRequest" && r.reply_containers.size() == 1 &&
	       field(r.reply_containers[0], "Success") == "1";
}

// The key's form and its stored hash.
int test_key_form_and_hash() {
	TEST_EXPECT(hostdb::is_host_key(key_of('A')));
	TEST_EXPECT(hostdb::is_host_key("ABCDEFGHIJKLMNOPABCDEFGHIJKLMNOPABCDEFGHIJKLMNOP"));
	TEST_EXPECT(!hostdb::is_host_key(std::string(47, 'A')));
	TEST_EXPECT(!hostdb::is_host_key(std::string(49, 'A')));
	TEST_EXPECT(!hostdb::is_host_key(std::string(48, 'Q')));
	TEST_EXPECT(!hostdb::is_host_key(std::string(48, 'a')));
	TEST_EXPECT(!hostdb::is_host_key(""));
	const std::string hash = hostdb::host_key_hash(key_of('A'));
	TEST_EXPECT(hash == opennova::io::sha256_hex(opennova::io::sha256(key_of('A'))));
	TEST_EXPECT(hash.size() == 64);
	return 0;
}

// record_host_key keeps the hash alone, records no non-key, and trims the
// account's keys to the newest kHostKeysPerUser, sparing a listed server's.
int test_record() {
	test_db::TestDatabase scratch;
	opennova::db::Database db = scratch.open();
	const int64_t alice = make_player(db, "alice");
	const int64_t bob = make_player(db, "bob");

	hostdb::record_host_key(db, key_of('A'), alice, 0);
	const auto rows = db.query("SELECT key_hash, user_id, claimed_rid FROM host_keys;");
	TEST_EXPECT(rows.size() == 1);
	TEST_EXPECT(rows[0].as_text(0).value() == hostdb::host_key_hash(key_of('A')));
	TEST_EXPECT(rows[0].as_int(1).value() == alice);
	TEST_EXPECT(!rows[0].as_int(2).has_value());
	// The key itself is stored nowhere.
	TEST_EXPECT(db.query("SELECT 1 FROM host_keys WHERE key_hash = $1;", {BindValue(key_of('A'))})
	                    .empty());

	TEST_EXPECT(!hostdb::record_host_key(db, "HK-NOT-A-KEY", alice, 0));
	TEST_EXPECT(!hostdb::record_host_key(db, std::string(48, 'a'), alice, 0));
	TEST_EXPECT(db.query("SELECT COUNT(*) FROM host_keys;").front().as_int(0).value() == 1);

	// Only at the account's credentials generation, and only for an active
	// account: a login whose generation a reset has since moved records
	// nothing (the race between /NWHost.dll's lookup and this insert), and
	// neither does a banned account; nor does an account that is gone.
	db.exec("UPDATE players SET credentials_gen = credentials_gen + 1 WHERE id = $1;",
			{ BindValue(alice) });
	TEST_EXPECT(!hostdb::record_host_key(db, key_of('O'), alice, 0));
	TEST_EXPECT(!key_stored(db, key_of('O')));
	TEST_EXPECT(hostdb::record_host_key(db, key_of('O'), alice, 1));
	TEST_EXPECT(key_stored(db, key_of('O')));
	db.exec("DELETE FROM host_keys WHERE key_hash = $1;",
			{ BindValue(hostdb::host_key_hash(key_of('O'))) });
	db.exec("UPDATE players SET credentials_gen = 0, account_status = 'banned' WHERE id = $1;",
			{ BindValue(alice) });
	TEST_EXPECT(!hostdb::record_host_key(db, key_of('O'), alice, 0));
	db.exec("UPDATE players SET account_status = 'active' WHERE id = $1;", { BindValue(alice) });
	TEST_EXPECT(!hostdb::record_host_key(db, key_of('O'), 9999, 0));
	TEST_EXPECT(db.query("SELECT COUNT(*) FROM host_keys;").front().as_int(0).value() == 1);

	// The cap: alice's 'A' key backs a listed server, then 24 more keys.
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('A'), 0x0A000001u) == alice);
	list_rid(db, 0x0A000001u);
	std::vector<std::string> later;
	for (int i = 0; i < 24; ++i) {
		std::string key = key_of('B');
		key[0] = static_cast<char>('A' + i % 16);
		key[1] = static_cast<char>('A' + i / 16);
		later.push_back(key);
		hostdb::record_host_key(db, key, alice, 0);
	}
	hostdb::record_host_key(db, key_of('P'), bob, 0);
	const auto counts = db.query("SELECT user_id, COUNT(*) FROM host_keys GROUP BY user_id "
	                             "ORDER BY user_id;");
	TEST_EXPECT(counts.size() == 2);
	TEST_EXPECT(counts[0].as_int(1).value() ==
	            static_cast<int64_t>(hostdb::kHostKeysPerUser) + 1);
	TEST_EXPECT(counts[1].as_int(1).value() == 1);
	TEST_EXPECT(key_stored(db, key_of('A')));       // the listed server's
	TEST_EXPECT(!key_stored(db, later[0]));          // the oldest unclaimed went
	TEST_EXPECT(!key_stored(db, later[3]));
	TEST_EXPECT(key_stored(db, later[4]));
	TEST_EXPECT(key_stored(db, later.back()));
	return 0;
}

// claim_host_key: the account for a live key of an active account; one listed
// RID at a time; idle, unknown and banned claim nothing; clear_all releases.
int test_claim() {
	test_db::TestDatabase scratch;
	opennova::db::Database db = scratch.open();
	const int64_t alice = make_player(db, "alice");
	hostdb::record_host_key(db, key_of('A'), alice, 0);

	TEST_EXPECT(!hostdb::claim_host_key(db, key_of('B'), 0x0A000001u));  // unknown
	TEST_EXPECT(!hostdb::claim_host_key(db, "", 0x0A000001u));
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('A'), 0x0A000001u) == alice);
	TEST_EXPECT(claimed_rid(db, key_of('A')) == 0x0A000001);
	// While RID 1 is listed, the key is RID 1's alone.
	list_rid(db, 0x0A000001u);
	TEST_EXPECT(!hostdb::claim_host_key(db, key_of('A'), 0x0A000002u));
	TEST_EXPECT(claimed_rid(db, key_of('A')) == 0x0A000001);
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('A'), 0x0A000001u) == alice);
	// Its listing gone, another RID may claim it.
	hostdb::remove_host_by_rid(db, 0x0A000001u);
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('A'), 0x0A000002u) == alice);
	TEST_EXPECT(claimed_rid(db, key_of('A')) == 0x0A000002);

	// A boot's clear_all releases every claim with the rows.
	list_rid(db, 0x0A000002u);
	hostdb::clear_all(db);
	TEST_EXPECT(!claimed_rid(db, key_of('A')).has_value());
	TEST_EXPECT(key_stored(db, key_of('A')));
	list_rid(db, 0x0A000002u);  // this run's RID 2, another listing
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('A'), 0x0A000003u) == alice);
	hostdb::clear_all(db);

	// A banned account's key claims nothing; lifted, it does again.
	db.exec("UPDATE players SET account_status = 'banned' WHERE id = $1;", {BindValue(alice)});
	TEST_EXPECT(!hostdb::claim_host_key(db, key_of('A'), 0x0A000004u));
	db.exec("UPDATE players SET account_status = 'active' WHERE id = $1;", {BindValue(alice)});
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('A'), 0x0A000004u) == alice);

	// Idle past kHostKeyIdleSeconds: nothing, even before the sweep deletes it.
	age_key(db, key_of('A'), "-86401 seconds");
	TEST_EXPECT(!hostdb::claim_host_key(db, key_of('A'), 0x0A000004u));
	// A claim moves last_used_at forward.
	hostdb::record_host_key(db, key_of('C'), alice, 0);
	age_key(db, key_of('C'), "-86000 seconds");
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('C'), 0x0A000005u) == alice);
	TEST_EXPECT(!db.query("SELECT 1 FROM host_keys WHERE key_hash = $1 "
	                      "AND last_used_at > (now() + interval '-60 seconds');",
	                      {BindValue(hostdb::host_key_hash(key_of('C')))})
	                     .empty());

	// A deleted account takes its keys.
	db.exec("DELETE FROM players WHERE id = $1;", {BindValue(alice)});
	TEST_EXPECT(db.query("SELECT 1 FROM host_keys;").empty());
	return 0;
}

// What the owner's push routes ask (listing_owned_by): the RID is listed and one
// of the account's keys stands claimed for it, read live, so a row that names
// the account without a claim answers false, and so does a claim once its keys
// are revoked, before the row's next write drops the owner.
int test_listing_owned_by() {
	test_db::TestDatabase scratch;
	opennova::db::Database db = scratch.open();
	const int64_t alice = make_player(db, "alice");
	const int64_t bob = make_player(db, "bob");
	constexpr uint32_t kRid = 0x0A000011u;
	hostdb::record_host_key(db, key_of('A'), alice, 0);

	// A claim with no listing, then a listing with no claim: neither.
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('A'), kRid) == alice);
	TEST_EXPECT(!hostdb::listing_owned_by(db, kRid, alice));
	hostdb::remove_host_by_rid(db, kRid);
	db.exec("UPDATE host_keys SET claimed_rid = NULL;");
	list_rid(db, kRid);
	db.exec("UPDATE active_hosts SET host_user_id = $1 WHERE rid = $2;",
	        {BindValue(alice), BindValue(static_cast<int64_t>(kRid))});
	TEST_EXPECT(!hostdb::listing_owned_by(db, kRid, alice));

	// Listed and claimed: Alice's, nobody else's, and no other RID.
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('A'), kRid) == alice);
	TEST_EXPECT(hostdb::listing_owned_by(db, kRid, alice));
	TEST_EXPECT(!hostdb::listing_owned_by(db, kRid, bob));
	TEST_EXPECT(!hostdb::listing_owned_by(db, kRid + 1, alice));
	TEST_EXPECT(!hostdb::listing_owned_by(db, 0, alice));

	// The keys revoked (an admin reset deletes them): not hers at once, though
	// the row still names her until its next write.
	db.exec("DELETE FROM host_keys WHERE user_id = $1;", {BindValue(alice)});
	TEST_EXPECT(row_owner(db, kRid) == alice);
	TEST_EXPECT(!hostdb::listing_owned_by(db, kRid, alice));

	// Claimed again, then the listing gone: not hers.
	hostdb::record_host_key(db, key_of('B'), alice, 0);
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('B'), kRid) == alice);
	TEST_EXPECT(hostdb::listing_owned_by(db, kRid, alice));
	hostdb::remove_host_by_rid(db, kRid);
	TEST_EXPECT(!hostdb::listing_owned_by(db, kRid, alice));
	return 0;
}

// The sweep keeps a listed server's key however long ago it was claimed, and
// deletes every other key idle for a day.
int test_prune() {
	test_db::TestDatabase scratch;
	opennova::db::Database db = scratch.open();
	const int64_t alice = make_player(db, "alice");
	for (char c : { 'A', 'B', 'C', 'D' })
		hostdb::record_host_key(db, key_of(c), alice, 0);
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('A'), 0x0A000001u) == alice);  // listed
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('B'), 0x0A000002u) == alice);  // gone
	list_rid(db, 0x0A000001u);
	age_key(db, key_of('A'), "-90000 seconds");
	age_key(db, key_of('B'), "-90000 seconds");
	age_key(db, key_of('C'), "-90000 seconds");  // never claimed, idle
	age_key(db, key_of('D'), "-3000 seconds");   // fresh

	TEST_EXPECT(hostdb::prune_host_keys(db) == 2);
	TEST_EXPECT(key_stored(db, key_of('A')));
	TEST_EXPECT(!key_stored(db, key_of('B')));
	TEST_EXPECT(!key_stored(db, key_of('C')));
	TEST_EXPECT(key_stored(db, key_of('D')));
	// The held key counts as used now, and still claims for its RID.
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('A'), 0x0A000001u) == alice);
	TEST_EXPECT(hostdb::prune_host_keys(db) == 0);
	return 0;
}

// The lobby session's links: request, update, roster statements, re-host.
int test_lobby_links() {
	test_db::TestDatabase scratch;
	opennova::db::Database db = scratch.open();
	const int64_t alice = make_player(db, "alice");
	hostdb::record_host_key(db, key_of('A'), alice, 0);

	LobbySession sess;
	sess.set_database(&db);
	// A stock host's fresh request carries no HostKey: listed, no owner yet;
	// the update that first carries the key links it.
	LobbyState a;
	auto r = sess.dispatch(fresh_request("Owned"), a, "10.0.0.7", 40000);
	TEST_EXPECT(answered_hosting(r));
	TEST_EXPECT(a.hosting && !a.host_user_id && !a.owner_key && !a.awaiting_host_key);
	TEST_EXPECT(hostdb::find_host_by_rid(db, a.rid).has_value());
	TEST_EXPECT(!row_owner(db, a.rid).has_value());
	r = sess.dispatch(key_update(key_of('A')), a, "10.0.0.7", 40000);
	TEST_EXPECT(r.label == "ClientHostUpdate" && r.reply_containers.empty());
	TEST_EXPECT(a.host_user_id == alice && a.owner_key == key_of('A'));
	TEST_EXPECT(row_owner(db, a.rid) == alice);
	TEST_EXPECT(claimed_rid(db, key_of('A')) == static_cast<int64_t>(a.rid));
	TEST_EXPECT(hostdb::list_hosts_by_owner(db, alice).size() == 1);
	const auto row = hostdb::find_host_by_rid(db, a.rid);
	TEST_EXPECT(row && !row->created_at.empty() && !row->updated_at.empty());

	// The updates keep it.
	sess.dispatch(host_update("Owned Renamed"), a, "10.0.0.7", 40000);
	TEST_EXPECT(row_owner(db, a.rid) == alice);
	TEST_EXPECT(hostdb::find_host_by_rid(db, a.rid)->server_name == "Owned Renamed");
	sess.dispatch(player_added(1, "joiner"), a, "10.0.0.7", 40000);
	TEST_EXPECT(row_owner(db, a.rid) == alice);
	TEST_EXPECT(hostdb::find_host_by_rid(db, a.rid)->player_count == 1);

	// Another connection presenting the same key while it is listed: no owner.
	LobbyState thief;
	r = sess.dispatch(host_request("Thief", key_of('A')), thief, "10.0.0.66", 40000);
	TEST_EXPECT(r.label == "ClientHostRequest");
	TEST_EXPECT(!thief.host_user_id.has_value());
	TEST_EXPECT(thief.rid != a.rid && !row_owner(db, thief.rid).has_value());
	TEST_EXPECT(claimed_rid(db, key_of('A')) == static_cast<int64_t>(a.rid));
	sess.end_hosting(thief, "test");
	LobbyState late_thief;
	TEST_EXPECT(answered_hosting(sess.dispatch(fresh_request("Late Thief"), late_thief,
	                                           "10.0.0.67", 40000)));
	sess.dispatch(key_update(key_of('A')), late_thief, "10.0.0.67", 40000);
	TEST_EXPECT(late_thief.hosting && !late_thief.host_user_id);
	TEST_EXPECT(hostdb::find_host_by_rid(db, late_thief.rid).has_value());
	TEST_EXPECT(!row_owner(db, late_thief.rid).has_value());
	sess.end_hosting(late_thief, "test");

	// An anonymous host (no key, or a key no account has): listed, no owner.
	LobbyState anon;
	sess.dispatch(host_request("Anonymous", ""), anon, "10.0.0.8", 40000);
	TEST_EXPECT(!anon.host_user_id && hostdb::find_host_by_rid(db, anon.rid).has_value());
	TEST_EXPECT(!row_owner(db, anon.rid).has_value());
	LobbyState unknown;
	sess.dispatch(host_request("Unknown Key", key_of('O')), unknown, "10.0.0.10", 40000);
	TEST_EXPECT(!unknown.host_user_id && hostdb::find_host_by_rid(db, unknown.rid).has_value());

	// The connection drops: its row goes and its state parks (the listener
	// keeps the LobbyState, RID and all, unhosted); the re-host on the
	// reconnect (CurrentlyHosting=1) keeps the RID and the link.
	const uint32_t rid = a.rid;
	hostdb::remove_host_by_rid(db, rid);
	LobbyState parked = a;
	parked.hosting = false;
	r = sess.dispatch(host_request("Owned Renamed", key_of('A'), "1"), parked, "10.0.0.7", 40000);
	TEST_EXPECT(r.label == "ClientHostRequest");
	TEST_EXPECT(parked.rid == rid && parked.host_user_id == alice);
	TEST_EXPECT(row_owner(db, rid) == alice);

	// The owner deleted while listed (the row's owner is set NULL with it): the
	// connection's next update writes no owner rather than tripping the key.
	db.exec("DELETE FROM players WHERE id = $1;", {BindValue(alice)});
	TEST_EXPECT(!row_owner(db, rid).has_value());
	sess.dispatch(host_update("After Delete"), parked, "10.0.0.7", 40000);
	const auto after = hostdb::find_host_by_rid(db, rid);
	TEST_EXPECT(after && after->server_name == "After Delete");
	TEST_EXPECT(!after->host_user_id.has_value());
	return 0;
}

// A connection keeps its RID across a stop, so its next hosting presents the
// fresh key its host page minted under the RID the earlier key still names:
// the claim lets the earlier key go (one of the account's keys per RID), and
// the row's writes keep naming the owner. Two claims on one RID (none
// claim_host_key leaves) still write: the row takes the newest's account.
int test_rehost_new_key() {
	test_db::TestDatabase scratch;
	opennova::db::Database db = scratch.open();
	const int64_t alice = make_player(db, "alice");

	// The store: a second key claimed for the RID the first holds.
	constexpr uint32_t kRid = 0x0A000021u;
	hostdb::record_host_key(db, key_of('A'), alice, 0);
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('A'), kRid) == alice);
	hostdb::record_host_key(db, key_of('B'), alice, 0);
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('B'), kRid) == alice);
	TEST_EXPECT(!claimed_rid(db, key_of('A')).has_value());
	TEST_EXPECT(claimed_rid(db, key_of('B')) == kRid);
	// Another RID's claim stays.
	hostdb::record_host_key(db, key_of('C'), alice, 0);
	TEST_EXPECT(hostdb::claim_host_key(db, key_of('C'), kRid + 1) == alice);
	TEST_EXPECT(claimed_rid(db, key_of('B')) == kRid && claimed_rid(db, key_of('C')) == kRid + 1);

	hostdb::HostRow row;
	row.rid = kRid;
	row.gsid = "GSID-REHOST";
	row.game = "jop_2_consumer";
	row.host_ip = "10.0.0.1";
	row.peer_ip = "10.0.0.1";
	row.host_user_id = alice;
	db.exec("UPDATE host_keys SET claimed_rid = $1;", {BindValue(static_cast<int64_t>(kRid))});
	hostdb::upsert_host(db, row);
	hostdb::update_host(db, row);
	TEST_EXPECT(row_owner(db, kRid) == alice);
	hostdb::clear_all(db);

	// The lobby: host, stop, a fresh key, host again on the same connection.
	hostdb::record_host_key(db, key_of('D'), alice, 0);
	LobbySession sess;
	sess.set_database(&db);
	LobbyState conn;
	TEST_EXPECT(answered_hosting(sess.dispatch(fresh_request("First"), conn, "10.0.0.7", 40000)));
	sess.dispatch(key_update(key_of('D')), conn, "10.0.0.7", 40000);
	const uint32_t rid = conn.rid;
	TEST_EXPECT(row_owner(db, rid) == alice);
	NapiMessage stop;
	stop.name = "ClientStopHosting";
	TEST_EXPECT(sess.dispatch(stop, conn, "10.0.0.7", 40000).label == "ClientStopHosting");
	TEST_EXPECT(!conn.hosting && !hostdb::find_host_by_rid(db, rid).has_value());
	TEST_EXPECT(claimed_rid(db, key_of('D')) == static_cast<int64_t>(rid));

	hostdb::record_host_key(db, key_of('E'), alice, 0);
	TEST_EXPECT(answered_hosting(sess.dispatch(fresh_request("Second"), conn, "10.0.0.7", 40000)));
	TEST_EXPECT(conn.rid == rid);
	auto r = sess.dispatch(key_update(key_of('E')), conn, "10.0.0.7", 40000);
	TEST_EXPECT(r.label == "ClientHostUpdate" && r.reply_containers.empty());
	TEST_EXPECT(conn.host_user_id == alice && conn.owner_key == key_of('E'));
	TEST_EXPECT(!claimed_rid(db, key_of('D')).has_value());
	TEST_EXPECT(claimed_rid(db, key_of('E')) == static_cast<int64_t>(rid));
	TEST_EXPECT(row_owner(db, rid) == alice);
	sess.dispatch(host_update("Second Renamed"), conn, "10.0.0.7", 40000);
	sess.dispatch(player_added(1, "joiner"), conn, "10.0.0.7", 40000);
	const auto listed = hostdb::find_host_by_rid(db, rid);
	TEST_EXPECT(listed && listed->server_name == "Second Renamed" && listed->player_count == 1);
	TEST_EXPECT(listed->host_user_id == alice);
	TEST_EXPECT(hostdb::listing_owned_by(db, rid, alice));
	TEST_EXPECT(hostdb::list_hosts_by_owner(db, alice).size() == 1);
	return 0;
}

// A row names its owner only while one of the owner's keys stands claimed for
// the row's RID. A revoked key (a password reset or a ban deletes the account's
// keys) drops the owner at the listing's next write; a deleted account's keys
// cascade away and its rows' owner is set NULL, a newcomer never gets its id
// (players.id is AUTOINCREMENT), and the listing's next write (the lobby still
// holding the old id) names no one; a row written with an owner but no claim
// (the dev inject) names no one.
int test_owner_needs_live_claim() {
	test_db::TestDatabase scratch;
	opennova::db::Database db = scratch.open();
	const int64_t alice = make_player(db, "alice");
	hostdb::record_host_key(db, key_of('A'), alice, 0);
	hostdb::record_host_key(db, key_of('B'), alice, 0);

	LobbySession sess;
	sess.set_database(&db);
	LobbyState revoked;
	TEST_EXPECT(answered_hosting(sess.dispatch(fresh_request("Revoked"), revoked, "10.0.0.7", 40000)));
	sess.dispatch(key_update(key_of('A')), revoked, "10.0.0.7", 40000);
	TEST_EXPECT(row_owner(db, revoked.rid) == alice);
	db.exec("DELETE FROM host_keys WHERE key_hash = $1;", {BindValue(hostdb::host_key_hash(key_of('A')))});
	sess.dispatch(host_update("Revoked Renamed"), revoked, "10.0.0.7", 40000);
	TEST_EXPECT(revoked.host_user_id == alice); // the lobby's memory; the row is what is read
	TEST_EXPECT(!row_owner(db, revoked.rid).has_value());

	LobbyState owned;
	TEST_EXPECT(answered_hosting(sess.dispatch(fresh_request("Owned"), owned, "10.0.0.8", 40000)));
	sess.dispatch(key_update(key_of('B')), owned, "10.0.0.8", 40000);
	TEST_EXPECT(row_owner(db, owned.rid) == alice);

	// The owner deleted, and a newcomer registered: never with the deleted id.
	db.exec("DELETE FROM players WHERE id = $1;", {BindValue(alice)});
	TEST_EXPECT(!row_owner(db, owned.rid).has_value());
	const int64_t newcomer = make_player(db, "newcomer");
	TEST_EXPECT(newcomer > alice);
	sess.dispatch(host_update("Owned Renamed"), owned, "10.0.0.8", 40000);
	sess.dispatch(player_added(1, "joiner"), owned, "10.0.0.8", 40000);
	const auto refreshed = hostdb::find_host_by_rid(db, owned.rid);
	TEST_EXPECT(refreshed && refreshed->server_name == "Owned Renamed");
	TEST_EXPECT(!refreshed->host_user_id.has_value());
	TEST_EXPECT(hostdb::list_hosts_by_owner(db, alice).empty());

	// An owner with no claim behind it is not stored.
	hostdb::HostRow injected;
	injected.rid = 0x0B000001u;
	injected.gsid = "GSID-INJECTED";
	injected.game = "jop_2_consumer";
	injected.host_ip = "10.0.0.9";
	injected.peer_ip = "10.0.0.9";
	injected.host_user_id = newcomer;
	hostdb::upsert_host(db, injected);
	TEST_EXPECT(!row_owner(db, injected.rid).has_value());
	return 0;
}

// ONNET_REQUIRE_HOST_KEY. A request carrying a key that names no account is
// refused (ServerHostResult Success=0, MsgCode 200: a stock host's NWEC58); one
// carrying a known key lists with its owner. A stock host's keyless fresh
// request is answered but held out of the browser (its roster too) until an
// update's key names its owner; one whose key names none (unknown, empty, or a
// listed server's) is ended with ServerStopHosting (MsgCode 200,
// NWUSERVERMSGCODE_NEEDTOLOGIN). A re-host whose key went (a revoking update)
// is refused and leaves the browser.
int test_require_host_key() {
	TEST_EXPECT(opennova::novaworld_host_error_tag(opennova::SERVER_MSG_CODE_NEED_TO_LOGIN) ==
	            "NWEC58");
	TEST_EXPECT(opennova::novaworld_server_msg_code_key(opennova::SERVER_MSG_CODE_NEED_TO_LOGIN) ==
	            "NWUSERVERMSGCODE_NEEDTOLOGIN");

	test_db::TestDatabase scratch;
	opennova::db::Database db = scratch.open();
	const int64_t alice = make_player(db, "alice");
	hostdb::record_host_key(db, key_of('A'), alice, 0);
	hostdb::record_host_key(db, key_of('B'), alice, 0);

	LobbySession sess;
	sess.set_database(&db);
	sess.set_require_host_key(true);

	// A fresh stock host: answered, held, its roster too; a keyless update keeps
	// it held; the update with its key lists it with the owner.
	LobbyState held;
	auto r = sess.dispatch(fresh_request("Held"), held, "10.0.0.5", 40000);
	TEST_EXPECT(answered_hosting(r));
	TEST_EXPECT(held.hosting && held.awaiting_host_key && hostdb::list_hosts(db).empty());
	sess.dispatch(player_added(0, "hoster"), held, "10.0.0.5", 40000);
	sess.dispatch(host_update("Held Renamed"), held, "10.0.0.5", 40000);
	TEST_EXPECT(held.awaiting_host_key && hostdb::list_hosts(db).empty());
	r = sess.dispatch(key_update(key_of('B')), held, "10.0.0.5", 40000);
	TEST_EXPECT(r.label == "ClientHostUpdate" && r.reply_containers.empty());
	TEST_EXPECT(held.hosting && !held.awaiting_host_key && held.host_user_id == alice);
	const auto listed = hostdb::find_host_by_rid(db, held.rid);
	TEST_EXPECT(listed && listed->host_user_id == alice && listed->server_name == "Held Renamed");
	TEST_EXPECT(listed->player_count == 1 && hostdb::list_roster(db, held.rid).size() == 1);
	sess.end_hosting(held, "test");

	// Held hosts whose first key names no account are ended.
	for (const std::string &bad : {std::string(), key_of('O')}) {
		LobbyState s;
		TEST_EXPECT(answered_hosting(sess.dispatch(fresh_request("Stopped"), s, "10.0.0.6", 40000)));
		TEST_EXPECT(stopped_unknown_key(sess.dispatch(key_update(bad), s, "10.0.0.6", 40000)));
		TEST_EXPECT(!s.hosting && !s.awaiting_host_key && hostdb::list_hosts(db).empty());
	}

	LobbyState anon;
	r = sess.dispatch(host_request("Anonymous", ""), anon, "10.0.0.8", 40000);
	TEST_EXPECT(refused_unknown_key(r));
	TEST_EXPECT(!anon.hosting && hostdb::list_hosts(db).empty());
	LobbyState unknown;
	r = sess.dispatch(host_request("Unknown Key", key_of('O')), unknown, "10.0.0.9", 40000);
	TEST_EXPECT(refused_unknown_key(r));

	LobbyState a;
	r = sess.dispatch(host_request("Owned", key_of('A')), a, "10.0.0.7", 40000);
	TEST_EXPECT(r.label == "ClientHostRequest" && field(r.reply_containers.at(0), "Success") == "1");
	TEST_EXPECT(a.hosting && row_owner(db, a.rid) == alice);

	// Another connection presenting the listed key is refused, in its request
	// or in its first update.
	LobbyState thief;
	r = sess.dispatch(host_request("Thief", key_of('A')), thief, "10.0.0.66", 40000);
	TEST_EXPECT(refused_unknown_key(r));
	TEST_EXPECT(hostdb::list_hosts(db).size() == 1);
	LobbyState late_thief;
	TEST_EXPECT(answered_hosting(sess.dispatch(fresh_request("Late Thief"), late_thief,
	                                           "10.0.0.67", 40000)));
	TEST_EXPECT(stopped_unknown_key(sess.dispatch(key_update(key_of('A')), late_thief,
	                                              "10.0.0.67", 40000)));
	TEST_EXPECT(!late_thief.hosting && hostdb::list_hosts(db).size() == 1);

	// A keyless request on the listed connection holds it again: its row and
	// roster leave the browser until a key names the owner once more.
	sess.dispatch(player_added(2, "joiner"), a, "10.0.0.7", 40000);
	TEST_EXPECT(hostdb::list_roster(db, a.rid).size() == 1);
	r = sess.dispatch(fresh_request("Owned Again"), a, "10.0.0.7", 40000);
	TEST_EXPECT(answered_hosting(r) && a.hosting && a.awaiting_host_key);
	TEST_EXPECT(!hostdb::find_host_by_rid(db, a.rid).has_value());
	TEST_EXPECT(hostdb::list_roster(db, a.rid).empty() && hostdb::list_hosts(db).empty());
	sess.dispatch(key_update(key_of('A')), a, "10.0.0.7", 40000);
	TEST_EXPECT(!a.awaiting_host_key && row_owner(db, a.rid) == alice);
	TEST_EXPECT(hostdb::find_host_by_rid(db, a.rid)->server_name == "Owned Again");

	// The key revoked (a password reset deletes the account's keys); the same
	// connection's re-request is refused and the row leaves the browser.
	db.exec("DELETE FROM host_keys WHERE user_id = $1;", {BindValue(alice)});
	r = sess.dispatch(host_request("Owned", key_of('A'), "1"), a, "10.0.0.7", 40000);
	TEST_EXPECT(refused_unknown_key(r));
	TEST_EXPECT(!a.hosting && hostdb::list_hosts(db).empty());

	// With no database there is nothing to check a key against: refused.
	LobbySession no_db;
	no_db.set_require_host_key(true);
	LobbyState s;
	TEST_EXPECT(refused_unknown_key(no_db.dispatch(host_request("No DB", key_of('A')), s,
	                                               "10.0.0.7", 40000)));
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	if (test_key_form_and_hash() != 0) return 1;
	if (test_record() != 0) return 1;
	if (test_claim() != 0) return 1;
	if (test_listing_owned_by() != 0) return 1;
	if (test_prune() != 0) return 1;
	if (test_lobby_links() != 0) return 1;
	if (test_rehost_new_key() != 0) return 1;
	if (test_owner_needs_live_claim() != 0) return 1;
	if (test_require_host_key() != 0) return 1;
	std::printf("OK: host keys (the store, the claims, the sweep, the lobby's links, "
	            "ONNET_REQUIRE_HOST_KEY)\n");
	return 0;
}
