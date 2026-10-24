// opennova-update-tool's core (apps/update_tool): a packed update built from two synthetic
// install trees (a base product with a loose version.txt and a .pff, a jox01 expansion product),
// its rows and blobs checked, what the updater could not install refused; then the package served
// on a loopback port and fetched the way the stock client fetches it
// (docs/update/updater-re.md §3-§9, ADR 0061 d4).

#include "common/temp_dir.h"
#include "common/test_expect.h"
#include "update/synthetic_trees.h"

#include "package_builder.h"
#include "package_catalog.h"
#include "update_tcp_server.h"

#include <base/io/file_io.h>
#include <formats/pff/pff.h>
#include <formats/update/command_file.h>
#include <formats/update/apply.h>
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

using synthetic_trees::bytes;
using synthetic_trees::make_archive;
using synthetic_trees::write_text;

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

int test_history(const test_temp::TempDir &dir) {
	const std::string base = dir.file("history-base"), first = dir.file("history-first"), latest = dir.file("history-latest");
	TEST_EXPECT(write_text(base + "/state.txt", "baseline value"));
	TEST_EXPECT(make_archive(base + "/res.pff", {{"base.def", bytes("baseline entry"), 1700000000}}));
	TEST_EXPECT(write_text(first + "/state.txt", "first release value"));
	TEST_EXPECT(write_text(first + "/temporary.txt", "removed next release"));
	TEST_EXPECT(make_archive(first + "/res.pff", {{"base.def", bytes("changed entry"), 1700000001},
			{"temp.def", bytes("removed next release"), 1700000001}}));
	TEST_EXPECT(write_text(latest + "/state.txt", "baseline value"));
	TEST_EXPECT(make_archive(latest + "/res.pff", {{"base.def", bytes("baseline entry"), 1700000000}}));
	update_tool::BuildSpec spec;
	spec.from = io::os_path(base);
	spec.to = io::os_path(first);
	spec.full = true;
	spec.products.push_back({"", "", "HISTORY.CMD", "Synthetic history", "1", ""});
	std::vector<update_tool::ProductReport> report;
	std::string error;
	update::SfsPackage p1, p2, p3;
	TEST_EXPECT(update_tool::build_update_package(spec, p1, report, error));
	spec.to = io::os_path(latest);
	spec.previous = p1;
	spec.products[0].version = "2";
	TEST_EXPECT(update_tool::build_update_package(spec, p2, report, error));
	spec.previous = p2;
	spec.products[0].version = "3";
	TEST_EXPECT(update_tool::build_update_package(spec, p3, report, error));
	const auto cmd = update::read_command_file(std::string(p3.sections[0].entries[0].data.begin(), p3.sections[0].entries[0].data.end()));
	TEST_EXPECT(cmd && row(*cmd, "state.txt") && row(*cmd, "base.def"));
	TEST_EXPECT(row(*cmd, "temporary.txt") && row(*cmd, "temporary.txt")->type == update::CommandType::Remove);
	TEST_EXPECT(row(*cmd, "temp.def") && row(*cmd, "temp.def")->type == update::CommandType::Remove);
	update::ApplyReport applied;
	for (const char *name : {"history-skip", "history-fresh"}) {
		const fs::path installed = io::os_path(dir.file(name));
		if (std::string(name) == "history-skip")
			TEST_EXPECT(update::apply_section(p1.sections[0], "HISTORY.CMD", installed, applied, error));
		TEST_EXPECT(write_text(io::utf8_path(installed / "player.sav"), "a player's file"));
		TEST_EXPECT(update::apply_section(p3.sections[0], "HISTORY.CMD", installed, applied, error));
		std::string text;
		TEST_EXPECT(io::read_file_text(io::utf8_path(installed / "state.txt"), text, error) && text == "baseline value");
		TEST_EXPECT(!fs::exists(installed / "temporary.txt") && fs::exists(installed / "player.sav"));
		pff::PffArchive archive{};
		TEST_EXPECT(pff::pff_open(&archive, io::utf8_path(installed / "res.pff").c_str()) == 0);
		const pff::PffEntry *entry = pff::pff_find(&archive, "base.def");
		bool correct = entry != nullptr && pff::pff_find(&archive, "temp.def") == nullptr;
		std::vector<uint8_t> data(entry ? entry->size : 0);
		if (entry) correct = correct && pff::pff_extract(&archive, entry, data.data(), data.size()) == 0 && data == bytes("baseline entry");
		pff::pff_close(&archive);
		TEST_EXPECT(correct);
	}
	// Runtime metadata is separate from the retail content's version.txt.
	TEST_EXPECT(write_text(latest + "/onversion.txt", "OpenNova Runtime, V3.2.1;"));
	spec.previous.reset();
	spec.products[0] = {"", "", "ONRUNTIME.CMD", "", "", "opennova.exe", "onversion.txt"};
	TEST_EXPECT(update_tool::build_update_package(spec, p3, report, error));
	const auto runtime = update::read_command_file(std::string(p3.sections[0].entries[0].data.begin(), p3.sections[0].entries[0].data.end()));
	TEST_EXPECT(runtime && runtime->product == "OpenNova Runtime" && runtime->version == "3.2.1");
	TEST_EXPECT(row(*runtime, "onversion.txt") && !row(*runtime, "version.txt"));
	// An archive removed for a release and then recreated keeps its member history.
	// A client skipping that absent release must lose the old member too.
	const std::string absent = dir.file("history-absent"), recreated = dir.file("history-recreated");
	TEST_EXPECT(write_text(absent + "/state.txt", "archive absent"));
	TEST_EXPECT(write_text(recreated + "/state.txt", "archive restored"));
	TEST_EXPECT(make_archive(recreated + "/res.pff", {{"replacement.def", bytes("new member"), 1700000010}}));
	spec.products[0] = {"", "", "HISTORY.CMD", "Synthetic history", "4", ""};
	spec.previous = p1;
	spec.to = io::os_path(absent);
	TEST_EXPECT(update_tool::build_update_package(spec, p2, report, error));
	spec.previous = p2;
	spec.to = io::os_path(recreated);
	TEST_EXPECT(update_tool::build_update_package(spec, p3, report, error));
	const fs::path skipped = io::os_path(dir.file("history-archive-skip"));
	TEST_EXPECT(update::apply_section(p1.sections[0], "HISTORY.CMD", skipped, applied, error));
	TEST_EXPECT(update::apply_section(p3.sections[0], "HISTORY.CMD", skipped, applied, error));
	pff::PffArchive archive{};
	TEST_EXPECT(pff::pff_open(&archive, io::utf8_path(skipped / "res.pff").c_str()) == 0);
	const bool recreated_cleanly = pff::pff_find(&archive, "replacement.def") &&
			!pff::pff_find(&archive, "base.def") && !pff::pff_find(&archive, "temp.def");
	pff::pff_close(&archive);
	TEST_EXPECT(recreated_cleanly);
	return 0;
}

int test_serve(const update::SfsPackage &package) {
	update_tool::PackageCatalog catalog({ "ftp.novaworld.com" });
	std::string error;
	TEST_EXPECT(catalog.bind("SYNus", "SYNPTR.CMD", { package }, error));
	TEST_EXPECT(catalog.bind("SYNEus", "SYNEPTR.CMD", { package }, error));
	TEST_EXPECT(!catalog.bind("NOPEus", "NOPE.CMD", { package }, error));
	TEST_EXPECT(net::startup() == 0);
	update::UpdateServer server(catalog);
	net::UpdateTcpServer listener(server);
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
	if (!synthetic_trees::make_trees(dir)) {
		std::fprintf(stderr, "update_tool: cannot write the synthetic trees\n");
		return 1;
	}
	update::SfsPackage package;
	if (int rc = test_build(dir, package))
		return rc;
	if (int rc = test_serve(package))
		return rc;
	if (int rc = test_history(dir))
		return rc;
	std::printf("update_tool: ok\n");
	return 0;
}
