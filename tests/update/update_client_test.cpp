#include "common/temp_dir.h"
#include "common/test_expect.h"
#include "update/synthetic_trees.h"

#include "client.h"
#include "package_catalog.h"
#include "update_tcp_server.h"

#include <base/io/file_io.h>
#include <formats/update/command_file.h>
#include <formats/update/nlz.h>
#include <formats/update/update_crc.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

using namespace opennova;
namespace fs = std::filesystem;
namespace {

struct Server {
	explicit Server(std::vector<std::string> hosts = {"127.0.0.1"}) : catalog(std::move(hosts)) {}
	update_tool::PackageCatalog catalog;
	update::UpdateServer server{catalog};
	std::mutex lines_mutex;
	std::vector<std::string> lines; // each request's first line, as the listener logs it
	net::UpdateTcpServer listener{server, [this](const std::string &line) {
		std::lock_guard<std::mutex> lock(lines_mutex);
		lines.push_back(line);
	}};
	// How many logged lines end with `request`, once at least `want` do or 2 s have passed.
	size_t requests(const std::string &request, size_t want) {
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		for (;;) {
			size_t n = 0;
			{
				std::lock_guard<std::mutex> lock(lines_mutex);
				for (const std::string &line : lines) {
					const size_t at = line.size() >= request.size() ? line.size() - request.size() : 0;
					if (line.size() >= request.size() && line.compare(at, request.size(), request) == 0) ++n;
				}
			}
			if (n >= want || std::chrono::steady_clock::now() >= until) return n;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}
	std::atomic_bool stop{false};
	std::thread thread;
	bool start(const update::SfsPackage &package, std::string &error, const std::string &login = "TEST",
			const std::string &command = "TEST.CMD") {
		if (!catalog.bind(login, command, {package}, error) || !listener.listen({0}, true)) return false;
		thread = std::thread([this]() {
			while (!stop.load()) {
				if (!listener.pump()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		});
		return true;
	}
	~Server() { stop.store(true); if (thread.joinable()) thread.join(); }
};

update::SfsPackage package(const std::string &text, bool corrupt = false, bool self = false,
		const std::string &command_file = "TEST.CMD") {
	update::CommandFile command;
	command.product = "Synthetic updater client";
	command.version = "1";
	command.exe = "opennova.exe";
	const auto bytes = synthetic_trees::bytes(text);
	command.rows.push_back({"UPD", update::CommandType::Loose, self ? "update.exe" : "version.txt",
			1700000000, int32_t(bytes.size()), update::crc_as_signed(update::file_crc(bytes.data(), bytes.size()))});
	const auto content = synthetic_trees::bytes("test archive entry");
	command.rows.push_back({"TARGET", update::CommandType::Target, "resource.pff", 0, 0, {}});
	command.rows.push_back({"COPY", update::CommandType::Copy, "menu.txt", 1700000000, int32_t(content.size()),
			update::crc_as_signed(update::file_crc(content.data(), content.size()))});
	const std::string cmd = update::write_command_file(command);
	update::SfsSection section;
	section.entries.push_back({command_file, synthetic_trees::bytes(cmd)});
	section.entries.push_back({self ? "update.exe" : "version.txt", update::nlz_encode(bytes.data(), bytes.size())});
	section.entries.push_back({"menu.txt", update::nlz_encode(content.data(), content.size())});
	if (corrupt) section.entries.back().data.back() ^= 0xff;
	return {"Test", {section}};
}

int test_first_install(const test_temp::TempDir &temp) {
	Server server;
	std::string error;
	TEST_EXPECT(server.start(package("First complete content release"), error));
	updater::Options options;
	options.runtime_dir = temp.path;
	options.resource_dir = io::os_path(temp.file("empty install"));
	options.server = "127.0.0.1";
	options.port = server.listener.ports()[0];
	options.check = true;
	std::vector<updater::Product> products{{options.resource_dir, "TEST", "TEST.CMD", {}, true}};
	updater::Result result;
	TEST_EXPECT(updater::run(options, products, result, error));
	TEST_EXPECT(result.available && !result.applied && result.files == 2);
	TEST_EXPECT(!fs::exists(options.resource_dir)); // --check never creates the empty install.
	// Each connection ends with the protocol's CLOSE, the gateway's once its list is read and the
	// update server's once its requests are done [orig: NLUpdateProtocol_Close @ 0x401660
	// (UPDATE.EXE), from Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE) @0x4085b1 and @0x4089c6].
	TEST_EXPECT(server.requests(" < CLOSE", 2) == 2);
	options.check = false;
	TEST_EXPECT(updater::run(options, products, result, error));
	TEST_EXPECT(result.available && result.applied && fs::exists(options.resource_dir / "resource.pff"));
	TEST_EXPECT(server.requests(" < CLOSE", 4) == 4);
	std::string installed;
	TEST_EXPECT(io::read_file_text(io::utf8_path(options.resource_dir / "version.txt"), installed, error));
	TEST_EXPECT(installed == "First complete content release");
	// The rows' files were fetched into the product's update\ directory, which the install
	// leaves behind it no more than UPDATE.EXE's DONE does.
	TEST_EXPECT(!fs::exists(options.resource_dir / "update"));
	options.check = true;
	TEST_EXPECT(updater::run(options, products, result, error));
	TEST_EXPECT(!result.available && !result.applied && result.files == 0);
	options.check = false;
	TEST_EXPECT(updater::run(options, products, result, error));
	TEST_EXPECT(!result.available && !result.applied);

	// A missing installed file is repairable even when the cached command CRC is current.
	std::error_code ec;
	fs::remove(options.resource_dir / "version.txt", ec);
	options.check = true;
	TEST_EXPECT(updater::run(options, products, result, error) && result.available);
	options.check = false;
	TEST_EXPECT(updater::run(options, products, result, error) && result.applied);

	// An update\ directory an earlier run left (a file of a row's name, another of none) is
	// written over and goes with the install: what is installed is what this run fetched.
	fs::remove(options.resource_dir / "version.txt", ec);
	TEST_EXPECT(synthetic_trees::write_text(io::utf8_path(options.resource_dir / "update" / "version.txt"),
			"A stale download"));
	TEST_EXPECT(synthetic_trees::write_text(io::utf8_path(options.resource_dir / "update" / "left.bin"), "left"));
	TEST_EXPECT(updater::run(options, products, result, error) && result.applied);
	TEST_EXPECT(io::read_file_text(io::utf8_path(options.resource_dir / "version.txt"), installed, error) &&
			installed == "First complete content release");
	TEST_EXPECT(!fs::exists(options.resource_dir / "update"));
	return 0;
}

// The jointops zip unzipped: an empty folder holding only JO's wizard. Its product is the retail
// one the wizard names (JOTRus after the territory), not native, so every row is prechecked, and
// in an empty folder every row is missing: a whole release installs the whole game, then checks
// current.
int test_retail_first_install(const test_temp::TempDir &temp) {
	Server server;
	std::string error;
	TEST_EXPECT(server.start(package("A whole retail release", false, false, "JOPTR.CMD"), error, "JOTRus",
			"JOPTR.CMD"));
	updater::Options options;
	TEST_EXPECT(updater::parse_options({"/game", "jo", "--resource-dir", temp.file("jointops zip"),
			"--no-runtime", "--server", "127.0.0.1", "--port", std::to_string(server.listener.ports()[0])},
			temp.path / "update.exe", options, error));
	TEST_EXPECT(synthetic_trees::write_text(temp.file("jointops zip/UPDATE.WIZ"),
			"[HEADER]\r\nCMD=JOPTR.CMD\r\nURL=update.opennova.net\r\nPRODUCT_LOGIN=JOTRqq\r\n"));
	std::vector<updater::Product> products;
	TEST_EXPECT(updater::discover_products(options, products, error) && products.size() == 1);
	TEST_EXPECT(products[0].login == "JOTRus" && products[0].command == "JOPTR.CMD" && !products[0].native);
	updater::Result result;
	TEST_EXPECT(updater::run(options, products, result, error));
	TEST_EXPECT(result.applied && result.files == 2 && fs::exists(options.resource_dir / "resource.pff") &&
			fs::exists(options.resource_dir / "JOPTR.CMD"));
	std::string installed;
	TEST_EXPECT(io::read_file_text(io::utf8_path(options.resource_dir / "version.txt"), installed, error) &&
			installed == "A whole retail release");
	options.check = true;
	TEST_EXPECT(updater::run(options, products, result, error) && !result.available && result.files == 0);
	return 0;
}

// The service names retail's ftp.novaworld.com in SERVER.LST for the products a stock
// UPDATE.EXE reaches through the Switcher; this client dials the gateway it came through.
int test_retail_server_list(const test_temp::TempDir &temp) {
	Server server({"ftp.novaworld.com"});
	std::string error;
	TEST_EXPECT(server.start(package("Served under retail's update host"), error));
	updater::Options options;
	options.runtime_dir = temp.path;
	options.resource_dir = io::os_path(temp.file("retail list"));
	options.server = "127.0.0.1";
	options.port = server.listener.ports()[0];
	options.check = true;
	const std::vector<updater::Product> products{{options.resource_dir, "TEST", "TEST.CMD", {}, false}};
	updater::Result result;
	TEST_EXPECT(updater::run(options, products, result, error) && result.available);
	return 0;
}

// A check goes on past a product it cannot check: another product's update is still
// reported; with none found, the check fails, as the dead product may have one.
int test_check_goes_past_a_failing_product(const test_temp::TempDir &temp) {
	Server server;
	std::string error;
	TEST_EXPECT(server.start(package("Checked beside an unpublished product"), error));
	updater::Options options;
	options.runtime_dir = temp.path;
	options.resource_dir = io::os_path(temp.file("check past"));
	options.server = "127.0.0.1";
	options.port = server.listener.ports()[0];
	options.check = true;
	const std::vector<updater::Product> products{{temp.path / "unpublished", "MISSING", "MISSING.CMD", {}, true},
			{options.resource_dir, "TEST", "TEST.CMD", {}, true}};
	updater::Result result;
	TEST_EXPECT(updater::run(options, products, result, error) && result.available && result.products == 1);
	options.check = false;
	TEST_EXPECT(!updater::run(options, products, result, error)); // a failing game stops an install
	// A failing expansion does not: it is recorded and the install goes on, as retail's nested
	// expansion runs do. Every expansion's outcome is recorded in order, a succeeded one's too, for
	// the box each nested run shows [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE), US_DONE
	// @0x408a9e, @0x408aac].
	updater::Product unpublished_expansion = products[0];
	unpublished_expansion.expansion = true;
	updater::Product published_expansion = products[1];
	published_expansion.expansion = true;
	// The folder's wizard names TEST.CMD, which nothing has installed yet: a mark taken at the run's
	// start finds none, the expansion's own run, after the game's install, finds it.
	TEST_EXPECT(synthetic_trees::write_text(io::utf8_path(options.resource_dir / "UPDATE.WIZ"),
			"[HEADER]\r\nCMD=TEST.CMD\r\n"));
	TEST_EXPECT(!updater::version_mark(options.resource_dir).present);
	TEST_EXPECT(updater::run(options, {products[1], unpublished_expansion, published_expansion}, result, error) &&
			result.applied && result.expansions.size() == 2);
	TEST_EXPECT(result.expansions[0].dir == products[0].dir && !result.expansions[0].ok &&
			!result.expansions[0].error.empty());
	TEST_EXPECT(result.expansions[1].dir == options.resource_dir && result.expansions[1].ok &&
			result.expansions[1].error.empty());
	// Its command file as its own run found it: after the game's install before it, so current.
	const updater::VersionMark installed = updater::version_mark(options.resource_dir);
	TEST_EXPECT(installed.present && result.expansions[1].start.present &&
			result.expansions[1].start.crc == installed.crc);
	options.check = true;
	TEST_EXPECT(updater::run(options, {products[1], published_expansion}, result, error) && result.expansions.empty());
	TEST_EXPECT(!updater::run(options, products, result, error) && !result.available && !error.empty());
	return 0;
}

// A gateway that refuses the login is closed with the protocol's CLOSE too, as retail's
// OPEN_GATEWAY closes it before the next try [orig: Cupdate2Dlg_StateTick @ 0x408170
// (UPDATE.EXE) @0x40838e; NLUpdateProtocol_Close @ 0x401660 (UPDATE.EXE)].
int test_refused_gateway_closes(const test_temp::TempDir &temp) {
	Server server;
	std::string error;
	TEST_EXPECT(server.start(package("Refused at the gateway"), error));
	updater::Options options;
	options.runtime_dir = temp.path;
	options.resource_dir = io::os_path(temp.file("refused gateway"));
	options.server = "127.0.0.1";
	options.port = server.listener.ports()[0];
	const std::vector<updater::Product> products{{options.resource_dir, "MISSING", "MISSING.CMD", {}, true}};
	updater::Result result;
	TEST_EXPECT(!updater::run(options, products, result, error));
	TEST_EXPECT(server.requests(" < CLOSE", 1) == 1);
	return 0;
}

int test_failures(const test_temp::TempDir &temp) {
	Server server;
	std::string error;
	TEST_EXPECT(server.start(package("No partial writes on corrupt payload", true), error));
	updater::Options options;
	options.runtime_dir = temp.path;
	options.resource_dir = io::os_path(temp.file("corrupt"));
	options.server = "127.0.0.1";
	options.port = server.listener.ports()[0];
	std::vector<updater::Product> products{{options.resource_dir, "TEST", "TEST.CMD", {}, true}};
	updater::Result result;
	TEST_EXPECT(!updater::run(options, products, result, error));
	TEST_EXPECT(!error.empty() && !fs::exists(options.resource_dir));
	std::atomic_bool cancel{true};
	TEST_EXPECT(!updater::run(options, products, result, error, {}, &cancel));
	TEST_EXPECT(error == "Update aborted!" && !fs::exists(options.resource_dir));
	products[0].login = "MISSING";
	options.check = true;
	TEST_EXPECT(!updater::run(options, products, result, error));
	TEST_EXPECT(!fs::exists(options.resource_dir));
	// The same refusal over an installed folder: the folder keeps what it had, and the files the
	// run fetched go with their update\ directory.
	options.check = false;
	products[0].login = "TEST";
	products[0].dir = options.resource_dir = io::os_path(temp.file("corrupt over an install"));
	TEST_EXPECT(synthetic_trees::write_text(io::utf8_path(options.resource_dir / "keep.txt"), "the player's"));
	TEST_EXPECT(!updater::run(options, products, result, error) && !error.empty());
	TEST_EXPECT(fs::exists(options.resource_dir / "keep.txt") && !fs::exists(options.resource_dir / "update") &&
			!fs::exists(options.resource_dir / "version.txt"));
	return 0;
}

int test_revision(const test_temp::TempDir &temp) {
	Server server;
	std::string error;
	const std::string replacement = "The replacement updater image";
	std::string running = replacement;
	running.back() = '!';
	TEST_EXPECT(update::file_crc(reinterpret_cast<const uint8_t *>(running.data()), running.size()) ==
			update::file_crc(reinterpret_cast<const uint8_t *>(replacement.data()), replacement.size()));
	TEST_EXPECT(server.start(package(replacement, false, true), error));
	updater::Options options;
	options.runtime_dir = io::os_path(temp.file("revision"));
	options.resource_dir = options.runtime_dir;
	options.server = "127.0.0.1";
	options.port = server.listener.ports()[0];
	TEST_EXPECT(synthetic_trees::write_text(io::utf8_path(options.runtime_dir / "update.exe"), running));
	const std::vector<updater::Product> products{{options.runtime_dir, "TEST", "TEST.CMD", {}, true},
			{temp.path / "unavailable-content", "MISSING", "CONTENT.CMD", {}, true}};
	updater::Result result;
	TEST_EXPECT(updater::run(options, products, result, error));
	TEST_EXPECT(result.revision_pending && result.products == 1);
	std::string text;
	TEST_EXPECT(io::read_file_text(io::utf8_path(options.runtime_dir / "update.exe"), text, error) && text == running);
	TEST_EXPECT(io::read_file_text(io::utf8_path(options.runtime_dir / "update.new"), text, error) && text == replacement);
	// update.exe's row was fetched as update\update.exe and installed from there as update.new.
	TEST_EXPECT(!fs::exists(options.runtime_dir / "update"));
	// A failed helper launch can retry despite the cached command and colliding old CRC.
	TEST_EXPECT(updater::run(options, products, result, error) && result.revision_pending && result.files == 1);
	// The helper's successful replacement leaves the runtime current and allows the
	// relaunched updater to continue into content, whose failure is then reported.
	TEST_EXPECT(io::replace_file(io::utf8_path(options.runtime_dir / "update.new"),
			io::utf8_path(options.runtime_dir / "update.exe"), error));
	TEST_EXPECT(updater::run(options, {products.front()}, result, error) && !result.available && !result.revision_pending);
	TEST_EXPECT(!updater::run(options, products, result, error) && !result.revision_pending && result.products == 1);
	return 0;
}

// The self-update hand-off round trip: the game's install arguments, update.exe's arguments to
// revupdat, and revupdat's relaunch of update.exe give the install's arguments back but the game's
// --parent-pid (the game has exited), so the relaunched updater runs the same install; revupdat
// itself waits for the updater that started it.
int test_revision_handoff(const test_temp::TempDir &temp) {
	std::string error;
	const std::vector<std::string> install = {"/RUN", "--parent-pid", "4242", "/game", "jo",
			"--runtime-dir", temp.file("handoff runtime"), "--resource-dir", temp.file("handoff content"),
			"--run-arg", "--", "--run-arg", "--parent-pid", "--run-arg", "--after-update"};
	updater::Options options;
	TEST_EXPECT(updater::parse_options(install, temp.path / "update.exe", options, error));
	updater::RevisionArguments revision;
	TEST_EXPECT(updater::read_revision_arguments(
			updater::revision_handoff_arguments(options, install, 777), revision, error));
	updater::Options helper;
	TEST_EXPECT(updater::parse_options(revision.own, temp.path / "revupdat.exe", helper, error));
	TEST_EXPECT(helper.parent_pid == 777 && helper.runtime_dir == options.runtime_dir);
	const std::vector<std::string> relaunch = {"/RUN", "/game", "jo",
			"--runtime-dir", io::utf8_path(options.runtime_dir), "--resource-dir", io::utf8_path(options.resource_dir),
			"--run-arg", "--", "--run-arg", "--parent-pid", "--run-arg", "--after-update"};
	TEST_EXPECT(revision.relaunch == relaunch);
	updater::Options relaunched;
	TEST_EXPECT(updater::parse_options(revision.relaunch, temp.path / "update.exe", relaunched, error));
	TEST_EXPECT(relaunched.parent_pid == 0 && relaunched.run && relaunched.game == "jo" &&
			relaunched.game_explicit && !relaunched.legacy && relaunched.runtime_dir == options.runtime_dir &&
			relaunched.resource_dir == options.resource_dir && relaunched.run_args == options.run_args);
	// Retail's game launches update.exe <its window> /force /log /run: by the relaunch that handle
	// is stale, so it goes over as 0 and /run carries the run.
	const std::vector<std::string> legacy = {"123456", "/force", "/log", "/run"};
	TEST_EXPECT(updater::parse_options(legacy, temp.path / "update.exe", options, error) && options.legacy &&
			options.parent_window == 123456);
	TEST_EXPECT(updater::read_revision_arguments(
			updater::revision_handoff_arguments(options, legacy, 777), revision, error));
	TEST_EXPECT((revision.relaunch == std::vector<std::string>{"0", "/force", "/log", "/run"}));
	TEST_EXPECT(updater::parse_options(revision.relaunch, temp.path / "update.exe", relaunched, error) &&
			relaunched.legacy && relaunched.run && relaunched.parent_window == 0);
	// revupdat's own options need their values.
	TEST_EXPECT(!updater::read_revision_arguments({"--runtime-dir"}, revision, error) && !error.empty());
	TEST_EXPECT(!updater::read_revision_arguments({"--forward-options", "--update-arg"}, revision, error));
	return 0;
}

// revupdat's replacement of update.exe [orig: RevUpdate_WndProc @ 0x401290 (REVUPDAT.EXE)]: a
// staged update.new takes update.exe's place; with none there is nothing to do; an update.exe that
// cannot be deleted fails with retail's text (@0x4070d0), update.new kept for the next run.
int test_staged_updater_install(const test_temp::TempDir &temp) {
	std::string error;
	const fs::path dir = io::os_path(temp.file("staged updater"));
	std::error_code ec;
	fs::create_directories(dir, ec);
	TEST_EXPECT(updater::install_staged_updater(dir, false, error)); // nothing staged
	TEST_EXPECT(synthetic_trees::write_text(io::utf8_path(dir / "update.exe"), "the old image"));
	TEST_EXPECT(synthetic_trees::write_text(io::utf8_path(dir / "update.new"), "the new image"));
	TEST_EXPECT(updater::install_staged_updater(dir, false, error));
	std::string text;
	TEST_EXPECT(io::read_file_text(io::utf8_path(dir / "update.exe"), text, error) && text == "the new image");
	TEST_EXPECT(!fs::exists(dir / "update.new"));
	// An update.exe that cannot be replaced (here a folder holding a file).
	const fs::path blocked = io::os_path(temp.file("staged updater blocked"));
	TEST_EXPECT(synthetic_trees::write_text(io::utf8_path(blocked / "update.exe" / "inside.txt"), "x"));
	TEST_EXPECT(synthetic_trees::write_text(io::utf8_path(blocked / "update.new"), "the new image"));
	error.clear();
	TEST_EXPECT(!updater::install_staged_updater(blocked, false, error));
	const std::string deleted = "ERROR: Could not delete 'Update.exe'. (";
	TEST_EXPECT(error.compare(0, deleted.size(), deleted) == 0 && error.back() == ')');
	TEST_EXPECT(fs::exists(blocked / "update.new"));
	// The relaunch's text and the boxes' caption, retail's (@0x407054, @0x40703c (REVUPDAT.EXE)).
	TEST_EXPECT(std::string(updater::kRevupdatRelaunchFailed) == "ERROR: Could not re-launch 'Update.exe'.");
	TEST_EXPECT(std::string(updater::kRevupdatTitle) == "Update Revision Control");
	return 0;
}

int test_options_and_discovery(const test_temp::TempDir &temp) {
	updater::Options options;
	std::string error;
	TEST_EXPECT(updater::parse_options({"--check", "/game", "DFX2", "--runtime-dir", temp.root(),
			"--resource-dir", temp.file("retail"), "/RUN", "--run-arg", "--resource-dir", "--run-arg", "a b"},
			temp.path / "update.exe", options, error));
	TEST_EXPECT(options.check && options.game == "dfx2" && options.run && options.run_args == std::vector<std::string>({"--resource-dir", "a b"}));
	TEST_EXPECT(!updater::parse_options({"--port", "0"}, temp.path / "update.exe", options, error));
	TEST_EXPECT(!updater::parse_options({"--unknown"}, temp.path / "update.exe", options, error));
	// The game's window handle implies /RUN; a zero one (revupdat's stand-in) does not.
	TEST_EXPECT(updater::parse_options({"1234", "/force", "/log"}, temp.path / "update.exe", options, error));
	TEST_EXPECT(options.legacy && options.run && options.parent_window == 1234);
	TEST_EXPECT(updater::parse_options({"0", "/DIR", temp.file("retail")}, temp.path / "update.exe", options, error));
	TEST_EXPECT(options.legacy && !options.run);
	TEST_EXPECT(updater::parse_options({"0", "/DIR", temp.file("retail"), "/RUN"}, temp.path / "update.exe", options, error));
	TEST_EXPECT(options.legacy && options.no_runtime);
	// A JO-shaped install: JO:CA's wizard (no cc.bin: the US territory over QQ).
	TEST_EXPECT(synthetic_trees::write_text(temp.file("retail/UPDATE.WIZ"),
			"[HEADER]\r\nCMD=JOPTR.CMD\r\nPRODUCT_LOGIN=JOTRqq\r\nURL=127.0.0.1\r\n"));
	std::vector<updater::Product> products;
	TEST_EXPECT(updater::discover_products(options, products, error));
	TEST_EXPECT(products.size() == 1 && products[0].login == "JOTRus" && !products[0].native);
	// Never the wizard's own URL: OpenNova's service, the runtime's OPENNOVA.WIZ naming it.
	TEST_EXPECT(products[0].gateways == std::vector<std::string>({"update.opennova.net"}));
	// Every expansion\<n>\ holding a wizard is a product, as retail's, its archive installed or
	// not: a whole release installs it (the jointops zip ships Escalation's wizard alone).
	TEST_EXPECT(synthetic_trees::write_text(temp.file("retail/expansion/testexp/UPDATE.WIZ"),
			"[HEADER]\r\nCMD=EXP.CMD\r\nPRODUCT_LOGIN=EXP\r\nURL=127.0.0.1\r\n"));
	TEST_EXPECT(updater::discover_products(options, products, error) && products.size() == 2);
	TEST_EXPECT(products[1].login == "EXP" && products[1].command == "EXP.CMD" && products[1].expansion &&
			!products[1].native && products[1].dir.filename() == "testexp");
	std::error_code removed;
	fs::remove_all(temp.path / "retail" / "expansion", removed);
	TEST_EXPECT(!removed);
	// OpenNova content never lands in a folder whose wizard names another game, even when
	// /game asks for it; nor does another product the wizard does not name.
	TEST_EXPECT(updater::parse_options({"/game", "acidgambit", "--resource-dir", temp.file("retail")},
			temp.path / "update.exe", options, error));
	TEST_EXPECT(!updater::discover_products(options, products, error) && error.find("UPDATE.WIZ") != std::string::npos);
	TEST_EXPECT(updater::parse_options({"/game", "dfx2", "--resource-dir", temp.file("retail")},
			temp.path / "update.exe", options, error));
	TEST_EXPECT(!updater::discover_products(options, products, error));
	// A bare launch there updates the game its wizard names, as /game jo does.
	TEST_EXPECT(updater::parse_options({}, temp.path / "retail/update.exe", options, error));
	TEST_EXPECT(updater::discover_products(options, products, error) && products.size() == 2);
	TEST_EXPECT(products[0].login == "ONRUNTIME" && products[1].login == "JOTRus" && !products[1].native);
	TEST_EXPECT(synthetic_trees::write_text(temp.file("retail/OPENNOVA.WIZ"),
			"[HEADER]\r\nCMD=ONRUNTIME.CMD\r\nPRODUCT_LOGIN=ONRUNTIME\r\nURL=update.test.example\r\n"));
	TEST_EXPECT(updater::discover_products(options, products, error) && products.size() == 2);
	TEST_EXPECT(products[0].gateways == std::vector<std::string>({"update.test.example"}) &&
			products[1].gateways == products[0].gateways);
	TEST_EXPECT(updater::parse_options({"/game", "JO"}, temp.path / "retail/update.exe", options, error));
	TEST_EXPECT(updater::discover_products(options, products, error) && products.size() == 2);
	// A wizard naming a game OpenNova does not update (BHD's) is refused outright.
	TEST_EXPECT(synthetic_trees::write_text(temp.file("bhd/UPDATE.WIZ"), "[HEADER]\r\nCMD=DFBHD.CMD\r\n"));
	TEST_EXPECT(updater::parse_options({}, temp.path / "bhd/update.exe", options, error));
	TEST_EXPECT(!updater::discover_products(options, products, error));
	// A folder with no wizard takes OpenNova content only while it holds no game archive (a
	// DFX install ships no wizard); an empty one is the first install.
	TEST_EXPECT(synthetic_trees::write_text(temp.file("wizardless/resource.pff"), "archive"));
	TEST_EXPECT(updater::parse_options({"/game", "acidgambit"}, temp.path / "wizardless/update.exe", options, error));
	TEST_EXPECT(!updater::discover_products(options, products, error));
	std::error_code made;
	fs::create_directories(temp.path / "fresh", made);
	TEST_EXPECT(updater::parse_options({}, temp.path / "fresh/update.exe", options, error));
	TEST_EXPECT(updater::discover_products(options, products, error) && products.size() == 2);
	TEST_EXPECT(products[0].login == "ONRUNTIME" && products[1].login == "ACIDGAMBIT" && products[1].native);
	// A completed native install also has UPDATE.WIZ. Bare launch must retain
	// full native rows and recognize its registered expansion without an archive.
	// The runtime's OPENNOVA.WIZ is a debug zip's, naming staging's service
	// (package_godot_windows.ps1 -ExportMode debug), and the content's and the
	// expansion's own wizards name prod's (a release prod's data program built): every
	// product updates from staging alone.
	TEST_EXPECT(synthetic_trees::write_text(temp.file("native/OPENNOVA.WIZ"),
			"[HEADER]\r\nCMD=ONRUNTIME.CMD\r\nURL=update.staging.opennova.net\r\n"
			"PRODUCT_LOGIN=ONRUNTIME\r\n"));
	TEST_EXPECT(synthetic_trees::write_text(temp.file("native/UPDATE.WIZ"),
			"[HEADER]\r\nCMD=ACIDGAMBIT.CMD\r\nURL=update.opennova.net\r\n"
			"PRODUCT_LOGIN=ACIDGAMBIT\r\n"));
	TEST_EXPECT(synthetic_trees::write_text(temp.file("native/expansion/nifty/UPDATE.WIZ"),
			"[HEADER]\r\nCMD=NIFTY.CMD\r\nURL=update.opennova.net\r\nPRODUCT_LOGIN=NIFTY\r\n"));
	TEST_EXPECT(updater::parse_options({}, temp.path / "native/update.exe", options, error));
	TEST_EXPECT(updater::discover_products(options, products, error) && products.size() == 3);
	TEST_EXPECT(products[0].login == "ONRUNTIME" && products[1].login == "ACIDGAMBIT" && products[1].native &&
			products[2].login == "NIFTY" && products[2].native);
	for (const updater::Product &product : products)
		TEST_EXPECT(product.gateways == std::vector<std::string>({"update.staging.opennova.net"}));
	return 0;
}

} // namespace
int main() {
	TEST_EXPECT(net::startup() == 0);
	test_temp::TempDir temp("update_client");
	if (int rc = test_first_install(temp)) return rc;
	if (int rc = test_retail_first_install(temp)) return rc;
	if (int rc = test_retail_server_list(temp)) return rc;
	if (int rc = test_check_goes_past_a_failing_product(temp)) return rc;
	if (int rc = test_refused_gateway_closes(temp)) return rc;
	if (int rc = test_failures(temp)) return rc;
	if (int rc = test_revision(temp)) return rc;
	if (int rc = test_revision_handoff(temp)) return rc;
	if (int rc = test_staged_updater_install(temp)) return rc;
	if (int rc = test_options_and_discovery(temp)) return rc;
	std::puts("update_client: ok");
	return 0;
}
