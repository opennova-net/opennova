// The lobby's tables below the listener (engine/net/novaworld/lobby_repository.h, migrations
// 0006_lobby_commands.sql, 0009_lobby_instances.sql, 0012_lobby_instances_share_endpoint.sql,
// 0020_lobby_live_view.sql and 0022_lobby_goodbyes.sql), on a migrated database per case: the
// goodbyes a lobby's clean stop leaves (listed within their window, replaced by address, the
// older deleted by the next stop's write), the lobby instances (each
// registration a row of its own naming its task, two
// at one endpoint, listed with how long ago each was seen, touched, drained, the live ones those
// active and seen within the live window and named by the newest one's endpoint, one unseen for
// longer than a database failover left alone, the stale reaped with their rows, a clean stop's
// delete), the RIDs from one sequence (a held suffix passed over past the
// wrap), the session rows the lobby writes and the routes read (the account each signed in as
// and the server it plays on; its sweep of the rows it holds no session for, its own
// instance's alone), the account a retail login's tag names, and the commands the
// routes and the supervisor queue for it. A push answers from the session rows what the lobby
// would (a RID no session holds, one not hosting, one another account owns) and inserts only when
// the lobby would take it, bearing the session's lobby_id; an end names its session by its row's
// id; forget_owner names none and goes to every instance; each lobby takes its own instance's in
// insert order, each once, with their expiry, never another's; a session's delete takes its
// commands; a lobby's start (hostdb::clear_unheld) takes the rows no instance holds, never an
// instance's.

#include <net/novaworld/db/database.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_repository.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "common/test_database.h"
#include "common/test_expect.h"

namespace {

using opennova::db::BindValue;
using opennova::db::Database;
namespace lobbydb = opennova::lobbydb;
using lobbydb::PushResult;

constexpr int64_t kNow = 1792120342793; // 2026-10-16 09:39 UTC, in epoch ms

lobbydb::SessionRow session(uint32_t connection_id, int port, int64_t lobby_id = 0) {
	lobbydb::SessionRow s;
	s.lobby_id = lobby_id;
	s.connection_id = connection_id;
	s.peer_ip = "10.1.2.3";
	s.peer_port = port;
	s.state = "active";
	s.pn = "NOVAWORLDUDP";
	s.identity = "jop:cus2";
	s.connected_ms = kNow - 60000;
	s.last_seen_ms = kNow;
	return s;
}

// A hosting session's columns on `s`.
lobbydb::SessionRow hosting(lobbydb::SessionRow s, uint32_t rid,
                            std::optional<int64_t> owner = std::nullopt) {
	s.rid = rid;
	s.hosting = true;
	s.listed = true;
	s.host_user_id = owner;
	s.server_name = "Lobby Host";
	s.mission_name = "Map";
	s.players = 3;
	s.max_players = 16;
	return s;
}

std::size_t command_count(Database &db) {
	return db.query("SELECT 1 FROM lobby_commands;").size();
}

// Instance `id`'s row, if any.
std::optional<lobbydb::LobbyInstance> instance_of(Database &db, int64_t id) {
	for (const lobbydb::LobbyInstance &i : lobbydb::list_instances(db))
		if (i.id == id)
			return i;
	return std::nullopt;
}

bool refused(Database &db, const std::string &sql) {
	try {
		db.exec(sql);
	} catch (const opennova::db::Error &) {
		return true;
	}
	return false;
}

int test_session_rows() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();
	// A client signed in as account 41, playing on a listing since half a minute ago.
	lobbydb::SessionRow a = session(1, 40001);
	a.user_id = 41;
	a.playing_rid = 0x0A000031u;
	a.play_addr = "203.0.113.31:32768";
	a.playing_since_ms = kNow - 30000;
	a.id = lobbydb::insert_session(db, a);
	TEST_EXPECT(a.id > 0);
	lobbydb::SessionRow b = hosting(session(0x80000000u, 40002), 0x0A000001u, 77);
	b.id = lobbydb::insert_session(db, b);
	TEST_EXPECT(b.id > a.id);

	auto rows = lobbydb::list_sessions(db);
	TEST_EXPECT(rows.size() == 2);
	TEST_EXPECT(rows[0].id == a.id && lobbydb::same_columns(rows[0], a));
	TEST_EXPECT(rows[0].connected_ms == a.connected_ms && rows[0].last_seen_ms == a.last_seen_ms);
	TEST_EXPECT(rows[0].rid == 0 && !rows[0].host_user_id && rows[0].user_id == 41);
	TEST_EXPECT(rows[0].playing_rid == 0x0A000031u && rows[0].play_addr == "203.0.113.31:32768" &&
			rows[0].playing_since_ms == kNow - 30000);
	TEST_EXPECT(lobbydb::session_addr(rows[0]) == "10.1.2.3:40001");
	TEST_EXPECT(rows[1].id == b.id && lobbydb::same_columns(rows[1], b));
	TEST_EXPECT(rows[1].connection_id == 0x80000000u && rows[1].host_user_id == 77);
	// Playing on nothing, signed in as no one: NULLs, read back as none.
	TEST_EXPECT(!rows[1].user_id && rows[1].playing_rid == 0 && rows[1].play_addr.empty() &&
			rows[1].playing_since_ms == 0);
	TEST_EXPECT(db.query("SELECT 1 FROM lobby_sessions WHERE id = $1 AND user_id IS NULL AND "
						 "playing_rid IS NULL AND playing_since IS NULL;",
						{ BindValue(b.id) })
						.size() == 1);

	// An update writes the columns, never the times; touch moves last_seen_at alone.
	lobbydb::SessionRow changed = a;
	changed.state = "closing";
	changed.connected_ms = 1;
	changed.last_seen_ms = 1;
	TEST_EXPECT(!lobbydb::same_columns(changed, a));
	lobbydb::update_session(db, changed);
	rows = lobbydb::list_sessions(db);
	TEST_EXPECT(rows[0].state == "closing" && rows[0].connected_ms == a.connected_ms &&
	            rows[0].last_seen_ms == a.last_seen_ms);
	// It stops playing: the play columns clear; another account's tag moves the account.
	changed.playing_rid = 0;
	changed.play_addr.clear();
	changed.playing_since_ms = 0;
	changed.user_id = 43;
	TEST_EXPECT(!lobbydb::same_columns(changed, rows[0]));
	lobbydb::update_session(db, changed);
	rows = lobbydb::list_sessions(db);
	TEST_EXPECT(lobbydb::same_columns(rows[0], changed) && rows[0].user_id == 43 &&
			rows[0].playing_since_ms == 0);
	lobbydb::touch_sessions(db, {{a.id, kNow + 5000}, {b.id, kNow + 7000}, {999999, kNow}});
	rows = lobbydb::list_sessions(db);
	TEST_EXPECT(rows[0].last_seen_ms == kNow + 5000 && rows[1].last_seen_ms == kNow + 7000);
	lobbydb::touch_sessions(db, {});

	// The table's rules: a state of the registry's, a listing only while hosting.
	TEST_EXPECT(refused(db, "UPDATE lobby_sessions SET state = 'gone';"));
	TEST_EXPECT(refused(db, "UPDATE lobby_sessions SET listed = true, hosting = false;"));
	TEST_EXPECT(refused(db, "UPDATE lobby_sessions SET rid = 0;"));
	TEST_EXPECT(refused(db, "UPDATE lobby_sessions SET playing_rid = 0;"));

	lobbydb::delete_session(db, a.id);
	rows = lobbydb::list_sessions(db);
	TEST_EXPECT(rows.size() == 1 && rows[0].id == b.id);

	// The lobby's sweep: a row it holds no session for (a drop's delete that failed, hosting)
	// answers a push for its RID until the sweep takes it with its commands; the held one stays,
	// and so do another instance's and the rows no instance holds.
	const int64_t lobby = lobbydb::register_instance(db, "10.0.0.5:64206");
	const int64_t other = lobbydb::register_instance(db, "10.0.0.6:64206");
	lobbydb::SessionRow held = session(8, 40008, lobby);
	held.id = lobbydb::insert_session(db, held);
	lobbydb::SessionRow ghost = hosting(session(9, 40009, lobby), 0x0A000009u);
	ghost.id = lobbydb::insert_session(db, ghost);
	lobbydb::SessionRow theirs = session(10, 40010, other);
	theirs.id = lobbydb::insert_session(db, theirs);
	TEST_EXPECT(lobbydb::queue_server_command(db, ghost.rid, "Cycle") == PushResult::Queued);
	TEST_EXPECT(lobbydb::delete_sessions_except(db, lobby, { held.id }) == 1);
	rows = lobbydb::list_sessions(db);
	TEST_EXPECT(rows.size() == 3 && rows[0].id == b.id && rows[1].id == held.id &&
			rows[2].id == theirs.id && command_count(db) == 0);
	TEST_EXPECT(lobbydb::queue_server_command(db, ghost.rid, "Cycle") == PushResult::UnknownRid);
	TEST_EXPECT(lobbydb::delete_sessions_except(db, lobby, { held.id }) == 0);
	TEST_EXPECT(lobbydb::delete_sessions_except(db, 0, {}) == 0);
	TEST_EXPECT(lobbydb::list_sessions(db).size() == 3);
	return 0;
}

int test_pushes() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();
	const uint32_t rid = 0x0A000007u;
	// Nothing listed: RID 0 and an unknown one.
	TEST_EXPECT(lobbydb::queue_server_command(db, 0, "Cycle") == PushResult::UnknownRid);
	TEST_EXPECT(lobbydb::queue_server_command(db, rid, "Cycle") == PushResult::UnknownRid);

	// A session that holds the RID and does not host.
	lobbydb::SessionRow idle = session(1, 40001);
	idle.rid = rid;
	idle.host_user_id = 42;
	idle.id = lobbydb::insert_session(db, idle);
	TEST_EXPECT(lobbydb::queue_server_command(db, rid, "Cycle") == PushResult::NotHosted);
	TEST_EXPECT(lobbydb::queue_stop_hosting(db, rid, 7) == PushResult::NotHosted);
	TEST_EXPECT(command_count(db) == 0);

	// The hosting one beside it takes the push, bearing its session's lobby_id.
	const int64_t lobby = lobbydb::register_instance(db, "10.0.0.5:64206");
	lobbydb::SessionRow host = hosting(session(2, 40002, lobby), rid, 42);
	host.id = lobbydb::insert_session(db, host);
	TEST_EXPECT(lobbydb::list_sessions(db).back().lobby_id == lobby);
	TEST_EXPECT(lobbydb::queue_server_command(db, rid, "SetServerName \"N\"") == PushResult::Queued);
	auto rows = db.query("SELECT kind, session_id, rid, account_id, cmd, lobby_id, "
	                     "expires_at > now() + interval '20 seconds' "
	                     "AND expires_at <= now() + interval '31 seconds' FROM lobby_commands;");
	TEST_EXPECT(rows.size() == 1);
	TEST_EXPECT(rows[0].as_text(0) == "server_command" && rows[0].as_int(1) == host.id);
	TEST_EXPECT(rows[0].as_int(2) == static_cast<int64_t>(rid) && rows[0].is_null(3));
	TEST_EXPECT(rows[0].as_text(4) == "SetServerName \"N\"" && rows[0].as_int(5) == lobby);
	TEST_EXPECT(rows[0].as_int(6) == 1);

	// An owner's push: the owner's session alone; another account's is the unknown RID's answer.
	TEST_EXPECT(lobbydb::queue_stop_hosting(db, rid, 7, 1, 2, 43) == PushResult::UnknownRid);
	TEST_EXPECT(lobbydb::queue_stop_hosting(db, rid, 7, 1, 2, 42) == PushResult::Queued);
	rows = db.query("SELECT kind, session_id, account_id, msg_code, msg_param1, msg_param2, cmd "
	                "FROM lobby_commands WHERE kind = 'stop_hosting';");
	TEST_EXPECT(rows.size() == 1 && rows[0].as_int(1) == host.id && rows[0].as_int(2) == 42);
	TEST_EXPECT(rows[0].as_int(3) == 7 && rows[0].as_int(4) == 1 && rows[0].as_int(5) == 2);
	TEST_EXPECT(rows[0].is_null(6));
	// The owner of a session that does not host: the queue's not-hosting answer.
	db.exec("UPDATE lobby_sessions SET hosting = false, listed = false WHERE id = $1;",
	        {BindValue(host.id)});
	TEST_EXPECT(lobbydb::queue_server_command(db, rid, "Cycle", 42) == PushResult::NotHosted);
	TEST_EXPECT(command_count(db) == 2);

	// The names the routes answer with.
	TEST_EXPECT(std::string(lobbydb::push_result_name(PushResult::Queued)) == "queued");
	TEST_EXPECT(std::string(lobbydb::push_result_name(PushResult::UnknownRid)) == "unknown_rid");
	TEST_EXPECT(std::string(lobbydb::push_result_name(PushResult::NotHosted)) == "not_hosting");
	return 0;
}

int test_end_session_and_forget() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();
	using Result = lobbydb::EndSession::Result;
	TEST_EXPECT(lobbydb::queue_end_session(db, 1).result == Result::NotFound);
	// Two instances' sessions share CI 1 and the address (a NATed client on two lobbies, or G.7's
	// two processes): the row's id names one, across the instances.
	const int64_t lobby_a = lobbydb::register_instance(db, "10.0.0.5:64206");
	const int64_t lobby_b = lobbydb::register_instance(db, "10.0.0.6:64206");
	lobbydb::SessionRow a = session(1, 40001, lobby_a);
	a.id = lobbydb::insert_session(db, a);
	lobbydb::SessionRow b = hosting(session(1, 40001, lobby_b), 0x0A000002u);
	b.id = lobbydb::insert_session(db, b);
	const lobbydb::EndSession hosted = lobbydb::queue_end_session(db, b.id);
	TEST_EXPECT(hosted.result == Result::Hosting && hosted.session.id == b.id &&
	            hosted.session.rid == 0x0A000002u && hosted.session.lobby_id == lobby_b);
	TEST_EXPECT(command_count(db) == 0);
	const lobbydb::EndSession ended = lobbydb::queue_end_session(db, a.id);
	TEST_EXPECT(ended.result == Result::Queued && ended.session.id == a.id &&
	            ended.session.identity == "jop:cus2");
	auto rows = db.query("SELECT kind, session_id, rid, lobby_id FROM lobby_commands;");
	TEST_EXPECT(rows.size() == 1 && rows[0].as_text(0) == "end_session" &&
	            rows[0].as_int(1) == a.id && rows[0].is_null(2) && rows[0].as_int(3) == lobby_a);
	TEST_EXPECT(std::string(lobbydb::end_session_result_name(Result::Hosting)) == "hosting");

	// forget_owner names no session: one row per instance, active or draining, each its own.
	TEST_EXPECT(lobbydb::set_instance_state(db, lobby_b, "draining"));
	lobbydb::queue_forget_owner(db, 42);
	rows = db.query("SELECT session_id, account_id, lobby_id FROM lobby_commands "
					"WHERE kind = 'forget_owner' ORDER BY lobby_id;");
	TEST_EXPECT(rows.size() == 2 && rows[0].is_null(0) && rows[0].as_int(1) == 42 &&
			rows[0].as_int(2) == lobby_a && rows[1].as_int(2) == lobby_b);
	const auto for_b = lobbydb::take_commands(db, lobby_b, 64);
	TEST_EXPECT(for_b.size() == 1 && for_b[0].kind == lobbydb::CommandKind::ForgetOwner &&
			for_b[0].account == 42);

	// The table's rules: a push names its session and RID, forget_owner its account alone.
	TEST_EXPECT(refused(db, "INSERT INTO lobby_commands (kind, expires_at) "
	                        "VALUES ('server_command', now());"));
	TEST_EXPECT(refused(db, "INSERT INTO lobby_commands (kind, account_id, expires_at) "
	                        "VALUES ('reboot', 1, now());"));
	TEST_EXPECT(refused(db, "INSERT INTO lobby_commands (kind, expires_at) "
	                        "VALUES ('forget_owner', now());"));
	TEST_EXPECT(refused(db, "INSERT INTO lobby_commands (kind, session_id, account_id, expires_at) "
	                        "SELECT 'forget_owner', id, 1, now() FROM lobby_sessions LIMIT 1;"));
	TEST_EXPECT(refused(db, "INSERT INTO lobby_commands (kind, session_id, expires_at) "
	                        "SELECT 'stop_hosting', id, now() FROM lobby_sessions LIMIT 1;"));
	return 0;
}

int test_take_and_cascade() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();
	const uint32_t rid = 0x0A000003u;
	const int64_t lobby = lobbydb::register_instance(db, "10.0.0.5:64206");
	lobbydb::SessionRow host = hosting(session(3, 40003, lobby), rid, 9);
	host.id = lobbydb::insert_session(db, host);
	lobbydb::SessionRow other = session(4, 40004, lobby);
	other.id = lobbydb::insert_session(db, other);
	// Another instance's session and its push: never this lobby's to take.
	const int64_t elsewhere = lobbydb::register_instance(db, "10.0.0.6:64206");
	lobbydb::SessionRow theirs = hosting(session(5, 40005, elsewhere), 0x0A000004u);
	theirs.id = lobbydb::insert_session(db, theirs);
	TEST_EXPECT(lobbydb::queue_server_command(db, 0x0A000004u, "Theirs") == PushResult::Queued);
	TEST_EXPECT(lobbydb::queue_server_command(db, rid, "Cycle") == PushResult::Queued);
	TEST_EXPECT(
			lobbydb::queue_end_session(db, other.id).result == lobbydb::EndSession::Result::Queued);
	lobbydb::queue_forget_owner(db, 9);
	TEST_EXPECT(
			lobbydb::take_commands(db, elsewhere, 64).size() == 2); // its push, its forget_owner
	TEST_EXPECT(lobbydb::queue_stop_hosting(db, rid, 7, 0, 0, 9) == PushResult::Queued);
	// The second one waited past its expiry.
	db.exec("UPDATE lobby_commands SET expires_at = now() - interval '1 second' "
	        "WHERE kind = 'end_session';");

	// Taken in insert order, two at a time, each once.
	auto taken = lobbydb::take_commands(db, lobby, 2);
	TEST_EXPECT(taken.size() == 2);
	TEST_EXPECT(taken[0].kind == lobbydb::CommandKind::ServerCommand && taken[0].cmd == "Cycle" &&
	            taken[0].session_id == host.id && taken[0].rid == rid && !taken[0].account &&
	            !taken[0].expired);
	TEST_EXPECT(taken[1].kind == lobbydb::CommandKind::EndSession &&
	            taken[1].session_id == other.id && taken[1].expired);
	TEST_EXPECT(taken[0].id < taken[1].id);
	taken = lobbydb::take_commands(db, lobby, 64);
	TEST_EXPECT(taken.size() == 2);
	TEST_EXPECT(taken[0].kind == lobbydb::CommandKind::ForgetOwner && taken[0].account == 9 &&
	            taken[0].session_id == 0 && !taken[0].expired);
	TEST_EXPECT(taken[1].kind == lobbydb::CommandKind::StopHosting && taken[1].msg_code == 7 &&
	            taken[1].account == 9);
	TEST_EXPECT(lobbydb::take_commands(db, lobby, 64).empty() && command_count(db) == 0);
	TEST_EXPECT(std::string(lobbydb::command_kind_name(lobbydb::CommandKind::ForgetOwner)) ==
	            "forget_owner");

	// A session's delete takes the commands addressed to it, not another's.
	TEST_EXPECT(lobbydb::queue_server_command(db, rid, "Cycle") == PushResult::Queued);
	TEST_EXPECT(
			lobbydb::queue_end_session(db, other.id).result == lobbydb::EndSession::Result::Queued);
	lobbydb::delete_session(db, host.id);
	auto rows = db.query("SELECT session_id FROM lobby_commands;");
	TEST_EXPECT(rows.size() == 1 && rows[0].as_int(0) == other.id);
	// A push whose session went: the unknown RID's answer, nothing inserted.
	TEST_EXPECT(lobbydb::queue_server_command(db, rid, "Cycle") == PushResult::UnknownRid);

	// A lobby's start takes the sessions and commands no instance holds (an end addressed to a
	// session with none); every instance's own stay, since each start registers an instance of
	// its own.
	lobbydb::queue_forget_owner(db, 9);
	lobbydb::SessionRow orphan = session(6, 40006);
	orphan.id = lobbydb::insert_session(db, orphan);
	TEST_EXPECT(lobbydb::queue_end_session(db, orphan.id).result ==
			lobbydb::EndSession::Result::Queued);
	TEST_EXPECT(command_count(db) == 4);
	opennova::hostdb::clear_unheld(db);
	const auto left = lobbydb::list_sessions(db);
	TEST_EXPECT(left.size() == 2);
	for (const auto &s : left)
		TEST_EXPECT(s.id == other.id || s.id == theirs.id);
	TEST_EXPECT(db.query("SELECT 1 FROM lobby_commands WHERE lobby_id IS NULL;").empty());
	TEST_EXPECT(command_count(db) == 3); // other's end, each instance's forget_owner
	return 0;
}

// The instances: each registration a row of its own (two lobbies at one endpoint, the
// deployment's one lobby address, under ids of their own; a running lobby's registration again
// keeps its drain), touched (the touch reading the row's state), drained (out of the live
// lobbies, which count the active ones seen within the live window and name the newest one's
// endpoint), reaped once stale with their sessions, commands and listings, deleted at a clean
// stop with theirs; the RIDs, one sequence for every instance, never collide.
int test_instances_and_rids() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();
	const std::string shared = "198.51.100.1:64206";
	const int64_t a = lobbydb::register_instance(db, shared);
	const int64_t b = lobbydb::register_instance(db, shared);
	TEST_EXPECT(a > 0 && b > a);
	lobbydb::LiveLobbies live = lobbydb::live_lobbies(db);
	TEST_EXPECT(live.endpoint == shared && live.instances == 2);
	const std::optional<lobbydb::LobbyInstance> found = instance_of(db, a);
	TEST_EXPECT(found && found->endpoint == shared && found->state == "active" &&
			found->seen_ago_ms >= 0 && found->seen_ago_ms < 5000 && found->task_id.empty());
	// One registered with the task it runs as names it (the admin's link to its usage).
	const int64_t tasked = lobbydb::register_instance(db, shared, false, "4f1c2a9e0b7d4c3e");
	TEST_EXPECT(instance_of(db, tasked) && instance_of(db, tasked)->task_id == "4f1c2a9e0b7d4c3e");
	lobbydb::remove_instance(db, tasked);
	TEST_EXPECT(!instance_of(db, 999999));
	TEST_EXPECT(lobbydb::set_instance_state(db, b, "draining"));
	TEST_EXPECT(!lobbydb::set_instance_state(db, b, "gone") &&
			!lobbydb::set_instance_state(db, 999999, "active"));
	live = lobbydb::live_lobbies(db);
	TEST_EXPECT(live.endpoint == shared && live.instances == 1);
	TEST_EXPECT(lobbydb::touch_instance(db, a) == std::optional<std::string>("active"));
	TEST_EXPECT(lobbydb::touch_instance(db, b) == std::optional<std::string>("draining"));
	// A drained lobby registering again (its row reaped while it ran): a new row, drained still,
	// so not live.
	const int64_t b_again = lobbydb::register_instance(db, shared, true);
	TEST_EXPECT(b_again > b);
	TEST_EXPECT(lobbydb::touch_instance(db, b_again) == std::optional<std::string>("draining"));
	TEST_EXPECT(lobbydb::live_lobbies(db).instances == 1);
	lobbydb::remove_instance(db, b_again);
	// The newest live instance's endpoint is the one handed out.
	const int64_t elsewhere = lobbydb::register_instance(db, "198.51.100.2:64206");
	live = lobbydb::live_lobbies(db);
	TEST_EXPECT(live.endpoint == "198.51.100.2:64206" && live.instances == 2);
	lobbydb::remove_instance(db, elsewhere);
	// None live: no endpoint.
	TEST_EXPECT(lobbydb::set_instance_state(db, a, "draining"));
	live = lobbydb::live_lobbies(db);
	TEST_EXPECT(live.endpoint.empty() && live.instances == 0);
	TEST_EXPECT(lobbydb::set_instance_state(db, a, "active"));
	TEST_EXPECT(lobbydb::set_instance_state(db, b, "active"));
	TEST_EXPECT(refused(db, "INSERT INTO lobby_instances (endpoint) VALUES ('');"));
	TEST_EXPECT(refused(db, "UPDATE lobby_instances SET state = 'gone';"));

	// RIDs: one sequence, 0x0A and 1..0xFFFFFF, distinct however many lobbies mint them.
	std::vector<uint32_t> rids;
	for (int i = 0; i < 200; ++i)
		rids.push_back(lobbydb::next_rid(db));
	for (size_t i = 0; i < rids.size(); ++i) {
		TEST_EXPECT((rids[i] & 0xFF000000u) == 0x0A000000u && (rids[i] & 0x00FFFFFFu) != 0);
		for (size_t j = i + 1; j < rids.size(); ++j)
			TEST_EXPECT(rids[i] != rids[j]);
	}

	// A's rows: a session, its command, a listing; B's beside them.
	const uint32_t rid_a = rids[0];
	const uint32_t rid_b = rids[1];
	lobbydb::SessionRow sa = hosting(session(1, 40001, a), rid_a);
	sa.id = lobbydb::insert_session(db, sa);
	lobbydb::SessionRow sb = hosting(session(1, 40002, b), rid_b);
	sb.id = lobbydb::insert_session(db, sb);
	for (const auto &[rid, lobby] : { std::make_pair(rid_a, a), std::make_pair(rid_b, b) }) {
		opennova::hostdb::HostRow row;
		row.rid = rid;
		row.game = "jop_2_consumer";
		row.server_name = "Host";
		row.lobby_id = lobby;
		opennova::hostdb::upsert_host(db, row);
	}
	TEST_EXPECT(opennova::hostdb::find_host_by_rid(db, rid_a)->lobby_id == a);
	TEST_EXPECT(lobbydb::queue_server_command(db, rid_a, "Cycle") == PushResult::Queued);

	// A's heartbeat stops for two minutes (a database failover's outage, longer than one takes):
	// no gate counts it as taking clients, and no reap takes it or its rows.
	db.exec("UPDATE lobby_instances SET last_seen_at = now() - interval '2 minutes' WHERE id = $1;",
			{ BindValue(a) });
	live = lobbydb::live_lobbies(db);
	TEST_EXPECT(live.endpoint == shared && live.instances == 1);
	TEST_EXPECT(instance_of(db, a)->seen_ago_ms >= 119000);
	TEST_EXPECT(lobbydb::reap_stale_instances(db, lobbydb::kInstanceStaleSeconds, b) == 0);
	TEST_EXPECT(lobbydb::reap_stale_instances(db, lobbydb::kInstanceStaleSeconds) == 0);
	TEST_EXPECT(opennova::hostdb::find_host_by_rid(db, rid_a) && command_count(db) == 1);
	// A's process went (no touch for six minutes): a reap takes it and its rows, B's stay.
	db.exec("UPDATE lobby_instances SET last_seen_at = now() - interval '6 minutes' WHERE id = $1;",
			{ BindValue(a) });
	TEST_EXPECT(lobbydb::reap_stale_instances(db, lobbydb::kInstanceStaleSeconds, b) == 1);
	TEST_EXPECT(!lobbydb::touch_instance(db, a));
	TEST_EXPECT(!opennova::hostdb::find_host_by_rid(db, rid_a) &&
			opennova::hostdb::find_host_by_rid(db, rid_b));
	auto sessions = lobbydb::list_sessions(db);
	TEST_EXPECT(sessions.size() == 1 && sessions[0].id == sb.id && command_count(db) == 0);
	// The one a reap keeps (the caller's own) stays however stale.
	db.exec("UPDATE lobby_instances SET last_seen_at = now() - interval '6 minutes';");
	TEST_EXPECT(lobbydb::reap_stale_instances(db, lobbydb::kInstanceStaleSeconds, b) == 0);
	// A lobby with no instance takes no command (NULL-addressed ones stay).
	TEST_EXPECT(lobbydb::queue_server_command(db, rid_b, "Cycle") == PushResult::Queued);
	db.exec("UPDATE lobby_commands SET lobby_id = NULL;");
	TEST_EXPECT(lobbydb::take_commands(db, 0, 64).empty() && command_count(db) == 1);
	// B's clean stop: its row and everything naming it.
	lobbydb::remove_instance(db, b);
	TEST_EXPECT(lobbydb::list_instances(db).empty() && lobbydb::list_sessions(db).empty() &&
			!opennova::hostdb::find_host_by_rid(db, rid_b));

	// The sequence cycles: a suffix a listing or a session row still holds is passed over, so a
	// RID minted past the wrap never lands on a live one.
	db.exec("SELECT setval('lobby_rid_seq', 16777214, false);");
	opennova::hostdb::HostRow listed;
	listed.rid = 0x0AFFFFFEu;
	listed.game = "jop_2_consumer";
	listed.server_name = "Listed";
	opennova::hostdb::upsert_host(db, listed);
	lobbydb::SessionRow listing_session = hosting(session(2, 40003), 0x0AFFFFFFu);
	lobbydb::insert_session(db, listing_session);
	lobbydb::SessionRow parked = session(3, 40004);
	parked.rid = 0x0A000001u;
	lobbydb::insert_session(db, parked);
	TEST_EXPECT(lobbydb::next_rid(db) == 0x0A000002u);
	TEST_EXPECT(lobbydb::next_rid(db) == 0x0A000003u);
	return 0;
}

// The account a session signs in as (lobbydb::login_account): the one whose live retail login
// set the tag, only while the account is at the credentials_gen the login was made under (a
// password reset or a ban moves it); none for an empty or unknown tag.
int test_login_account() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();
	const int64_t pilot = db.query("INSERT INTO players (username, password_hash, pcid, nwh, "
								   "nwhandle) VALUES ('lobby_pilot', 'x', '0000c101', '1', "
								   "'Pilot') RETURNING id;")
								  .front()
								  .as_int(0)
								  .value_or(0);
	TEST_EXPECT(pilot > 0);
	db.exec("INSERT INTO active_user_sessions (user_id, username, session_tag, credentials_gen) "
			"VALUES ($1, 'lobby_pilot', 'tag-lobby-pilot', 0);",
			{ BindValue(pilot) });
	TEST_EXPECT(lobbydb::login_account(db, "tag-lobby-pilot") == pilot);
	TEST_EXPECT(!lobbydb::login_account(db, ""));
	TEST_EXPECT(!lobbydb::login_account(db, "tag-lobby-nobody"));
	db.exec("UPDATE players SET credentials_gen = credentials_gen + 1 WHERE id = $1;",
			{ BindValue(pilot) });
	TEST_EXPECT(!lobbydb::login_account(db, "tag-lobby-pilot"));
	return 0;
}

// The goodbyes a lobby's clean stop leaves (lobbydb::leave_goodbyes / list_goodbyes): every one
// listed by address with its keys, a key at the top of the u32 range kept whole; a later stop's
// goodbye at an address replaces the earlier one; one left past the window is not listed, and
// the next stop's write deletes it.
int test_goodbyes() {
	test_db::TestDatabase scratch;
	Database db = scratch.open();
	TEST_EXPECT(lobbydb::list_goodbyes(db, 240).empty());
	lobbydb::leave_goodbyes(db,
			{ { "10.7.1.2", 32768, 0xFFFFFFFFu, 0x5A5A0001u }, { "10.7.1.3", 2302, 0x11u, 0u } },
			240);
	auto left = lobbydb::list_goodbyes(db, 240);
	TEST_EXPECT(left.size() == 2);
	TEST_EXPECT(left[0].peer_ip == "10.7.1.2" && left[0].peer_port == 32768 &&
			left[0].server_sk == 0xFFFFFFFFu && left[0].client_ck == 0x5A5A0001u);
	TEST_EXPECT(left[1].peer_ip == "10.7.1.3" && left[1].peer_port == 2302 &&
			left[1].server_sk == 0x11u && left[1].client_ck == 0u);

	// Another stop, at one of the addresses: its goodbye replaces the earlier one.
	lobbydb::leave_goodbyes(db, { { "10.7.1.2", 32768, 0x22u, 0x5A5A0002u } }, 240);
	left = lobbydb::list_goodbyes(db, 240);
	TEST_EXPECT(left.size() == 2 && left[0].server_sk == 0x22u && left[0].client_ck == 0x5A5A0002u);

	// One left past the window: no longer listed, and the next stop's write deletes it (an empty
	// one too).
	db.exec("UPDATE lobby_goodbyes SET left_at = now() - interval '10 minutes' WHERE peer_ip = "
			"'10.7.1.3';");
	left = lobbydb::list_goodbyes(db, 240);
	TEST_EXPECT(left.size() == 1 && left[0].peer_ip == "10.7.1.2");
	TEST_EXPECT(db.query("SELECT 1 FROM lobby_goodbyes;").size() == 2);
	lobbydb::leave_goodbyes(db, {}, 240);
	TEST_EXPECT(db.query("SELECT 1 FROM lobby_goodbyes;").size() == 1);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	if (test_session_rows() != 0) return 1;
	if (test_login_account() != 0) return 1;
	if (test_pushes() != 0) return 1;
	if (test_end_session_and_forget() != 0) return 1;
	if (test_take_and_cascade() != 0) return 1;
	if (test_instances_and_rids() != 0)
		return 1;
	if (test_goodbyes() != 0)
		return 1;
	std::printf("OK: lobby repository (instances, RIDs, session rows, pushes, ends, forget_owner, "
				"take, cascade, clear_unheld, goodbyes)\n");
	return 0;
}
