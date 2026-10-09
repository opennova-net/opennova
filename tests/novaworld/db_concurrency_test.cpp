// Several threads on one database file and one db::ConnectionPool, the way
// opennova-novaworld-server runs them (issue #981). Registrations (the
// /api/register write, create_user, each on a connection leased for the call
// as a Crow handler leases one) race two writers per host: the NW UDP lobby
// dispatch (LobbySession re-sending ClientHostRequest: the host row and its
// roster in one transaction) and the gate's status-blob fold
// (apply_status_blob, a read-modify-write of the same row). Readers meanwhile
// take each host's row and roster in one db::ReadSnapshot, as /api/hosts and
// the GSB feed do.
//
// Guards:
//   * every registration's id is the row it inserted and no two share one,
//     which fails if the pool ever hands one connection to two holders (the
//     shared-handle bug itself is the HTTP route harness's concurrent case);
//   * racing registrations of one username leave one account and answer the
//     rest username_exists, never a UNIQUE-violation db_error;
//   * a read snapshot never holds a host's roster partial, empty or mixed, or
//     beside a row whose server name or player count belongs to another
//     write: every write replaces the whole roster and those columns with one
//     tagged generation's;
//   * a row never goes back in time: the UDP writer stamps MissionName with a
//     rising counter the status blob does not carry, so a blob fold that wrote
//     back a row it had read before a newer UDP write landed shows to a
//     reader as the counter falling;
//   * no write fails (SQLITE_BUSY included: writers wait on the busy timeout),
//     and each host ends on one of its writers' last generations.
//
// A file, not a shared-cache in-memory URI: shared cache locks per table and
// its SQLITE_LOCKED skips the busy timeout, and the production journal (WAL)
// needs a file.

#include "auth.h"

#include <base/io/log.h>
#include <net/novaworld/db/sqlite.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_session.h>
#include <net/novaworld/lobby_update.h>

#include "client_var_fixture.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using opennova::db::ConnectionPool;
using opennova::db::Database;
using opennova::novaworld_server::CreateUserParams;
using opennova::novaworld_server::create_user;
using opennova::novaworld_server::get_user_by_id;

namespace hostdb = opennova::hostdb;

namespace {

constexpr int kRegistrars = 3;
constexpr int kUsersPerRegistrar = 5;
constexpr int kHosts = 3;
constexpr int kGenerationsPerWriter = 50;
constexpr int kReaders = 3;

// Everything a worker thread reports, checked by main() after the join.
struct Failures {
	std::mutex mu;
	std::vector<std::string> lines;

	void add(std::string line) {
		std::lock_guard<std::mutex> lock(mu);
		lines.push_back(std::move(line));
	}
};

Failures g_failures;

// LobbySession logs a failed host write and carries on; any warning here is a
// write that did not land.
void record_warnings(opennova::io::LogLevel level, const char *message) {
	if (level >= opennova::io::LogLevel::kWarn) g_failures.add(message);
}

// A write's tag: its writer ('u' the UDP lobby, 'g' the gate) and generation.
std::string tag_of(char writer, int generation) {
	return std::string(1, writer) + std::to_string(generation);
}

// Generation g's roster has 1 + g % 5 slots, so a partial roster comes up short.
int roster_size(int generation) { return 1 + generation % 5; }

std::string slot_name(uint32_t rid, const std::string &tag, int slot) {
	return "h" + std::to_string(rid) + "-" + tag + "-s" + std::to_string(slot);
}

std::string server_name(const std::string &tag) { return "S-" + tag; }

std::string host_key(uint32_t rid) { return "HK-" + std::to_string(rid); }

// The decimal after `prefix` at the start of `text`, or -1.
int number_after(const std::string &text, const std::string &prefix) {
	if (text.size() <= prefix.size() || text.compare(0, prefix.size(), prefix) != 0) return -1;
	int value = 0;
	for (std::size_t i = prefix.size(); i < text.size(); ++i) {
		if (text[i] < '0' || text[i] > '9') return -1;
		value = value * 10 + (text[i] - '0');
	}
	return value;
}

// The tag a slot name carries ("h<rid>-<tag>-s<slot>"), or "".
std::string tag_in(uint32_t rid, const std::string &name) {
	const std::string head = "h" + std::to_string(rid) + "-";
	const auto end = name.find("-s", head.size());
	if (name.compare(0, head.size(), head) != 0 || end == std::string::npos) return {};
	return name.substr(head.size(), end - head.size());
}

opennova::NapiMessage make_host_request(uint32_t rid, int generation) {
	const std::string tag = tag_of('u', generation);
	opennova::NapiMessage in;
	in.name = "ClientHostRequest";
	in.children.push_back(test_novaworld::make_client_var_list("HostSetup", {
		{"AppId", std::to_string(1000 + rid)},
		{"LobbyName", "jop_2_consumer"},
		{"MaxPlayers", "16"},
	}));
	in.children.push_back(test_novaworld::make_client_var_list("Host", {
		{"HostKey", host_key(rid)},
		{"ServerName", server_name(tag)},
		{"MissionName", "m" + std::to_string(generation)},
		{"Players", std::to_string(roster_size(generation))},
		{"Port", "-1"},
	}));
	std::vector<test_novaworld::IndexedVar> players;
	for (int slot = 0; slot < roster_size(generation); ++slot) {
		for (auto &v : test_novaworld::player_slot_vars(slot, slot_name(rid, tag, slot),
		                                                "10.0.0.9:40000", "00000002", "1", "0")) {
			players.push_back(std::move(v));
		}
	}
	in.children.push_back(test_novaworld::make_indexed_var_list("PlayerList", players));
	return in;
}

void wait_for(const std::atomic<bool> &go) {
	while (!go.load()) std::this_thread::yield();
}

// A worker's lease, its busy timeout raised from the server's 5 s to 60 s: on
// a loaded CI runner one writer can queue behind every other writer's WAL fsync
// for longer than 5 s, and how long a write waits is not what this test asks.
ConnectionPool::Lease lease(ConnectionPool &pool) {
	auto conn = pool.acquire();
	conn->exec("PRAGMA busy_timeout = 60000;");
	return conn;
}

// The NW UDP side of one host: re-sends ClientHostRequest (upsert_host and the
// roster in one transaction), each generation with a higher MissionName.
void write_host_udp(ConnectionPool &pool, uint32_t rid, const std::atomic<bool> &go) {
	try {
		auto conn = lease(pool);
		opennova::LobbySession session;
		session.set_database(conn.get());
		session.set_rid_generator([rid] { return rid; });
		session.set_gsid_generator([rid](const std::string &) {
			return "GSID-" + std::to_string(rid);
		});
		opennova::LobbyState state;
		wait_for(go);
		for (int generation = 1; generation <= kGenerationsPerWriter; ++generation) {
			const auto result = session.dispatch(make_host_request(rid, generation), state,
			                                     "10.0.0.9", 40000);
			if (result.label != "ClientHostRequest") {
				g_failures.add("udp " + std::to_string(rid) + ": dispatch " + result.label);
			}
		}
	} catch (const std::exception &e) {
		g_failures.add("udp " + std::to_string(rid) + ": " + e.what());
	}
}

// The gate side of the same host: folds status blobs (read the row, take the
// blob's columns, write it and the roster back), never carrying MissionName.
void write_host_gate(ConnectionPool &pool, uint32_t rid, const std::atomic<bool> &go) {
	try {
		auto conn = lease(pool);
		wait_for(go);
		for (int generation = 1; generation <= kGenerationsPerWriter; ++generation) {
			const std::string tag = tag_of('g', generation);
			opennova::LobbyStatusBlob blob;
			blob.lobby_name = "jop_2_consumer";
			blob.host_key = host_key(rid);
			blob.host_vars = {{"ServerName", server_name(tag)},
			                  {"Players", std::to_string(roster_size(generation))}};
			for (int slot = 0; slot < roster_size(generation); ++slot) {
				blob.player_names.push_back(slot_name(rid, tag, slot));
			}
			if (!hostdb::apply_status_blob(*conn, blob)) {
				g_failures.add("gate " + std::to_string(rid) + ": status blob unmatched");
			}
		}
	} catch (const std::exception &e) {
		g_failures.add("gate " + std::to_string(rid) + ": " + e.what());
	}
}

// Takes each host's row and roster in one snapshot until the writers finish.
void read_hosts(ConnectionPool &pool, const std::vector<uint32_t> &rids,
                const std::atomic<bool> &writing, std::atomic<int> &reads) {
	try {
		auto conn = lease(pool);
		Database &db = *conn;
		std::map<uint32_t, int> last_mission;
		while (writing.load()) {
			for (const uint32_t rid : rids) {
				std::optional<hostdb::HostRow> host;
				std::vector<opennova::HostRosterSlot> roster;
				{
					opennova::db::ReadSnapshot snapshot(db);
					host = hostdb::find_host_by_rid(db, rid);
					roster = hostdb::list_roster(db, rid);
				}
				reads.fetch_add(1);

				const std::string who = "reader host " + std::to_string(rid) + ": ";
				if (!host) {
					g_failures.add(who + "row missing");
					return;
				}
				if (roster.empty()) {
					g_failures.add(who + "empty roster");
					return;
				}
				const std::string tag = tag_in(rid, roster.front().player_name);
				const int generation = tag.empty() ? -1 : number_after(tag, tag.substr(0, 1));
				if (generation < 0 || static_cast<int>(roster.size()) != roster_size(generation)) {
					g_failures.add(who + "partial roster: " + std::to_string(roster.size()) +
					               " slots, first '" + roster.front().player_name + "'");
					return;
				}
				for (int slot = 0; slot < static_cast<int>(roster.size()); ++slot) {
					if (roster[slot].slot != slot ||
					    roster[slot].player_name != slot_name(rid, tag, slot)) {
						g_failures.add(who + "mixed roster at slot " + std::to_string(slot) +
						               ": '" + roster[slot].player_name + "'");
						return;
					}
				}
				if (host->server_name != server_name(tag) ||
				    host->player_count != static_cast<int>(roster.size())) {
					g_failures.add(who + "row '" + host->server_name + "' players " +
					               std::to_string(host->player_count) + " beside roster " + tag);
					return;
				}
				const int mission = number_after(host->mission_name, "m");
				if (mission < 0 || mission < last_mission[rid]) {
					g_failures.add(who + "row went back: mission '" + host->mission_name +
					               "' after m" + std::to_string(last_mission[rid]));
					return;
				}
				last_mission[rid] = mission;
			}
		}
	} catch (const std::exception &e) {
		g_failures.add(std::string("reader: ") + e.what());
	}
}

struct Registration {
	std::string username;
	int64_t id = 0;
};

// One registrar: first races every other registrar for one username, then
// registers its own users. Each registration leases its connection for the
// call and reads the account back by the id create_user returned.
void register_users(ConnectionPool &pool, int registrar, const std::atomic<bool> &go,
                    std::mutex &registered_mu, std::vector<Registration> &registered,
                    std::atomic<int> &contested_ok, std::atomic<int> &contested_taken) {
	auto pcid = [registrar](int n) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%04x%04x", static_cast<unsigned>(registrar),
		              static_cast<unsigned>(n));
		return std::string(buf);
	};
	const std::string who = "registrar " + std::to_string(registrar) + ": ";
	try {
		wait_for(go);
		{
			CreateUserParams p;
			p.username = "contested";
			p.password = "pw";
			p.pcid     = pcid(0xffff);
			p.nwhandle = "contested";
			const auto result = create_user(*lease(pool), p);
			if (result.ok) {
				contested_ok.fetch_add(1);
			} else if (result.error_code == "username_exists") {
				contested_taken.fetch_add(1);
			} else {
				g_failures.add(who + "contested: " + result.error_code + " " +
				               result.error_message);
			}
		}
		for (int n = 0; n < kUsersPerRegistrar; ++n) {
			CreateUserParams p;
			p.username = "r" + std::to_string(registrar) + "-u" + std::to_string(n);
			p.password = "pw";
			p.pcid     = pcid(n);
			p.nwhandle = p.username;
			auto conn = lease(pool);
			const auto result = create_user(*conn, p);
			if (!result.ok) {
				g_failures.add(who + p.username + ": " + result.error_code + " " +
				               result.error_message);
				continue;
			}
			const auto user = get_user_by_id(*conn, result.id);
			if (!user || user->username != p.username) {
				g_failures.add(who + p.username + " got id " + std::to_string(result.id) +
				               ", which is '" + (user ? user->username : "(none)") + "'");
			}
			std::lock_guard<std::mutex> lock(registered_mu);
			registered.push_back({p.username, result.id});
		}
	} catch (const std::exception &e) {
		g_failures.add(who + e.what());
	}
}

int run(const std::filesystem::path &db_path) {
	ConnectionPool pool(db_path);
	std::vector<uint32_t> rids;
	{
		auto boot = pool.acquire();
		opennova::db::run_migrations(*boot, OPENNOVA_SOURCE_DIR "/backend/migrations");
		// Every host exists (UDP generation 0) before a reader looks.
		for (int h = 0; h < kHosts; ++h) {
			const uint32_t rid = 7000 + static_cast<uint32_t>(h);
			opennova::LobbySession session;
			session.set_database(boot.get());
			session.set_rid_generator([rid] { return rid; });
			opennova::LobbyState state;
			const auto result = session.dispatch(make_host_request(rid, 0), state, "10.0.0.9", 40000);
			TEST_EXPECT(result.label == "ClientHostRequest");
			rids.push_back(rid);
		}
	}
	TEST_EXPECT(g_failures.lines.empty());

	std::atomic<bool> go{false};
	std::atomic<bool> writing{true};
	std::atomic<int> reads{0};
	std::mutex registered_mu;
	std::vector<Registration> registered;
	std::atomic<int> contested_ok{0};
	std::atomic<int> contested_taken{0};

	std::vector<std::thread> writers;
	for (const uint32_t rid : rids) {
		writers.emplace_back(write_host_udp, std::ref(pool), rid, std::cref(go));
		writers.emplace_back(write_host_gate, std::ref(pool), rid, std::cref(go));
	}
	for (int r = 0; r < kRegistrars; ++r) {
		writers.emplace_back(register_users, std::ref(pool), r, std::cref(go),
		                     std::ref(registered_mu), std::ref(registered),
		                     std::ref(contested_ok), std::ref(contested_taken));
	}
	std::vector<std::thread> readers;
	for (int r = 0; r < kReaders; ++r) {
		readers.emplace_back(read_hosts, std::ref(pool), std::cref(rids), std::cref(writing),
		                     std::ref(reads));
	}
	go.store(true);
	for (auto &t : writers) t.join();
	writing.store(false);
	for (auto &t : readers) t.join();

	for (const auto &line : g_failures.lines) {
		std::fprintf(stderr, "db_concurrency: %s\n", line.c_str());
	}
	TEST_EXPECT(g_failures.lines.empty());
	TEST_EXPECT(reads.load() > 0);

	// One account for the contested name; every other racer told it is taken.
	TEST_EXPECT(contested_ok.load() == 1);
	TEST_EXPECT(contested_taken.load() == kRegistrars - 1);

	// Every registration's id is its own row, and no two share one.
	TEST_EXPECT(registered.size() == static_cast<std::size_t>(kRegistrars * kUsersPerRegistrar));
	auto check = pool.acquire();
	std::set<int64_t> ids;
	for (const auto &r : registered) {
		TEST_EXPECT(ids.insert(r.id).second);
		const auto user = get_user_by_id(*check, r.id);
		TEST_EXPECT(user && user->username == r.username);
	}

	// Each host ends on one writer's last generation, complete, and on the UDP
	// writer's last MissionName (no blob fold after it wrote back an older one).
	for (const uint32_t rid : rids) {
		const auto roster = hostdb::list_roster(*check, rid);
		const auto host = hostdb::find_host_by_rid(*check, rid);
		TEST_EXPECT(host.has_value() && !roster.empty());
		const std::string tag = tag_in(rid, roster.front().player_name);
		TEST_EXPECT(tag == tag_of('u', kGenerationsPerWriter) ||
		            tag == tag_of('g', kGenerationsPerWriter));
		const int size = roster_size(kGenerationsPerWriter);
		TEST_EXPECT(static_cast<int>(roster.size()) == size);
		for (int slot = 0; slot < size; ++slot) {
			TEST_EXPECT(roster[slot].player_name == slot_name(rid, tag, slot));
		}
		TEST_EXPECT(host->server_name == server_name(tag) && host->player_count == size);
		TEST_EXPECT(host->mission_name == "m" + std::to_string(kGenerationsPerWriter));
	}
	return 0;
}

} // namespace

int main() {
	opennova::io::set_log_sink(&record_warnings);
	auto dir = std::filesystem::temp_directory_path() /
	           test_paths_unique("opennova_db_concurrency_test");
	std::random_device rd;
	dir /= std::to_string(std::mt19937_64(rd())());
	std::filesystem::create_directories(dir);

	// run() closes every connection (the pool and its leases) before the
	// directory goes: Windows will not delete an open database's WAL/SHM files.
	const int rc = run(dir / "novaworld.db");
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	if (rc != 0) return rc;
	std::printf("OK: pooled connections keep ids, rosters and rows consistent "
	            "(%d registrars, %d hosts x 2 writers x %d generations, %d readers)\n",
	            kRegistrars, kHosts, kGenerationsPerWriter, kReaders);
	return 0;
}
