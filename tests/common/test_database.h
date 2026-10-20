#pragma once

// A PostgreSQL database of the test's own, per test case: the NovaWorld
// service's tests (tests/novaworld/service_tests.cmake) run against a real
// server, the one the CMake cache variable OPENNOVA_TEST_PG names, which ctest
// passes the test as `--pg <uri>` (opennova_add_pg_test). main() calls
// test_db::init(argc, argv) first: with no server it prints the SKIP line and
// exits 77, which ctest reports Skipped and the suite fails.
//
// Each TestDatabase creates nwt_<pid>_<n> on that server the way the service's
// own is made (TEMPLATE template0, SQL_ASCII, C collation: db::run_migrations
// checks), runs backend/migrations on it unless asked not to, and drops it
// WITH (FORCE) when destroyed, ending any connection still on it.

#include <net/novaworld/db/database.h>

#ifndef OPENNOVA_SOURCE_DIR
#error "test_database.h needs OPENNOVA_SOURCE_DIR (the repo root) for backend/migrations"
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace test_db {

inline std::string &server_slot() {
	static std::string url;
	return url;
}

// The server `--pg <uri>` names in argv; with none, the SKIP line and exit 77.
inline void init(int argc, char **argv) {
	for (int i = 1; i + 1 < argc; ++i) {
		if (std::string(argv[i]) == "--pg") server_slot() = argv[i + 1];
	}
	if (server_slot().empty()) {
		std::printf("SKIP: needs a PostgreSQL server (-DOPENNOVA_TEST_PG=<uri>)\n");
		std::fflush(stdout);
		std::exit(77);
	}
}

inline const std::string &server_url() { return server_slot(); }

// `url` (a postgresql:// URI) naming database `name` in place of its own.
inline std::string with_database(const std::string &url, const std::string &name) {
	const std::size_t scheme = url.find("://");
	const std::size_t authority = scheme == std::string::npos ? 0 : scheme + 3;
	const std::size_t path = url.find_first_of("/?", authority);
	if (path == std::string::npos) return url + "/" + name;
	if (url[path] == '?') return url.substr(0, path) + "/" + name + url.substr(path);
	const std::size_t query = url.find('?', path);
	return url.substr(0, path) + "/" + name + (query == std::string::npos ? "" : url.substr(query));
}

// Connection options for `url`, as a test's connections take them.
inline opennova::db::ConnectOptions options_for(const std::string &url) {
	opennova::db::ConnectOptions o;
	o.url = url;
	o.application_name = "opennova-test";
	return o;
}

inline std::string read_text(const std::filesystem::path &path) {
	std::ifstream in(path, std::ios::binary);
	std::ostringstream os;
	os << in.rdbuf();
	return os.str();
}

inline const std::filesystem::path &source_dir() {
	static const std::filesystem::path dir{ OPENNOVA_SOURCE_DIR };
	return dir;
}

struct Options {
	// Run backend/migrations on it (false: an empty database).
	bool migrate = true;
	// Then backend/seed/0001_games.sql (the games rows; no dev accounts).
	bool seed_games = false;
};

class TestDatabase {
public:
	TestDatabase() : TestDatabase(Options{}) {}

	explicit TestDatabase(const Options &options) {
		static std::atomic<int> next{ 0 };
#ifdef _WIN32
		const long pid = static_cast<long>(_getpid());
#else
		const long pid = static_cast<long>(getpid());
#endif
		name_ = "nwt_" + std::to_string(pid) + "_" + std::to_string(next++);
		conninfo_ = with_database(server_url(), name_);
		{
			opennova::db::Database admin(options_for(server_url()));
			// A run of an earlier process with this pid that died before its
			// drop left the name taken.
			admin.exec("DROP DATABASE IF EXISTS " + name_ + " WITH (FORCE);");
			admin.exec("CREATE DATABASE " + name_ +
					" TEMPLATE template0 ENCODING 'SQL_ASCII' LC_COLLATE 'C' LC_CTYPE 'C';");
		}
		if (options.migrate || options.seed_games) {
			opennova::db::Database db = open();
			if (options.migrate) opennova::db::run_migrations(db, source_dir() / "backend" / "migrations");
			if (options.seed_games)
				db.exec_script(read_text(source_dir() / "backend" / "seed" / "0001_games.sql"));
		}
	}

	~TestDatabase() {
		try {
			opennova::db::Database admin(options_for(server_url()));
			admin.exec("DROP DATABASE IF EXISTS " + name_ + " WITH (FORCE);");
		} catch (const std::exception &e) {
			std::fprintf(stderr, "test_database: dropping %s: %s\n", name_.c_str(), e.what());
		}
	}

	TestDatabase(const TestDatabase &) = delete;
	TestDatabase &operator=(const TestDatabase &) = delete;

	const std::string &name() const { return name_; }
	// The URI naming this database on the test server.
	const std::string &conninfo() const { return conninfo_; }
	opennova::db::ConnectOptions options() const { return options_for(conninfo_); }
	opennova::db::Database open() const { return opennova::db::Database(options()); }

private:
	std::string name_;
	std::string conninfo_;
};

} // namespace test_db
