#pragma once

// A Redis namespace of the test's own: the NovaWorld server's HTTP-layer tests
// (tests/novaworld/service_tests.cmake) run against a real server (Valkey), the
// one the CMake cache variable OPENNOVA_TEST_REDIS names, which ctest passes the
// test as `--redis <url>` (opennova_add_redis_test, or opennova_add_pg_test's
// REDIS). main() calls test_redis::init(argc, argv) first: with no server it
// prints the SKIP line and exits 77, which ctest reports Skipped and the core
// suite fails.
//
// The tests share the server and run at once, so each TestRedis is a pool
// whose keys all start with a prefix of its own (nwt_<pid>_<n>:), the way
// test_database.h makes a database per case; destroying it deletes its keys.
// One per listener gives each listener the state a service of its own would
// have (its own rate limiters' buckets); two over one prefix are two web
// replicas of one service.

#include "redis_client.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace test_redis {

inline std::string &server_slot() {
	static std::string url;
	return url;
}

// The server `--redis <url>` names in argv; with none, the SKIP line and exit 77.
inline void init(int argc, char **argv) {
	for (int i = 1; i + 1 < argc; ++i) {
		if (std::string(argv[i]) == "--redis")
			server_slot() = argv[i + 1];
	}
	if (server_slot().empty()) {
		std::printf("SKIP: needs a Redis server (-DOPENNOVA_TEST_REDIS=<url>)\n");
		std::fflush(stdout);
		std::exit(77);
	}
#ifdef SIGPIPE
	// As the server's main() does: a write to a connection the server closed
	// fails with EPIPE (the client reconnects) instead of ending the test.
	std::signal(SIGPIPE, SIG_IGN);
#endif
}

inline const std::string &server_url() {
	return server_slot();
}

// A key prefix no other TestRedis, here or in another test process, uses.
inline std::string unique_prefix() {
	static std::atomic<int> next{ 0 };
#ifdef _WIN32
	const long pid = static_cast<long>(_getpid());
#else
	const long pid = static_cast<long>(getpid());
#endif
	return "nwt_" + std::to_string(pid) + "_" + std::to_string(next++) + ":";
}

inline opennova::novaworld_server::RedisOptions options_for(const std::string &prefix) {
	opennova::novaworld_server::RedisOptions o;
	o.url = server_url();
	o.key_prefix = prefix;
	return o;
}

class TestRedis {
public:
	TestRedis() : TestRedis(unique_prefix()) {}
	// Another pool over `prefix`: a second replica of the service whose
	// TestRedis made it (no cleanup of its own; the first one's covers both).
	explicit TestRedis(const std::string &prefix, bool owns = true) :
			prefix_(prefix), owns_(owns), pool_(options_for(prefix)) {}

	~TestRedis() {
		if (!owns_)
			return;
		try {
			std::string cursor = "0";
			do {
				const auto page =
						pool_.command({ "SCAN", cursor, "MATCH", prefix_ + "*", "COUNT", "1000" });
				cursor = page.elements.at(0).string;
				for (const auto &key : page.elements.at(1).elements)
					pool_.command({ "DEL", key.string });
			} while (cursor != "0");
		} catch (const std::exception &e) {
			std::fprintf(stderr, "test_redis: deleting %s*: %s\n", prefix_.c_str(), e.what());
		}
	}

	TestRedis(const TestRedis &) = delete;
	TestRedis &operator=(const TestRedis &) = delete;

	opennova::novaworld_server::RedisPool &pool() { return pool_; }
	const std::string &prefix() const { return prefix_; }

private:
	std::string prefix_;
	bool owns_;
	opennova::novaworld_server::RedisPool pool_;
};

} // namespace test_redis
