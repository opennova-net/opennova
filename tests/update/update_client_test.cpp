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
#include <thread>

using namespace opennova;
namespace fs = std::filesystem;
namespace {

struct Server {
	explicit Server(std::vector<std::string> hosts = {"127.0.0.1"}) : catalog(std::move(hosts)) {}
	update_tool::PackageCatalog catalog;
	update::UpdateServer server{catalog};
	net::UpdateTcpServer listener{server};
	std::atomic_bool stop{false};
	std::thread thread;
	bool start(const update::SfsPackage &package, std::string &error) {
		if (!catalog.bind("TEST", "TEST.CMD", {package}, error) || !listener.listen({0}, true)) return false;
		thread = std::thread([this]() {
			while (!stop.load()) {
				if (!listener.pump()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		});
		return true;
	}
	~Server() { stop.store(true); if (thread.joinable()) thread.join(); }
};

update::SfsPackage package(const std::string &text, bool corrupt = false, bool self = false) {
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
	section.entries.push_back({"TEST.CMD", synthetic_trees::bytes(cmd)});
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
	options.check = false;
	TEST_EXPECT(updater::run(options, products, result, error));
	TEST_EXPECT(result.available && result.applied && fs::exists(options.resource_dir / "resource.pff"));
	std::string installed;
	TEST_EXPECT(io::read_file_text(io::utf8_path(options.resource_dir / "version.txt"), installed, error));
	TEST_EXPECT(installed == "First complete content release");
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
	TEST_EXPECT(!updater::run(options, products, result, error)); // an install stops at the first
	TEST_EXPECT(updater::run(options, {products[1]}, result, error) && result.applied);
	options.check = true;
	TEST_EXPECT(!updater::run(options, products, result, error) && !result.available && !error.empty());
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

int test_options_and_discovery(const test_temp::TempDir &temp) {
	updater::Options options;
	std::string error;
	TEST_EXPECT(updater::parse_options({"--check", "/game", "DFX2", "--runtime-dir", temp.root(),
			"--resource-dir", temp.file("retail"), "/RUN", "--run-arg", "--resource-dir", "--run-arg", "a b"},
			temp.path / "update.exe", options, error));
	TEST_EXPECT(options.check && options.game == "dfx2" && options.run && options.run_args == std::vector<std::string>({"--resource-dir", "a b"}));
	TEST_EXPECT(!updater::parse_options({"--port", "0"}, temp.path / "update.exe", options, error));
	TEST_EXPECT(!updater::parse_options({"--unknown"}, temp.path / "update.exe", options, error));
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
	// Retail expansions still need their installed archive, so copying a wizard alone
	// cannot install a retail expansion.
	TEST_EXPECT(synthetic_trees::write_text(temp.file("retail/expansion/testexp/UPDATE.WIZ"),
			"[HEADER]\r\nCMD=EXP.CMD\r\nPRODUCT_LOGIN=EXP\r\nURL=127.0.0.1\r\n"));
	TEST_EXPECT(updater::discover_products(options, products, error) && products.size() == 1);
	// OpenNova content never lands in a folder whose wizard names another game, even when
	// /game asks for it; nor does another product the wizard does not name.
	TEST_EXPECT(updater::parse_options({"/game", "opennova", "--resource-dir", temp.file("retail")},
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
	TEST_EXPECT(updater::parse_options({"/game", "opennova"}, temp.path / "wizardless/update.exe", options, error));
	TEST_EXPECT(!updater::discover_products(options, products, error));
	std::error_code made;
	fs::create_directories(temp.path / "fresh", made);
	TEST_EXPECT(updater::parse_options({}, temp.path / "fresh/update.exe", options, error));
	TEST_EXPECT(updater::discover_products(options, products, error) && products.size() == 2);
	TEST_EXPECT(products[0].login == "ONRUNTIME" && products[1].login == "OPENNOVA" && products[1].native);
	// A completed native install also has UPDATE.WIZ. Bare launch must retain
	// full native rows and recognize its registered expansion without an archive.
	TEST_EXPECT(synthetic_trees::write_text(temp.file("native/UPDATE.WIZ"),
			"[HEADER]\r\nCMD=OPENNOVA.CMD\r\nPRODUCT_LOGIN=OPENNOVA\r\n"));
	TEST_EXPECT(synthetic_trees::write_text(temp.file("native/expansion/onjo1/UPDATE.WIZ"),
			"[HEADER]\r\nCMD=ONJO1.CMD\r\nPRODUCT_LOGIN=ONJO1\r\n"));
	TEST_EXPECT(updater::parse_options({}, temp.path / "native/update.exe", options, error));
	TEST_EXPECT(updater::discover_products(options, products, error) && products.size() == 3);
	TEST_EXPECT(products[0].login == "ONRUNTIME" && products[1].login == "OPENNOVA" && products[1].native &&
			products[2].login == "ONJO1" && products[2].native);
	return 0;
}

} // namespace
int main() {
	TEST_EXPECT(net::startup() == 0);
	test_temp::TempDir temp("update_client");
	if (int rc = test_first_install(temp)) return rc;
	if (int rc = test_retail_server_list(temp)) return rc;
	if (int rc = test_check_goes_past_a_failing_product(temp)) return rc;
	if (int rc = test_failures(temp)) return rc;
	if (int rc = test_revision(temp)) return rc;
	if (int rc = test_options_and_discovery(temp)) return rc;
	std::puts("update_client: ok");
	return 0;
}
