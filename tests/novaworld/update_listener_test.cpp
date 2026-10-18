// The update listener (apps/novaworld_web/updater/update_listener.h over
// release_catalog.h; ADR 0061 d3, ADR 0064) on a database of the run's own
// (common/test_database.h) and a local object store in a temp directory: a packed update
// built from the synthetic trees (tests/update/synthetic_trees.h through
// opennova-update-tool's builder) put in the store under its sha, a base data set (login
// SYNus, SYNPTR.CMD) and its expansion (SYNEus, SYNEPTR.CMD, a retail updater) with that
// package as their published release; the listener on a loopback port the OS picks,
// fetching and unpacking the package into its cache; the stock client's sequence
// (update_tool_test's test_serve: the gateway's LOGIN and SERVER.LST naming the update host,
// then the update server's LOGIN, the command file's CRC and the files, each answered from
// the cache); a login no published release serves refused; the release retired and the
// catalog refreshed, the login refused and the package dropped from the cache; a second
// release published, served after a refresh.

#include "core/local_object_store.h"
#include "hosted/data_set_repository.h"
#include "net_sockets.h"
#include "updater/update_listener.h"

#include "package_builder.h"

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <formats/update/server_list.h>
#include <formats/update/sfs.h>
#include <formats/update/update_crc.h>
#include <net/novaworld/db/database.h>
#include <net/update/update_wire.h>

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "update/synthetic_trees.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

using namespace opennova;
namespace nws = opennova::novaworld_server;
namespace hosted = opennova::novaworld_server::hosted;
namespace fs = std::filesystem;
using opennova::db::ConnectionPool;
using opennova::db::Database;

namespace {

constexpr const char *kUpdateHost = "update.test.example";

// One request sent, its replies read until `count` complete (as update_tool_test reads them).
bool exchange(net::Socket &socket, const std::string &request, size_t count,
		std::vector<update::Reply> &out) {
	if (!net::tcp_send_all(
				socket, reinterpret_cast<const uint8_t *>(request.data()), request.size()))
		return false;
	std::vector<uint8_t> stream;
	uint8_t buffer[4096];
	out.clear();
	while (out.size() < count) {
		update::Reply reply;
		size_t consumed = 0;
		if (!stream.empty() &&
				update::decode_reply(stream.data(), stream.size(), reply, consumed) ==
						update::Frame::Complete) {
			out.push_back(reply);
			stream.erase(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(consumed));
			continue;
		}
		const int n = net::tcp_recv(socket, buffer, sizeof buffer);
		if (n <= 0)
			return false;
		stream.insert(stream.end(), buffer, buffer + n);
	}
	return true;
}

net::Socket connect_to(uint16_t port) {
	net::Endpoint to;
	to.ip = { 127, 0, 0, 1 };
	to.port = port;
	return net::tcp_connect(to, 5000);
}

void close_after(net::Socket &socket) {
	if (!socket.is_valid())
		return;
	const std::string close = update::encode_close();
	net::tcp_send_all(socket, reinterpret_cast<const uint8_t *>(close.data()), close.size());
	net::close_socket(socket);
}

// The LOGIN reply's result for `login` on a fresh connection, as the update server (id -2).
int login_result(uint16_t port, const char *login) {
	net::Socket socket = connect_to(port);
	if (!socket.is_valid())
		return -99;
	std::vector<update::Reply> r;
	int result = -98;
	if (exchange(socket, update::encode_login(login, "", update::kIdServerLogin), 1, r) &&
			r[0].verb == update::Verb::Login)
		result = r[0].result;
	close_after(socket);
	return result;
}

const update::SfsEntry *entry_of(const update::SfsSection &section, const char *name) {
	for (const update::SfsEntry &e : section.entries)
		if (e.name == name)
			return &e;
	return nullptr;
}

std::string text_of(const std::vector<uint8_t> &bytes) {
	return std::string(bytes.begin(), bytes.end());
}

// A release of `sha` (its package at updates/<sha>.sfs already) with `sections`, published.
int64_t publish(Database &db, const std::string &sha, int64_t bytes,
		const std::vector<hosted::NewReleaseSection> &sections) {
	const int64_t id = hosted::create_release(db, "v" + sha.substr(0, 6), bytes, "test");
	if (!hosted::set_release_status(
				db, id, hosted::ReleaseStatus::Uploading, hosted::ReleaseStatus::Validating) ||
			!hosted::set_release_validated(db, id, sha, bytes, "Synthetic Product", sections) ||
			!hosted::publish_release(db, id))
		return 0;
	return id;
}

int run(const test_temp::TempDir &dir) {
	// The package, built from the synthetic trees, in the store under its sha.
	TEST_EXPECT(synthetic_trees::make_trees(dir));
	update_tool::BuildSpec spec;
	spec.from = io::os_path(dir.file("from"));
	spec.to = io::os_path(dir.file("to"));
	spec.package_product = "Synthetic Product";
	spec.products.push_back({ "", "", "SYNPTR.CMD", "", "", "" });
	spec.products.push_back({ "expansion/jox01", "jox01", "SYNEPTR.CMD", "", "", "" });
	update::SfsPackage package;
	std::vector<update_tool::ProductReport> report;
	std::string error;
	TEST_EXPECT(update_tool::build_update_package(spec, package, report, error));
	TEST_EXPECT(package.sections.size() == 2 && package.sections[1].expansion == "jox01");
	const std::vector<uint8_t> bytes = update::sfs_write(package);
	TEST_EXPECT(!bytes.empty());
	const std::string sha = io::sha256_hex(bytes.data(), bytes.size());
	const std::string package_path = dir.file("package.sfs");
	TEST_EXPECT(io::write_file_atomic(package_path, bytes.data(), bytes.size(), error));
	nws::LocalObjectStore store(dir.file("store"));
	TEST_EXPECT(store.put_file("updates/" + sha + ".sfs", package_path, error));

	// The rows: a base and its expansion, each an updater product, their published release.
	test_db::TestDatabase database;
	ConnectionPool pool(database.options());
	int64_t base = 0, expansion = 0, release = 0;
	{
		auto db = pool.acquire();
		hosted::NewDataSet b;
		b.display_name = "Synthetic";
		b.kind = hosted::DataSetKind::Base;
		b.title = "jo";
		b.game = "jop_2_consumer";
		b.updater_login = "SYNus";
		b.command_file = "SYNPTR.CMD";
		b.created_by = "test";
		TEST_EXPECT(hosted::create_data_set(*db, b, base) == hosted::DataSetOutcome::Ok);
		hosted::NewDataSet e;
		e.display_name = "Synthetic Expansion";
		e.kind = hosted::DataSetKind::Expansion;
		e.parent_id = base;
		e.expansion_code = "jox01";
		e.updater_login = "SYNEus";
		e.command_file = "SYNEPTR.CMD";
		e.retail_updater = true;
		e.created_by = "test";
		TEST_EXPECT(hosted::create_data_set(*db, e, expansion) == hosted::DataSetOutcome::Ok);
		release = publish(*db, sha, static_cast<int64_t>(bytes.size()),
				{ { base, "", "SYNPTR.CMD", "1.0.0.10", 4, 100 },
						{ expansion, "jox01", "SYNEPTR.CMD", "1.0.0.11", 1, 50 } });
		TEST_EXPECT(release != 0);
	}

	// The listener on loopback, the package fetched and both sections unpacked.
	nws::update_service::UpdateListenerConfig config;
	config.update_host = kUpdateHost;
	config.ports = { 0 };
	config.cache_dir = dir.file("cache");
	config.loopback_only = true;
	nws::update_service::UpdateListener listener(pool, store, config);
	TEST_EXPECT(listener.start());
	TEST_EXPECT(listener.ports().size() == 1 && listener.ports()[0] != 0);
	const uint16_t port = listener.ports()[0];
	TEST_EXPECT(listener.catalog().product_count() == 2);
	const std::string cache = dir.file("cache");
	TEST_EXPECT(fs::is_regular_file(io::os_path(io::utf8_join(cache, sha + ".sfs"))));
	TEST_EXPECT(fs::is_regular_file(io::os_path(io::utf8_join(cache, sha + "/0/SYNPTR.CMD"))));
	TEST_EXPECT(fs::is_regular_file(io::os_path(io::utf8_join(cache, sha + "/0/VERSION.TXT"))));
	TEST_EXPECT(fs::is_regular_file(io::os_path(io::utf8_join(cache, sha + "/1/SYNEPTR.CMD"))));

	// The gateway: LOGIN (id -4), SERVER.LST (id -3) naming the update host, CLOSE.
	{
		net::Socket socket = connect_to(port);
		TEST_EXPECT(socket.is_valid());
		std::vector<update::Reply> r;
		TEST_EXPECT(exchange(socket,
				update::encode_login("SYNus", "", update::kIdGatewayLogin) +
						update::encode_get(
								{ std::string(update::kServerListName) }, update::kIdServerList),
				2, r));
		TEST_EXPECT(r[0].verb == update::Verb::Login && r[0].id == -4 && r[0].result == 0);
		TEST_EXPECT(r[1].verb == update::Verb::Get && r[1].id == -3 && r[1].result == 0);
		TEST_EXPECT(update::read_server_list(text_of(r[1].payload)) ==
				std::vector<std::string>{ kUpdateHost });
		close_after(socket);
	}
	// The update server: LOGIN (id -2), CRC of the command file (id -1), the files by row id.
	{
		net::Socket socket = connect_to(port);
		TEST_EXPECT(socket.is_valid());
		std::vector<update::Reply> r;
		TEST_EXPECT(exchange(socket,
				update::encode_login("SYNus", "", update::kIdServerLogin) +
						update::encode_crc("SYNPTR.CMD", update::kIdCommandFile),
				2, r));
		TEST_EXPECT(r[0].verb == update::Verb::Login && r[0].result == 0);
		const update::SfsEntry *cmd = entry_of(package.sections[0], "SYNPTR.CMD");
		TEST_EXPECT(cmd != nullptr);
		TEST_EXPECT(r[1].verb == update::Verb::Crc && r[1].id == -1 &&
				r[1].crc ==
						update::crc_as_signed(
								update::command_file_crc(cmd->data.data(), cmd->data.size())));
		TEST_EXPECT(exchange(
				socket, update::encode_get({ "SYNPTR.CMD", "version.txt", "nope.txt" }, 4), 3, r));
		TEST_EXPECT(r[0].verb == update::Verb::Get && r[0].name == "SYNPTR.CMD" &&
				r[0].result == 0 && r[0].payload == cmd->data);
		const update::SfsEntry *version = entry_of(package.sections[0], "version.txt");
		TEST_EXPECT(version != nullptr && r[1].result == 0 && r[1].payload == version->data);
		TEST_EXPECT(r[2].name == "nope.txt" && r[2].result < 0);
		close_after(socket);
	}
	// The expansion's product: a retail updater, so SERVER.LST names ftp.novaworld.com.
	{
		net::Socket socket = connect_to(port);
		TEST_EXPECT(socket.is_valid());
		std::vector<update::Reply> r;
		TEST_EXPECT(exchange(socket,
				update::encode_login("SYNEus", "", update::kIdGatewayLogin) +
						update::encode_get(
								{ std::string(update::kServerListName) }, update::kIdServerList) +
						update::encode_get({ "SYNEPTR.CMD" }, 0),
				3, r));
		TEST_EXPECT(r[0].result == 0);
		TEST_EXPECT(update::read_server_list(text_of(r[1].payload)) ==
				std::vector<std::string>{ "ftp.novaworld.com" });
		const update::SfsEntry *cmd = entry_of(package.sections[1], "SYNEPTR.CMD");
		TEST_EXPECT(cmd != nullptr && r[2].result == 0 && r[2].payload == cmd->data);
		close_after(socket);
	}
	// A login no published release serves (the seeded JOTRus has none; an unknown name).
	TEST_EXPECT(login_result(port, "JOTRus") < 0);
	TEST_EXPECT(login_result(port, "NOPEus") < 0);
	// A login is matched without case, as retail's catalog matched products.
	TEST_EXPECT(login_result(port, "synus") == 0);

	// Retired: after a refresh the logins are refused and the package leaves the cache.
	{
		auto db = pool.acquire();
		TEST_EXPECT(hosted::retire_release(*db, release));
	}
	listener.refresh_now();
	TEST_EXPECT(listener.catalog().product_count() == 0);
	TEST_EXPECT(login_result(port, "SYNus") < 0 && login_result(port, "SYNEus") < 0);
	TEST_EXPECT(!fs::exists(io::os_path(io::utf8_join(cache, sha + ".sfs"))));
	TEST_EXPECT(!fs::exists(io::os_path(io::utf8_join(cache, sha))));

	// A second release (the same bytes under another sha: the catalog serves what the rows
	// name) for the base alone, served once refreshed and fetched again.
	const std::string sha2 = "ab" + sha.substr(2);
	TEST_EXPECT(store.put_file("updates/" + sha2 + ".sfs", package_path, error));
	{
		auto db = pool.acquire();
		TEST_EXPECT(publish(*db, sha2, static_cast<int64_t>(bytes.size()),
							{ { base, "", "SYNPTR.CMD", "1.0.0.10", 4, 100 } }) != 0);
	}
	listener.refresh_now();
	TEST_EXPECT(listener.catalog().product_count() == 1);
	TEST_EXPECT(login_result(port, "SYNus") == 0 && login_result(port, "SYNEus") < 0);
	TEST_EXPECT(fs::is_regular_file(io::os_path(io::utf8_join(cache, sha2 + "/0/SYNPTR.CMD"))));
	TEST_EXPECT(!fs::exists(io::os_path(io::utf8_join(cache, sha2 + "/1"))));
	listener.stop();
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	int rc = 1;
	{
		test_temp::TempDir dir("update_listener");
		rc = run(dir);
	}
	net::shutdown();
	if (rc == 0)
		std::printf("update_listener: ok\n");
	return rc;
}
