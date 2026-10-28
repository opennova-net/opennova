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
#include <cstdio>
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
		const std::string &command_file = "TEST.CMD", const std::string &exe = "opennova.exe") {
	update::CommandFile command;
	command.product = "Synthetic updater client";
	command.version = "1";
	command.exe = exe;
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

// An install's connect waits retail's 10 s before the next server is tried [orig: UpdateSocket_Connect
// @ 0x403570 (UPDATE.EXE), 0x2710 @0x40372c].
int test_install_connect_timeout() {
	TEST_EXPECT(updater::kInstallConnectTimeoutMs == 10000);
	// The result box is retail's type 0, no icon [orig: Cupdate2Dlg_ReportResult @ 0x405140
	// (UPDATE.EXE) @0x4053ce].
	TEST_EXPECT(updater::kResultBoxType == 0);
	return 0;
}

// The dialog's counter and elapsed lines, StateTick's formats [orig: Cupdate2Dlg_StateTick @ 0x408170
// (UPDATE.EXE) @0x408bd2..0x408c7c, @0x408d34, @0x408dc4; Cupdate2Dlg_PrecheckCommands @ 0x407820
// (UPDATE.EXE) @0x40791b].
int test_dialog_lines() {
	updater::Progress progress;
	updater::DialogLines lines = updater::dialog_lines(progress, 2500);
	TEST_EXPECT(lines.downloaded.empty() && lines.throughput.empty() && lines.estimated.empty());
	TEST_EXPECT(lines.elapsed == "Elapsed time: 2.5 seconds");
	TEST_EXPECT(updater::dialog_lines(progress, 12345).elapsed == "Elapsed time: 12.3 seconds");
	TEST_EXPECT(updater::dialog_lines(progress, 0).elapsed == "Elapsed time: 0.0 seconds");
	progress.lines = updater::Progress::Lines::Commands;
	progress.done = 3;
	progress.total = 10;
	lines = updater::dialog_lines(progress, 0);
	TEST_EXPECT(lines.downloaded == "3/10 Commands" && lines.throughput.empty() && lines.estimated.empty());
	progress.lines = updater::Progress::Lines::Files;
	progress.done = 2;
	progress.total = 5;
	TEST_EXPECT(updater::dialog_lines(progress, 0).downloaded == "2/5 Files");
	// The fetch: KBytes by a shift, the rate in bytes a second, KBPS past 1024, the estimate all the
	// bytes over the rate.
	progress.lines = updater::Progress::Lines::Download;
	progress.done = 1536000;
	progress.total = 3072000;
	lines = updater::dialog_lines(progress, 3000);
	TEST_EXPECT(lines.downloaded == "Downloading:  1500/3000 KBytes");
	TEST_EXPECT(lines.throughput == "Throughput:  500 KBPS");
	TEST_EXPECT(lines.estimated == "Estimated time: 6.0 seconds");
	progress.done = 1000;
	progress.total = 4000;
	lines = updater::dialog_lines(progress, 2000);
	TEST_EXPECT(lines.downloaded == "Downloading:  0/3 KBytes" && lines.throughput == "Throughput:  500 BPS" &&
			lines.estimated == "Estimated time: 8.0 seconds");
	progress.done = 1024;
	TEST_EXPECT(updater::dialog_lines(progress, 1000).throughput == "Throughput:  1024 BPS");
	progress.done = 1025;
	TEST_EXPECT(updater::dialog_lines(progress, 1000).throughput == "Throughput:  1 KBPS");
	// No bytes yet: a rate of 0 and no estimate; the first millisecond's infinite rate is x87's
	// integer indefinite.
	progress.done = 0;
	lines = updater::dialog_lines(progress, 500);
	TEST_EXPECT(lines.throughput == "Throughput:  0 BPS" && lines.estimated == "Estimated time: -");
	progress.done = 100;
	lines = updater::dialog_lines(progress, 0);
	TEST_EXPECT(lines.throughput == "Throughput:  -2147483648 BPS" && lines.estimated == "Estimated time: -");
	return 0;
}

// A run's dialog, as retail's states write it: every status line in their order (the gateway's
// with its place and port), the product line from the command file, the precheck's rows, the
// fetch's bytes and files, the install's rows and the integration's jobs.
int test_dialog_progress(const test_temp::TempDir &temp) {
	Server server;
	std::string error;
	TEST_EXPECT(server.start(package("A release the dialog follows"), error));
	updater::Options options;
	options.runtime_dir = temp.path;
	options.resource_dir = io::os_path(temp.file("dialog install"));
	options.server = "127.0.0.1";
	options.port = server.listener.ports()[0];
	std::vector<updater::Product> products{{options.resource_dir, "TEST", "TEST.CMD", {}, true}};
	std::vector<updater::Progress> shown;
	const updater::ProgressSink sink = [&shown](const updater::Progress &progress) { shown.push_back(progress); };
	updater::Result result;
	TEST_EXPECT(updater::run(options, products, result, error, sink) && result.applied);
	std::vector<std::string> statuses;
	for (const updater::Progress &progress : shown)
		if (statuses.empty() || statuses.back() != progress.status) statuses.push_back(progress.status);
	char where[48], opening[48];
	std::snprintf(where, sizeof where, "\n(Server 1, Port %x)   ", unsigned(options.port));
	std::snprintf(opening, sizeof opening, "Opening update server...\n(Port %x)   ", unsigned(options.port));
	const std::vector<std::string> expected = {"Processing wizard file...",
		std::string("Locating Gateway Server...") + where, std::string("Logging into Gateway Server...") + where,
		std::string("Fetching server list...") + where, opening, "Logging into remote server...",
		"Checking for update...", "Fetching command file...", "Processing command file...",
		"Checking file versions...", "Fetching files...", "Decompressing update...", "Integrating update..."};
	TEST_EXPECT(statuses == expected);
	const auto with = [&shown](const char *status) {
		std::vector<updater::Progress> out;
		for (const updater::Progress &progress : shown) if (progress.status == status) out.push_back(progress);
		return out;
	};
	// The product line is the command file's PRODUCT once it is read; the clock runs from the start.
	TEST_EXPECT(with("Processing command file...").front().product.empty());
	TEST_EXPECT(with("Checking file versions...").front().product == "Synthetic updater client");
	TEST_EXPECT(shown.back().start == shown.front().start && shown.front().start != std::chrono::steady_clock::time_point{});
	// The precheck: each of the 3 rows as it is reached, the bar over them.
	const std::vector<updater::Progress> precheck = with("Checking file versions...");
	TEST_EXPECT(precheck.back().lines == updater::Progress::Lines::Commands && precheck.back().done == 2 &&
			precheck.back().total == 3 && precheck.back().range == 3 && precheck.back().position == 2);
	// The fetch: version.txt's and menu.txt's bytes of theirs, the bar over the 2 files, the last one
	// the current file.
	const uint64_t bytes = synthetic_trees::bytes("A release the dialog follows").size() +
			synthetic_trees::bytes("test archive entry").size();
	const std::vector<updater::Progress> fetch = with("Fetching files...");
	TEST_EXPECT(fetch.front().lines == updater::Progress::Lines::Download && fetch.front().done == 0 &&
			fetch.front().total == bytes && fetch.front().range == 2 && fetch.front().position == 0);
	TEST_EXPECT(fetch.back().lines == updater::Progress::Lines::Blank && fetch.back().position == 2 &&
			fetch.back().file == "menu.txt");
	TEST_EXPECT(fetch[fetch.size() - 2].done == bytes && fetch[fetch.size() - 2].lines == updater::Progress::Lines::Download);
	// The install: the rows by their place among the command file's 3, the COPY row the current file;
	// then the integration's 1 job done.
	const std::vector<updater::Progress> install = with("Decompressing update...");
	TEST_EXPECT(install.front().lines == updater::Progress::Lines::Blank);
	TEST_EXPECT(install.back().lines == updater::Progress::Lines::Files && install.back().done == 2 &&
			install.back().total == 3 && install.back().range == 3 && install.back().file == "menu.txt");
	const updater::Progress &end = shown.back();
	TEST_EXPECT(end.status == "Integrating update..." && end.lines == updater::Progress::Lines::Files &&
			end.done == 1 && end.total == 1 && end.position == 1 && end.range == 1 && end.file.empty());
	// A run with nothing to update: the command file kept (no "Fetching command file..."), nothing to
	// fetch, and INTEGRATE's jobs still the archive TARGET's one, as retail queues an AddDir for every
	// archive target [orig: Cupdate2Dlg_InstallPffRows @ 0x406380 (UPDATE.EXE) @0x406530].
	shown.clear();
	TEST_EXPECT(updater::run(options, products, result, error, sink) && !result.available);
	TEST_EXPECT(with("Fetching command file...").empty() && !with("Checking file versions...").empty());
	const std::vector<updater::Progress> idle = with("Fetching files...");
	TEST_EXPECT(idle.front().total == 0 && idle.front().range == 0 && idle.back().position == 0);
	TEST_EXPECT(shown.back().status == "Integrating update..." && shown.back().done == 1 &&
			shown.back().total == 1 && shown.back().range == 1 && shown.back().position == 1);
	return 0;
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
// UPDATE.EXE reaches through a hosts-file line; this client dials the gateway it came through.
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
	// Only update.exe's own hand-off is read: a stock UPDATE.EXE's `/RUN <its command line>`, or
	// any argument not revupdat's, is refused, as is a command line with no --forward-options.
	error.clear();
	TEST_EXPECT(!updater::read_revision_arguments({"/RUN", "123456", "/DIR", "x"}, revision, error) &&
			error == "unknown option /RUN");
	TEST_EXPECT(!updater::read_revision_arguments({"--runtime-dir", "x", "--parent-pid", "7"}, revision, error));
	TEST_EXPECT(updater::read_revision_arguments({"--forward-options"}, revision, error) &&
			revision.own.empty() && revision.relaunch.empty());
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
	// A player's game (ADR 0074): its wizard's GAME makes it a native game of its own, which its
	// own login and CMD update, a bare launch's or /game's by its login; its mods are native with
	// it. Another product there is refused, OpenNova's own included.
	TEST_EXPECT(synthetic_trees::write_text(temp.file("player/UPDATE.WIZ"),
			"[HEADER]\r\nCMD=MYGAME.CMD\r\nURL=update.opennova.net\r\nPRODUCT_LOGIN=MYGAME\r\nGAME=jo\r\n"));
	TEST_EXPECT(synthetic_trees::write_text(temp.file("player/expansion/mymod/UPDATE.WIZ"),
			"[HEADER]\r\nCMD=MYMOD.CMD\r\nURL=update.opennova.net\r\nPRODUCT_LOGIN=MYMOD\r\n"));
	for (const std::vector<std::string> &args : {std::vector<std::string>{},
			std::vector<std::string>{"/game", "MyGame"}}) {
		TEST_EXPECT(updater::parse_options(args, temp.path / "player/update.exe", options, error));
		TEST_EXPECT(updater::discover_products(options, products, error) && products.size() == 3);
		TEST_EXPECT(products[0].login == "ONRUNTIME" && products[1].login == "MYGAME" &&
				products[1].command == "MYGAME.CMD" && products[1].native && !products[1].expansion &&
				products[2].login == "MYMOD" && products[2].native && products[2].expansion);
	}
	for (const char *other : {"acidgambit", "jo", "othergame"}) {
		TEST_EXPECT(updater::parse_options({"/game", other}, temp.path / "player/update.exe", options, error));
		TEST_EXPECT(!updater::discover_products(options, products, error) && error.find("UPDATE.WIZ") != std::string::npos);
	}
	// Its login names no game anywhere else.
	TEST_EXPECT(updater::parse_options({"/game", "mygame"}, temp.path / "fresh/update.exe", options, error));
	TEST_EXPECT(!updater::discover_products(options, products, error));
	return 0;
}

// An app's folder: the editor's, its OPENNOVA.WIZ naming ONEDITOR. It updates that app alone, with
// no game slot and no expansion (an expansion\ folder there is not one), /game and --no-runtime
// refused; the app installs every row of a new release, and /RUN starts its command file's EXE.
int test_app_folder(const test_temp::TempDir &temp) {
	Server server;
	std::string error;
	TEST_EXPECT(server.start(package("The editor's release", false, false, "ONEDITOR.CMD", "oneditor.exe"), error,
			"ONEDITOR", "ONEDITOR.CMD"));
	TEST_EXPECT(synthetic_trees::write_text(temp.file("editor/OPENNOVA.WIZ"),
			"[HEADER]\r\nCMD=ONEDITOR.CMD\r\nURL=update.test.example\r\nPRODUCT_LOGIN=ONEDITOR\r\n"));
	TEST_EXPECT(synthetic_trees::write_text(temp.file("editor/expansion/stray/UPDATE.WIZ"),
			"[HEADER]\r\nCMD=EXP.CMD\r\nPRODUCT_LOGIN=EXP\r\n"));
	updater::Options options;
	TEST_EXPECT(updater::parse_options({"/RUN"}, temp.path / "editor/update.exe", options, error));
	std::vector<updater::Product> products;
	TEST_EXPECT(updater::discover_products(options, products, error) && products.size() == 1);
	TEST_EXPECT(products[0].login == "ONEDITOR" && products[0].command == "ONEDITOR.CMD" && products[0].app &&
			products[0].native && !products[0].expansion && products[0].dir == options.runtime_dir);
	TEST_EXPECT(products[0].gateways == std::vector<std::string>({"update.test.example"}));
	// No game is updated from an app's folder, nor the app skipped.
	updater::Options refused;
	for (const std::vector<std::string> &args : {std::vector<std::string>{"/game", "jo"},
			std::vector<std::string>{"/game", "acidgambit"}, std::vector<std::string>{"--no-runtime"},
			std::vector<std::string>{"0", "/DIR", temp.file("editor")}}) {
		TEST_EXPECT(updater::parse_options(args, temp.path / "editor/update.exe", refused, error));
		error.clear();
		TEST_EXPECT(!updater::discover_products(refused, products, error) && error.find("ONEDITOR") != std::string::npos);
	}
	// No command file yet: the result box shows no version; the install then reads the app's own.
	TEST_EXPECT(!updater::version_mark(options.runtime_dir).present);
	options.server = "127.0.0.1";
	options.port = server.listener.ports()[0];
	options.check = true;
	updater::Result result;
	TEST_EXPECT(updater::discover_products(options, products, error));
	TEST_EXPECT(updater::run(options, products, result, error) && result.available);
	options.check = false;
	TEST_EXPECT(updater::run(options, products, result, error) && result.applied && result.app &&
			result.launch_exe == "oneditor.exe");
	TEST_EXPECT(fs::exists(options.runtime_dir / "ONEDITOR.CMD") && fs::exists(options.runtime_dir / "version.txt"));
	const updater::VersionMark mark = updater::version_mark(options.runtime_dir);
	TEST_EXPECT(mark.present && mark.version == "1");
	updater::Launch launch;
	TEST_EXPECT(updater::relaunch_command(options, result.app, result.launch_exe, launch, error));
	TEST_EXPECT(launch.program == options.runtime_dir / "oneditor.exe" && launch.args.empty());
	options.check = true;
	TEST_EXPECT(updater::run(options, products, result, error) && !result.available);
	// A runtime folder's OPENNOVA.WIZ (ONRUNTIME) is no app's: its game beside it updates as before.
	TEST_EXPECT(synthetic_trees::write_text(temp.file("runtime/OPENNOVA.WIZ"),
			"[HEADER]\r\nCMD=ONRUNTIME.CMD\r\nURL=update.test.example\r\nPRODUCT_LOGIN=ONRUNTIME\r\n"));
	TEST_EXPECT(updater::parse_options({}, temp.path / "runtime/update.exe", options, error));
	TEST_EXPECT(updater::discover_products(options, products, error) && products.size() == 2);
	TEST_EXPECT(products[0].login == "ONRUNTIME" && !products[0].app && products[1].login == "ACIDGAMBIT");
	return 0;
}

// What /RUN starts once a run is done: opennova.exe with the game and its folder, or the --run-arg
// values in their place; a legacy run's command file's EXE in its install; an app's EXE beside the
// updater, never a path, and never none.
int test_relaunch_command(const test_temp::TempDir &temp) {
	std::string error;
	updater::Options options;
	updater::Launch launch;
	TEST_EXPECT(updater::parse_options({"/game", "jo", "--resource-dir", temp.file("relaunch game"), "/RUN"},
			temp.path / "relaunch runtime/update.exe", options, error));
	TEST_EXPECT(updater::relaunch_command(options, false, "", launch, error));
	TEST_EXPECT(launch.program == options.runtime_dir / "opennova.exe" && launch.args == std::vector<std::string>(
			{"--", "/game", "jo", "--resource-dir", io::utf8_path(options.resource_dir)}));
	// A non-legacy run's command file's EXE is not what it starts: the runtime is.
	TEST_EXPECT(updater::relaunch_command(options, false, "Jointops.exe", launch, error) &&
			launch.program == options.runtime_dir / "opennova.exe");
	options.run_args = {"--", "/game", "jo", "--after-update"};
	TEST_EXPECT(updater::relaunch_command(options, false, "", launch, error) && launch.args == options.run_args);
	// A run without /game relaunches the game without it: the game takes its folder's product,
	// as the run did (a player's game, JO's).
	TEST_EXPECT(updater::parse_options({"--resource-dir", temp.file("relaunch game"), "/RUN"},
			temp.path / "relaunch runtime/update.exe", options, error));
	TEST_EXPECT(updater::relaunch_command(options, false, "", launch, error) && launch.args == std::vector<std::string>(
			{"--", "--resource-dir", io::utf8_path(options.resource_dir)}));
	// The retail game's own call: its command file's EXE in its install, with no arguments of ours.
	TEST_EXPECT(updater::parse_options({"123", "/DIR", temp.file("relaunch retail"), "/RUN"},
			temp.path / "relaunch runtime/update.exe", options, error));
	TEST_EXPECT(updater::relaunch_command(options, false, "Jointops.exe", launch, error));
	TEST_EXPECT(launch.program == options.resource_dir / "Jointops.exe" && launch.args.empty());
	TEST_EXPECT(updater::relaunch_command(options, false, "", launch, error) &&
			launch.program == options.runtime_dir / "opennova.exe" && launch.args.empty());
	TEST_EXPECT(!updater::relaunch_command(options, false, "..\\Jointops.exe", launch, error) && !error.empty());
	// An app: its EXE beside the updater, with the --run-arg values alone.
	TEST_EXPECT(updater::parse_options({"/RUN", "--run-arg", "--verbose"}, temp.path / "relaunch editor/update.exe",
			options, error));
	TEST_EXPECT(updater::relaunch_command(options, true, "oneditor.exe", launch, error));
	TEST_EXPECT(launch.program == options.runtime_dir / "oneditor.exe" &&
			launch.args == std::vector<std::string>({"--verbose"}));
	error.clear();
	TEST_EXPECT(!updater::relaunch_command(options, true, "", launch, error) && !error.empty());
	TEST_EXPECT(!updater::relaunch_command(options, true, "bin/oneditor.exe", launch, error));
	return 0;
}

} // namespace
int main() {
	TEST_EXPECT(net::startup() == 0);
	test_temp::TempDir temp("update_client");
	if (int rc = test_install_connect_timeout()) return rc;
	if (int rc = test_dialog_lines()) return rc;
	if (int rc = test_dialog_progress(temp)) return rc;
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
	if (int rc = test_app_folder(temp)) return rc;
	if (int rc = test_relaunch_command(temp)) return rc;
	std::puts("update_client: ok");
	return 0;
}
