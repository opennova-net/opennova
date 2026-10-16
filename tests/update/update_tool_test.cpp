// opennova-update-tool's core (apps/update_tool): a packed update built from two synthetic
// install trees (a base product with a loose version.txt and a .pff, a jox01 expansion product),
// its rows and blobs checked, what the updater could not install refused; then the package served
// on a loopback port and fetched the way the stock client fetches it
// (docs/update/updater-re.md §3-§9, ADR 0061 d4).

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include "package_builder.h"
#include "package_catalog.h"
#include "update_tcp_server.h"

#include <base/io/file_io.h>
#include <formats/pff/pff.h>
#include <formats/update/command_file.h>
#include <formats/update/nlz.h>
#include <formats/update/update_crc.h>
#include <net/update/update_wire.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace opennova;
namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> bytes(std::string_view text) {
	return std::vector<uint8_t>(text.begin(), text.end());
}

bool write_text(const std::string &path, std::string_view text) {
	std::string error;
	fs::create_directories(fs::path(io::os_path(path)).parent_path());
	return io::write_file_atomic(path, text.data(), text.size(), error);
}

bool make_archive(const std::string &path, const std::vector<pff::PffEditEntry> &entries) {
	fs::create_directories(fs::path(io::os_path(path)).parent_path());
	bool rewritten = false;
	return pff::pff_apply_edits(path.c_str(), entries, {}, &rewritten) == pff::PFF_WRITE_OK;
}

// The baseline and the tree to update it to.
bool make_trees(const test_temp::TempDir &dir) {
	const std::string from = dir.file("from"), to = dir.file("to");
	return write_text(from + "/version.txt", "Synthetic Product, V1.0.0.0; English") &&
			write_text(from + "/old.txt", "gone in the update") &&
			write_text(from + "/same.txt", "unchanged") &&
			write_text(from + "/update.log", "the updater's own file") &&
			make_archive(from + "/res.pff",
					{ { "keep.def", bytes("kept"), 1100000000 },
							{ "wpn.def", bytes("rifle 1"), 1100000000 },
							{ "drop.def", bytes("x"), 1100000000 } }) &&
			write_text(from + "/expansion/jox01/version.txt",
					"Synthetic Expansion, V1.0.0.0; English") &&
			write_text(to + "/version.txt", "Synthetic Product, V1.0.0.10; English") &&
			write_text(to + "/same.txt", "unchanged") && write_text(to + "/new.txt", "added") &&
			write_text(to + "/update.log", "a different log, never shipped") &&
			make_archive(to + "/res.pff",
					{ { "keep.def", bytes("kept"), 1100000000 },
							{ "wpn.def", bytes("rifle 2"), 1700000000 },
							{ "add.def", bytes("new"), 1700000001 } }) &&
			write_text(
					to + "/expansion/jox01/version.txt", "Synthetic Expansion, V1.0.0.11; English");
}

const update::CommandRow *row(const update::CommandFile &cmd, const std::string &name) {
	for (const update::CommandRow &r : cmd.rows)
		if (r.name == name)
			return &r;
	return nullptr;
}

int test_build(const test_temp::TempDir &dir, update::SfsPackage &package) {
	update_tool::BuildSpec spec;
	spec.from = io::os_path(dir.file("from"));
	spec.to = io::os_path(dir.file("to"));
	spec.package_product = "Synthetic Product";
	spec.products.push_back({ "", "", "SYNPTR.CMD", "", "", "" });
	spec.products.push_back({ "expansion/jox01", "jox01", "SYNEPTR.CMD", "", "", "" });
	std::vector<update_tool::ProductReport> report;
	std::string error;
	TEST_EXPECT(update_tool::build_update_package(spec, package, report, error));
	if (!error.empty())
		std::fprintf(stderr, "%s\n", error.c_str());
	TEST_EXPECT(package.sections.size() == 2 && package.sections[1].expansion == "jox01");
	TEST_EXPECT(
			report[0].loose_rows == 2 && report[0].copy_rows == 2 && report[0].remove_rows == 2);

	const update::SfsSection &base = package.sections[0];
	TEST_EXPECT(base.entries[0].name == "SYNPTR.CMD");
	const std::optional<update::CommandFile> cmd = update::read_command_file(
			std::string(base.entries[0].data.begin(), base.entries[0].data.end()));
	TEST_EXPECT(cmd.has_value());
	if (!cmd)
		return 1;
	// The header from version.txt; the loose rows and removes at the root, then the archive's.
	TEST_EXPECT(cmd->product == "Synthetic Product" && cmd->version == "1.0.0.10");
	TEST_EXPECT(row(*cmd, "same.txt") == nullptr && row(*cmd, "update.log") == nullptr);
	const update::CommandRow *version = row(*cmd, "version.txt");
	TEST_EXPECT(version && version->type == update::CommandType::Loose);
	const std::vector<uint8_t> version_text = bytes("Synthetic Product, V1.0.0.10; English");
	TEST_EXPECT(version &&
			version->crc ==
					update::crc_as_signed(
							update::file_crc(version_text.data(), version_text.size())));
	TEST_EXPECT(row(*cmd, "old.txt") && row(*cmd, "old.txt")->type == update::CommandType::Remove);
	const update::CommandRow *target = row(*cmd, "res.pff");
	TEST_EXPECT(target && target->type == update::CommandType::Target);
	const update::CommandRow *wpn = row(*cmd, "wpn.def");
	TEST_EXPECT(wpn && wpn->type == update::CommandType::Copy && wpn->time == 1700000000 &&
			wpn->size == 7);
	TEST_EXPECT(row(*cmd, "keep.def") == nullptr);
	TEST_EXPECT(
			row(*cmd, "drop.def") && row(*cmd, "drop.def")->type == update::CommandType::Remove);
	// Each UPD / COPY row has its blob, which inflates to the row's size.
	for (const update::CommandRow &r : cmd->rows) {
		if (r.type != update::CommandType::Loose && r.type != update::CommandType::Copy)
			continue;
		bool found = false;
		for (const update::SfsEntry &entry : base.entries) {
			if (entry.name != r.name)
				continue;
			std::vector<uint8_t> content;
			found = update::nlz_decode(entry.data.data(), entry.data.size(), content) ==
							update::NlzDecode::Inflated &&
					int32_t(content.size()) == r.size;
		}
		TEST_EXPECT(found);
	}

	// A change the stock precheck cannot see (same size, same defective CRC): refused.
	update::SfsPackage refused;
	TEST_EXPECT(
			write_text(dir.file("to") + "/version.txt", "Synthetic Product, V1.0.0.9; English"));
	TEST_EXPECT(!update_tool::build_update_package(spec, refused, report, error));
	TEST_EXPECT(error.find("cannot see") != std::string::npos);
	TEST_EXPECT(
			write_text(dir.file("to") + "/version.txt", "Synthetic Product, V1.0.0.10; English"));

	// A changed file below the product's root, two files of one name: refused.
	TEST_EXPECT(write_text(dir.file("to") + "/sub/deep.txt", "nested"));
	TEST_EXPECT(!update_tool::build_update_package(spec, refused, report, error));
	TEST_EXPECT(error.find("root only") != std::string::npos);
	spec.excludes.push_back("sub/");
	TEST_EXPECT(write_text(dir.file("to") + "/add.def", "collides with the archive's"));
	TEST_EXPECT(!update_tool::build_update_package(spec, refused, report, error));
	TEST_EXPECT(error.find("two files") != std::string::npos);
	std::error_code ec;
	fs::remove(io::os_path(dir.file("to") + "/add.def"), ec);

	// B-12: an empty loose file is refused (the stock precheck fetches an empty file on every
	// pass, so the update would loop UPDATE -> REVUPDAT forever).
	// [orig: Cupdate2Dlg_PrecheckCommands @ 0x407970..0x4079B9 (UPDATE.EXE)]
	TEST_EXPECT(write_text(dir.file("to") + "/empty.txt", ""));
	TEST_EXPECT(!update_tool::build_update_package(spec, refused, report, error));
	TEST_EXPECT(error.find("empty") != std::string::npos);
	fs::remove(io::os_path(dir.file("to") + "/empty.txt"), ec);

	// B-25: an archive the baseline has and `to` lacks gets a REMOVE row at the root, ahead of
	// every TARGET row.
	TEST_EXPECT(make_archive(dir.file("from") + "/old.pff", { { "gone.def", bytes("x"), 1100000000 } }));
	update::SfsPackage removed;
	TEST_EXPECT(update_tool::build_update_package(spec, removed, report, error));
	TEST_EXPECT(report[0].remove_rows == 3);
	const update::SfsSection &removed_base = removed.sections[0];
	const std::optional<update::CommandFile> removed_cmd = update::read_command_file(std::string(
			removed_base.entries[0].data.begin(), removed_base.entries[0].data.end()));
	TEST_EXPECT(removed_cmd.has_value());
	if (!removed_cmd)
		return 1;
	bool before_target = true, archive_removed = false;
	for (const update::CommandRow &r : removed_cmd->rows) {
		if (r.type == update::CommandType::Target)
			before_target = false;
		if (r.name == "old.pff")
			archive_removed = r.type == update::CommandType::Remove && before_target;
	}
	TEST_EXPECT(archive_removed);
	fs::remove(io::os_path(dir.file("from") + "/old.pff"), ec);
	return 0;
}

// One request sent, its replies read until `count` complete.
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

int test_serve(const update::SfsPackage &package) {
	update_tool::PackageCatalog catalog({ "ftp.novaworld.com" });
	std::string error;
	TEST_EXPECT(catalog.bind("SYNus", "SYNPTR.CMD", { package }, error));
	TEST_EXPECT(catalog.bind("SYNEus", "SYNEPTR.CMD", { package }, error));
	TEST_EXPECT(!catalog.bind("NOPEus", "NOPE.CMD", { package }, error));
	TEST_EXPECT(net::startup() == 0);
	update::UpdateServer server(catalog);
	update_tool::UpdateTcpServer listener(server);
	TEST_EXPECT(listener.listen({ 0 }, true));
	if (listener.ports().empty())
		return 1;
	std::atomic<bool> stop{ false };
	std::thread pump([&] {
		while (!stop)
			if (!listener.pump())
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
	});
	net::Endpoint to;
	to.ip = { 127, 0, 0, 1 };
	to.port = listener.ports()[0];
	net::Socket socket = net::tcp_connect(to, 5000);
	int rc = 0;
	std::vector<update::Reply> r;
	if (!socket.is_valid() ||
			!exchange(socket,
					update::encode_login("SYNEus", "", update::kIdServerLogin) +
							update::encode_crc("SYNEPTR.CMD", update::kIdCommandFile),
					2, r) ||
			r[0].result != 0 || r[1].verb != update::Verb::Crc) {
		std::fprintf(stderr, "update_tool: the expansion login or CRC failed\n");
		rc = 1;
	}
	if (rc == 0 &&
			(!exchange(socket, update::encode_get({ "SYNEPTR.CMD", "version.txt" }, 4), 2, r) ||
					r[0].payload != package.sections[1].entries[0].data || r[1].result != 0)) {
		std::fprintf(stderr, "update_tool: the expansion's files did not come back as packed\n");
		rc = 1;
	}
	if (socket.is_valid()) {
		const std::string close = update::encode_close();
		net::tcp_send_all(socket, reinterpret_cast<const uint8_t *>(close.data()), close.size());
		net::close_socket(socket);
	}
	stop = true;
	pump.join();
	listener.close();
	net::shutdown();
	return rc;
}

} // namespace

int main() {
	test_temp::TempDir dir("update_tool");
	if (!make_trees(dir)) {
		std::fprintf(stderr, "update_tool: cannot write the synthetic trees\n");
		return 1;
	}
	update::SfsPackage package;
	if (int rc = test_build(dir, package))
		return rc;
	if (int rc = test_serve(package))
		return rc;
	std::printf("update_tool: ok\n");
	return 0;
}
