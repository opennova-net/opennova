// Several threads on one database file, each on its own pooled connection, the
// way opennova-novaworld-server runs them (issue #981): registrations (the
// /api/register write, create_user) race host registrations through the NW UDP
// lobby dispatch (LobbySession: the host row and its roster in one
// transaction) and the gate's status-blob fold (apply_status_blob), while
// readers take the host row and its roster the way /api/hosts and the GSB feed
// do.
//
// Guards:
//   * every registration's id is the row it inserted (last_insert_rowid read
//     off the inserting connection), and no two registrations share an id;
//   * racing registrations of one username leave one account and answer the
//     rest username_exists, never a UNIQUE-violation db_error;
//   * a reader never sees a host's roster partial or empty, or beside a row
//     whose player count disagrees with it: every write replaces the whole
//     roster with one generation's slots and count, and every read snapshot
//     holds exactly one complete generation;
//   * no write fails (SQLITE_BUSY included: writers wait on the busy timeout),
//     and each host ends on its last generation.
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
#include <mutex>
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
constexpr int kGenerationsPerHost = 60;
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

// Generation g's roster has 1 + g % 5 slots, so a partial roster comes up short.
int roster_size(int generation) { return 1 + generation % 5; }

std::string slot_name(uint32_t rid, int generation, int slot) {
	return "h" + std::to_string(rid) + "-g" + std::to_string(generation) + "-s" +
	       std::to_string(slot);
}

std::string host_key(uint32_t rid) { return "HK-" + std::to_string(rid); }

// The generation a slot name carries, or -1.
int generation_of(const std::string &name) {
	const auto g = name.find("-g");
	const auto s = name.find("-s", g == std::string::npos ? 0 : g);
	if (g == std::string::npos || s == std::string::npos || s <= g + 2) return -1;
	int value = 0;
	for (std::size_t i = g + 2; i < s; ++i) {
		if (name[i] < '0' || name[i] > '9') return -1;
		value = value * 10 + (name[i] - '0');
	}
	return value;
}

opennova::NapiMessage make_host_request(uint32_t rid, int generation) {
	opennova::NapiMessage in;
	in.name = "ClientHostRequest";
	in.children.push_back(test_novaworld::make_client_var_list("HostSetup", {
		{"AppId", std::to_string(1000 + rid)},
		{"LobbyName", "jop_2_consumer"},
		{"MaxPlayers", "16"},
	}));
	in.children.push_back(test_novaworld::make_client_var_list("Host", {
		{"HostKey", host_key(rid)},
		{"ServerName", "Concurrency " + std::to_string(rid)},
		{"Players", std::to_string(roster_size(generation))},
		{"Port", "-1"},
	}));
	std::vector<test_novaworld::IndexedVar> players;
	for (int slot = 0; slot < roster_size(generation); ++slot) {
		for (auto &v : test_novaworld::player_slot_vars(slot, slot_name(rid, generation, slot),
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

// One host's writer: even generations re-register through the lobby dispatch
// (upsert_host, whose INSERT OR REPLACE cascades the old roster away, and the
// roster in one transaction); odd ones fold a status blob (update_host and the
// roster in one transaction).
void write_host(ConnectionPool &pool, uint32_t rid, const std::atomic<bool> &go) {
	try {
		auto conn = pool.acquire();
		opennova::LobbySession session;
		session.set_database(conn.get());
		session.set_rid_generator([rid] { return rid; });
		session.set_gsid_generator([rid](const std::string &) {
			return "GSID-" + std::to_string(rid);
		});
		opennova::LobbyState state;
		wait_for(go);
		for (int generation = 1; generation <= kGenerationsPerHost; ++generation) {
			if (generation % 2 == 0) {
				const auto result = session.dispatch(make_host_request(rid, generation), state,
				                                     "10.0.0.9", 40000);
				if (result.label != "ClientHostRequest") {
					g_failures.add("host " + std::to_string(rid) + ": dispatch " + result.label);
				}
			} else {
				opennova::LobbyStatusBlob blob;
				blob.lobby_name = "jop_2_consumer";
				blob.host_key = host_key(rid);
				blob.host_vars = {{"Players", std::to_string(roster_size(generation))}};
				for (int slot = 0; slot < roster_size(generation); ++slot) {
					blob.player_names.push_back(slot_name(rid, generation, slot));
				}
				if (!hostdb::apply_status_blob(*conn, blob)) {
					g_failures.add("host " + std::to_string(rid) + ": status blob unmatched");
				}
			}
		}
	} catch (const std::exception &e) {
		g_failures.add("host " + std::to_string(rid) + ": " + e.what());
	}
}

// Reads each host's row and roster in one snapshot until the writers finish.
void read_hosts(ConnectionPool &pool, const std::vector<uint32_t> &rids,
                const std::atomic<bool> &writing, std::atomic<int> &reads) {
	try {
		auto conn = pool.acquire();
		Database &db = *conn;
		while (writing.load()) {
			for (const uint32_t rid : rids) {
				// A deferred transaction is one WAL read snapshot for both reads.
				db.begin();
				const auto host = hostdb::find_host_by_rid(db, rid);
				const auto roster = hostdb::list_roster(db, rid);
				db.commit();
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
				const int generation = generation_of(roster.front().player_name);
				const int size = roster_size(generation);
				if (generation < 0 || static_cast<int>(roster.size()) != size) {
					g_failures.add(who + "partial roster: " + std::to_string(roster.size()) +
					               " slots, first '" + roster.front().player_name + "'");
					return;
				}
				for (int slot = 0; slot < size; ++slot) {
					if (roster[slot].slot != slot ||
					    roster[slot].player_name != slot_name(rid, generation, slot)) {
						g_failures.add(who + "mixed roster at slot " + std::to_string(slot) +
						               ": '" + roster[slot].player_name + "'");
						return;
					}
				}
				if (host->player_count != size) {
					g_failures.add(who + "player_count " + std::to_string(host->player_count) +
					               " beside a " + std::to_string(size) + "-slot roster");
					return;
				}
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
// registers its own users and reads each back by the id create_user returned.
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
		auto conn = pool.acquire();
		wait_for(go);
		{
			CreateUserParams p;
			p.username = "contested";
			p.password = "pw";
			p.pcid     = pcid(0xffff);
			p.nwhandle = "contested";
			const auto result = create_user(*conn, p);
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
		// Every host exists (generation 0) before a reader looks.
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
		writers.emplace_back(write_host, std::ref(pool), rid, std::cref(go));
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

	// Each host ends on its last generation, complete.
	for (const uint32_t rid : rids) {
		const auto roster = hostdb::list_roster(*check, rid);
		const int size = roster_size(kGenerationsPerHost);
		TEST_EXPECT(static_cast<int>(roster.size()) == size);
		for (int slot = 0; slot < size; ++slot) {
			TEST_EXPECT(roster[slot].player_name == slot_name(rid, kGenerationsPerHost, slot));
		}
		const auto host = hostdb::find_host_by_rid(*check, rid);
		TEST_EXPECT(host && host->player_count == size);
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
	std::printf("OK: per-thread connections keep ids and rosters consistent "
	            "(%d registrars, %d hosts x %d generations, %d readers)\n",
	            kRegistrars, kHosts, kGenerationsPerHost, kReaders);
	return 0;
}
