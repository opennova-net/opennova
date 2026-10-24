#include "client.h"

#include "net_sockets.h"

#include <base/io/file_io.h>
#include <base/io/file_time.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <base/resource_index/boot_policy.h>
#include <base/vfs/vfs.h>
#include <formats/pff/pff.h>
#include <formats/update/apply.h>
#include <formats/update/nlz.h>
#include <formats/update/server_list.h>
#include <formats/update/update_crc.h>
#include <formats/update/update_wizard.h>
#include <net/update/update_wire.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>

namespace opennova::updater {
namespace fs = std::filesystem;
namespace {
using Clock = std::chrono::steady_clock;

constexpr const char *kServiceHost = "update.opennova.net";

fs::path named_file(const fs::path &dir, const std::string &name) {
	std::error_code ec;
	const fs::path exact = dir / io::os_path(name);
	if (fs::exists(exact, ec)) return exact;
	for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
		if (strutil::iequals(io::utf8_path(it->path().filename()), name)) return it->path();
	return exact;
}

// The wizard's CMD and PRODUCT_LOGIN after cc.bin's territory substitution.
// [orig: Cupdate2Dlg_ReadWizard @ 0x404c90 (UPDATE.EXE); Cupdate2Dlg_SubstituteTerritory @ 0x404be0 (UPDATE.EXE)]
bool wizard_product(const fs::path &dir, const fs::path &wizard_file,
		Product &product, std::string &error) {
	std::string text;
	if (!io::read_file_text(io::utf8_path(wizard_file), text, error)) return false;
	std::vector<uint8_t> cc;
	std::string ignored;
	(void)io::read_file_bytes(io::utf8_path(named_file(dir, "cc.bin")), cc, ignored);
	const update::UpdateWizard wizard = update::with_territory(update::read_update_wizard(text),
			update::territory_from_cc_bin(cc.data(), cc.size()));
	if (wizard.product_login.empty() || !update::is_safe_row_name(wizard.cmd)) {
		error = "update wizard needs PRODUCT_LOGIN and a safe CMD: " + io::utf8_path(wizard_file);
		return false;
	}
	if (wizard.product_login.find_first_of(" \t\r\n") != std::string::npos) {
		error = "invalid update product login"; return false;
	}
	product.dir = dir;
	product.command = wizard.cmd;
	product.login = wizard.product_login;
	product.native = product.native || strutil::iequals(product.login, "ONRUNTIME") ||
			strutil::iequals(product.login, "OPENNOVA");
	return true;
}

// Every product updates from OpenNova's service: the runtime's OPENNOVA.WIZ URLs, else
// update.opennova.net; a product's own wizard URLs are never dialed. Retail overwrites its
// wizard's URLs with the dead ftp*.novaworld.com hosts (updater-re section 3; D-UPD-2).
std::vector<std::string> service_gateways(const fs::path &runtime_dir) {
	std::vector<std::string> hosts;
	std::string text, ignored;
	if (io::read_file_text(io::utf8_path(named_file(runtime_dir, "OPENNOVA.WIZ")), text, ignored)) {
		const update::UpdateWizard wizard = update::read_update_wizard(text);
		for (const std::string &host : {wizard.url, wizard.url2, wizard.url3})
			if (!host.empty()) hosts.push_back(host);
	}
	if (hosts.empty()) hosts.push_back(kServiceHost);
	return hosts;
}

// The update server a SERVER.LST row names, as dialed: the service names retail's
// ftp.novaworld.com for the products a stock UPDATE.EXE reaches through the Switcher's hosts
// block; this client reaches that same service at the gateway it came through instead.
std::string dialed_server(const std::string &listed, const std::string &gateway) {
	const std::string host = strutil::to_lower(strutil::trim(listed));
	const std::string_view domain = "novaworld.com";
	const bool novaworld = host == domain ||
			(host.size() > domain.size() && strutil::ends_with_icase(host, domain) &&
					host[host.size() - domain.size() - 1] == '.');
	return novaworld ? gateway : listed;
}

struct Worker {
	const Options &options;
	const ProgressSink &sink;
	const std::atomic_bool *cancel;
	Clock::time_point deadline;
	std::string &error;
	bool interrupted() {
		if (cancel && cancel->load()) { error = "Update aborted!"; return true; }
		if (options.check && Clock::now() >= deadline) { error = "Update check timed out."; return true; }
		return false;
	}
	void status(const std::string &text, const std::string &file = "", uint64_t n = 0, uint64_t total = 0) {
		if (sink) sink({text, file, n, total});
	}
	int connect_timeout() const {
		if (!options.check) return 3000;
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
		return static_cast<int>(std::max<int64_t>(1, std::min<int64_t>(remaining, 2000)));
	}
	// [orig: UpdateSocket_Connect @ 0x403570 (UPDATE.EXE)]: a failed host or port is passed over.
	bool connect(const std::string &host, uint16_t port, net::ScopedSocket &socket) {
		if (interrupted()) return false;
		for (net::Endpoint endpoint : net::resolve_ipv4_all(host)) {
			if (interrupted()) return false;
			endpoint.port = port;
			socket = net::ScopedSocket(net::tcp_connect(endpoint, connect_timeout()));
			if (socket.is_valid()) return true;
		}
		error = "Unable to open update server " + host + ":" + std::to_string(port);
		return false;
	}
	// A reply's bytes. The 90 s silence limit restarts whenever bytes arrive.
	// [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE), US_WAIT_REPLY's 0x15F90 ms and US_FETCH_WAIT]
	bool receive(net::Socket &socket, uint8_t *out, size_t size, const std::string &file = "") {
		size_t received = 0;
		auto last = Clock::now();
		while (received < size) {
			if (interrupted()) return false;
			const int ready = net::tcp_wait_readable(socket, 100);
			if (ready < 0 || Clock::now() - last > std::chrono::seconds(90)) {
				error = "The connection to the server has timed out."; return false;
			}
			if (ready == 0) continue;
			const int count = net::tcp_recv(socket, out + received, std::min<size_t>(size - received, 65536));
			if (count <= 0) { error = "The server closed an incomplete update reply."; return false; }
			received += static_cast<size_t>(count);
			last = Clock::now();
			if (!file.empty()) status("Fetching files...", file, received, size);
		}
		return true;
	}
	// One request and its reply, which must echo the request's verb and id; a GET reply must
	// name the file asked for (D-UPD-3). [orig: NLUpdateProtocol_ParseReplyHeader @ 0x4011d0
	// (UPDATE.EXE); NLUpdateProtocol_DispatchReply @ 0x4017a0 (UPDATE.EXE)]
	bool exchange(net::Socket &socket, const std::string &request, update::Verb verb,
			int32_t id, uint64_t limit, update::Reply &reply, const std::string &name = "") {
		if (interrupted()) return false;
		if (!net::tcp_send_all(socket, reinterpret_cast<const uint8_t *>(request.data()), request.size())) {
			error = "Unable to send update request."; return false;
		}
		std::vector<uint8_t> header;
		size_t consumed = 0;
		while (true) {
			uint8_t byte = 0;
			if (!receive(socket, &byte, 1)) return false;
			header.push_back(byte);
			const update::Frame state = update::decode_reply_header(header.data(), header.size(), reply, consumed);
			if (state == update::Frame::Malformed) { error = "Malformed update reply."; return false; }
			if (state == update::Frame::Complete) break;
		}
		if (reply.verb != verb || reply.id != id || reply.result < 0 ||
				static_cast<uint64_t>(reply.size) > limit ||
				(verb == update::Verb::Get && (!update::is_safe_row_name(reply.name) ||
						!strutil::iequals(reply.name, name)))) {
			error = "Update server refused the request or returned an unexpected reply: " + name;
			return false;
		}
		reply.payload.resize(static_cast<size_t>(reply.size));
		return receive(socket, reply.payload.data(), reply.payload.size(), name);
	}
	bool login(net::Socket &socket, const Product &product, int32_t id) {
		update::Reply reply;
		return exchange(socket, update::encode_login(product.login, "", id), update::Verb::Login, id, 0, reply);
	}
	// The gateway's LOGIN (id -4) and SERVER.LST (id -3), then the update server's LOGIN (id -2)
	// on the gateway's port. Every listed server is a failover candidate, with no LOADTEST
	// ranking (D-UPD-8). [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE), states 1-10;
	// Cupdate2Dlg_ReadServerList @ 0x408070 (UPDATE.EXE)]
	bool open_product(const Product &product, net::ScopedSocket &out) {
		std::vector<std::string> hosts = options.server.empty() ? product.gateways : std::vector<std::string>{options.server};
		if (hosts.empty()) hosts.push_back(kServiceHost);
		std::vector<uint16_t> ports;
		if (options.port) ports.push_back(options.port);
		else ports.assign(std::begin(update::kGatewayPorts), std::end(update::kGatewayPorts));
		for (const std::string &host : hosts) for (uint16_t port : ports) {
			if (interrupted()) return false;
			status("Locating Gateway Server...", host);
			net::ScopedSocket gateway;
			if (!connect(host, port, gateway) || !login(gateway.get(), product, update::kIdGatewayLogin)) continue;
			update::Reply list;
			if (!exchange(gateway.get(), update::encode_get({"SERVER.LST"}, update::kIdServerList),
						update::Verb::Get, update::kIdServerList, 65536, list, "SERVER.LST")) continue;
			const std::vector<std::string> servers = update::read_server_list(
					std::string(list.payload.begin(), list.payload.end()));
			if (servers.empty() || servers.size() > 64) { error = "Unable to locate an update server."; continue; }
			const std::string close = update::encode_close();
			(void)net::tcp_send_all(gateway.get(), reinterpret_cast<const uint8_t *>(close.data()), close.size());
			// Every advertised server is a failover candidate. A single-server gateway, the
			// deployed service's form, needs no LOADTEST round trip (updater-re section 5).
			for (const std::string &listed : servers) {
				const std::string server = dialed_server(listed, host);
				status("Opening update server...", server);
				if (connect(server, port, out) && login(out.get(), product, update::kIdServerLogin)) return true;
			}
		}
		return false;
	}
};

// Whether a command row still has work: a loose file missing, empty, of another size (row
// size 0 skips) or another file CRC (row crc 0 skips); an archive entry missing, more than 2 s
// from the row's time, or of another checksum when both are set; a REMOVE whose file or entry
// is there. [orig: Cupdate2Dlg_PrecheckCommands @ 0x407820 (UPDATE.EXE)]
struct Precheck {
	const Product &product;
	explicit Precheck(const Product &p) : product(p) {}
	pff::PffArchive archive{};
	std::string opened;
	bool have_archive = false;
	void close() { if (have_archive) pff::pff_close(&archive); have_archive = false; }
	~Precheck() { close(); }
	bool needed(const update::CommandRow &row, const std::string &target) {
	std::error_code ec;
	if (strutil::ends_with_icase(target, ".pff")) {
		if (opened != target) {
			if (have_archive) pff::pff_close(&archive);
			archive = {};
			opened = target;
			have_archive = pff::pff_open(&archive, io::utf8_path(named_file(product.dir, target)).c_str()) == 0;
		}
		if (!have_archive) return row.type != update::CommandType::Remove;
		const pff::PffEntry *entry = pff::pff_find(&archive, row.name.c_str());
		bool needed = row.type == update::CommandType::Remove ? entry != nullptr : entry == nullptr;
		if (entry && row.type == update::CommandType::Copy)
			needed = std::llabs(int64_t(entry->timestamp) - row.time) > 2 ||
					(entry->checksum && row.crc && *row.crc && entry->checksum != static_cast<uint32_t>(*row.crc));
		return needed;
	}
	const fs::path path = named_file(target == "." ? product.dir : product.dir / io::os_path(target), row.name);
	const bool exists = fs::is_regular_file(path, ec);
	if (row.type == update::CommandType::Remove) return fs::exists(path, ec);
	if (!exists) return true;
	const uint64_t size = fs::file_size(path, ec);
	if (ec || size == 0 || (row.size && size != static_cast<uint64_t>(row.size))) return true;
	if (!row.crc || *row.crc == 0) return false;
	std::vector<uint8_t> bytes;
	std::string error;
	return !io::read_file_bytes(io::utf8_path(path), bytes, error) ||
			update::file_crc(bytes.data(), bytes.size()) != static_cast<uint32_t>(*row.crc);
	}
};

// One product: the command file's CRC against the local copy's (equal: keep it, else GET it),
// the rows the precheck finds, fetched, then installed in one pass, update.exe landing as
// update.new for revupdat (D-UPD-4). The command file is written only once every row is in
// (D-UPD-5). [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE), states 11-16;
// Cupdate2Dlg_InstallLooseRows @ 0x406860 (UPDATE.EXE)]
bool update_product(Worker &worker, const Product &product, Result &result) {
	if (!update::is_safe_row_name(product.command)) { worker.error = "Unsafe command file name."; return false; }
	net::ScopedSocket socket;
	if (!worker.open_product(product, socket)) return false;
	worker.status("Checking for update...", product.login);
	update::Reply remote;
	if (!worker.exchange(socket.get(), update::encode_crc(product.command, update::kIdCommandFile),
				update::Verb::Crc, update::kIdCommandFile, 0, remote)) return false;
	std::vector<uint8_t> command_bytes;
	std::string ignored;
	const fs::path command_path = named_file(product.dir, product.command);
	const uint32_t remote_crc = static_cast<uint32_t>(remote.crc);
	const bool cached = io::read_file_bytes(io::utf8_path(command_path), command_bytes, ignored) &&
			update::command_file_crc(command_bytes.data(), command_bytes.size()) == remote_crc;
	if (!cached) {
		if (!worker.exchange(socket.get(), update::encode_get({product.command}, update::kIdCommandFile),
					update::Verb::Get, update::kIdCommandFile, 1024 * 1024, remote, product.command)) return false;
		command_bytes = std::move(remote.payload);
		if (update::command_file_crc(command_bytes.data(), command_bytes.size()) != remote_crc) {
			worker.error = "The command file did not match the server's CRC. Please retry."; return false;
		}
	}
	const auto command = update::read_command_file(std::string(command_bytes.begin(), command_bytes.end()));
	if (!command || !update::validate_commands(*command, worker.error)) {
		if (!command) worker.error = "Error processing CMD file: " + product.command;
		return false;
	}
	if (worker.options.legacy && result.launch_exe.empty()) result.launch_exe = command->exe;
	update::CommandFile selected = *command;
	selected.rows.clear();
	std::string target = ".";
	std::map<std::string, update::SfsEntry> blobs;
	bool revised = false;
	std::error_code revision_error;
	const bool pending_revision = product.dir == worker.options.runtime_dir &&
			fs::is_regular_file(named_file(product.dir, "update.new"), revision_error);
	Precheck precheck{product};
	for (size_t i = 0; i < command->rows.size(); ++i) {
		update::CommandRow row = command->rows[i];
		if (row.type == update::CommandType::Target) {
			target = row.name; selected.rows.push_back(row); continue;
		}
		// A new native command file installs every row (D-UPD-6). This avoids treating the retail
		// defective CRC as a full-file digest when code/PCK bytes changed beyond its prefix.
		const bool updater_image = row.type == update::CommandType::Loose && target == "." &&
				strutil::iequals(row.name, "update.exe") && product.dir == worker.options.runtime_dir;
		// A failed helper launch leaves update.new behind. Retry the handoff even if
		// the cached command and the old image's defective prefix CRC look current.
		if (!(pending_revision && updater_image) &&
				!(!cached && product.native && row.type != update::CommandType::Remove) &&
				!precheck.needed(row, target)) continue;
		result.available = true;
		++result.files;
		if (worker.options.check) continue;
		if (row.type == update::CommandType::Loose || row.type == update::CommandType::Copy) {
			if (worker.interrupted()) return false;
			if (!blobs.count(strutil::to_lower(row.name))) {
				update::Reply reply;
				const uint64_t bound = uint64_t(row.size) + uint64_t(row.size) / 100 + 65536;
				if (!worker.exchange(socket.get(), update::encode_get({row.name}, int32_t(i)),
							update::Verb::Get, int32_t(i), bound, reply, row.name)) return false;
				blobs[strutil::to_lower(row.name)] = {row.name, std::move(reply.payload)};
			}
			if (updater_image) {
				auto &blob = blobs[strutil::to_lower(row.name)];
				blob.name = "update.new";
				row.name = "update.new";
				revised = true;
			}
		}
		selected.rows.push_back(row);
	}
	++result.products;
	if (worker.options.check) return true;
	if (worker.interrupted()) return false;
	precheck.close(); // Windows must release the source archive before replacement.
	update::SfsSection section;
	const std::string selected_text = update::write_command_file(selected);
	if (selected_text.empty()) { worker.error = "Cannot prepare update commands."; return false; }
	section.entries.push_back({product.command, std::vector<uint8_t>(selected_text.begin(), selected_text.end())});
	for (auto &[key, blob] : blobs) section.entries.push_back(std::move(blob));
	worker.status("Integrating update...", product.login);
	update::ApplyReport report;
	if (!update::apply_section(section, product.command, product.dir, report, worker.error)) return false;
	// Persist the remote command only after every selected row succeeded. An interrupted
	// first install therefore checks/fetches again, rather than becoming a false no-op.
	if (!io::write_file_atomic(io::utf8_path(command_path), command_bytes.data(), command_bytes.size(), worker.error))
		return false;
	result.applied = result.applied || !report.files_written.empty() || !report.archives_rewritten.empty() || !report.files_deleted.empty();
	result.revision_pending = result.revision_pending || revised;
	return true;
}
} // namespace

// The runtime, the selected game and every expansion/<name>/ with an UPDATE.WIZ, each a product
// of its own. [orig: Cupdate2Dlg_RunExpansionUpdates @ 0x407f10 (UPDATE.EXE);
// Cupdate2Dlg_RunWizardsIn @ 0x406290 (UPDATE.EXE)]
bool discover_products(const Options &o, std::vector<Product> &out, std::string &error) {
	out.clear();
	std::error_code ec;
	if (!o.no_runtime) {
		Product runtime{o.runtime_dir, "ONRUNTIME", "ONRUNTIME.CMD", {}, true};
		const fs::path wizard = named_file(o.runtime_dir, "OPENNOVA.WIZ");
		if (fs::exists(wizard, ec) && !wizard_product(o.runtime_dir, wizard, runtime, error)) return false;
		out.push_back(std::move(runtime));
	}
	// The folder's own UPDATE.WIZ names what it holds (boot_policy.h folder_product, ADR 0067
	// d2). A launch without /game takes that product, and a product the folder does not hold
	// is refused, so OpenNova content never lands over another game's data. A legacy launch
	// (the retail game's own call) keeps the stock wizard.
	bool native_content = false;
	if (!o.legacy) {
		const std::string folder = folder_product(io::utf8_path(o.resource_dir));
		if (folder == kForeignFolder) {
			error = "This folder's UPDATE.WIZ names a game OpenNova does not update.";
			return false;
		}
		const std::string product = o.game_explicit ? o.game : (folder.empty() ? std::string("opennova") : folder);
		if (product == "opennova") {
			if (!folder.empty() && folder != "opennova") {
				error = "This folder holds another game (its UPDATE.WIZ). OpenNova content installs only into its own folder.";
				return false;
			}
			// No wizard: a fresh folder, never one already holding game archives (a DFX
			// install ships none). OpenNova's own content writes its wizard first (its
			// loose rows precede the archives), so an interrupted install resumes.
			const auto slots = vfs_boot_archive_slots(io::utf8_path(o.resource_dir));
			if (folder.empty() && std::any_of(slots.begin(), slots.end(), [](const std::string &n) { return !n.empty(); })) {
				error = "This folder holds game data without OpenNova's UPDATE.WIZ. OpenNova content installs only into its own folder.";
				return false;
			}
			native_content = true;
		} else if (folder != product) {
			error = "This folder's UPDATE.WIZ does not name /game " + product + ".";
			return false;
		}
	}
	if (native_content) {
		out.push_back({o.resource_dir, "OPENNOVA", "OPENNOVA.CMD", {}, true});
	} else {
		Product retail;
		if (!wizard_product(o.resource_dir, named_file(o.resource_dir, "UPDATE.WIZ"), retail, error)) return false;
		native_content = retail.native;
		out.push_back(std::move(retail));
	}
	const fs::path expansions = named_file(o.resource_dir, "expansion");
	std::vector<fs::path> dirs;
	for (fs::directory_iterator it(expansions, ec), end; !ec && it != end; it.increment(ec))
		if (it->is_directory(ec)) dirs.push_back(it->path());
	std::sort(dirs.begin(), dirs.end());
	for (const fs::path &dir : dirs) {
		const fs::path wizard = named_file(dir, "UPDATE.WIZ");
		if (!fs::is_regular_file(wizard, ec)) continue;
		// A retail game's expansion needs its archive installed, where retail needs only the wizard
		// (D-UPD-9); a native content's registers by its wizard alone.
		if (!native_content && !fs::is_regular_file(named_file(dir, io::utf8_path(dir.filename()) + ".pff"), ec)) continue;
		Product registered;
		if (!wizard_product(dir, wizard, registered, error)) return false;
		registered.native = registered.native || native_content;
		registered.expansion = true;
		out.push_back(std::move(registered));
	}
	const std::vector<std::string> gateways = service_gateways(o.runtime_dir);
	for (Product &product : out) product.gateways = gateways;
	return true;
}

bool run(const Options &options, const std::vector<Product> &products, Result &result,
		std::string &error, const ProgressSink &progress, const std::atomic_bool *cancel) {
	result = {};
	error.clear();
	if (net::startup() != 0) { error = "Unable to initialize networking."; return false; }
	Worker worker{options, progress, cancel, Clock::now() + std::chrono::seconds(10), error};
	std::string failure;
	for (const Product &product : products) {
		if (!update_product(worker, product, result)) {
			// A check (D-UPD-7) goes on: one dead or unpublished product (a registered expansion's) must
			// not hide another product's update.
			if (options.check) {
				if (failure.empty()) failure = error;
				continue;
			}
			// An install stops at a failing runtime or game. An expansion's failure is its own (D-UPD-12):
			// retail runs each as a nested updater whose exit it ignores, and goes on to the next
			// and to the relaunch. [orig: Cupdate2Dlg_RunWizardsIn @ 0x406290 (UPDATE.EXE), the
			// exit code dropped @0x40633b; Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE), US_DONE]
			if (!product.expansion) return false;
			result.expansion_failures.push_back({product.dir, error});
			error.clear();
			continue;
		}
		// Let the helper replace this process before continuing with content. A
		// later product failure must not strand an already staged updater revision.
		if (result.revision_pending) break;
	}
	// A check with an update reports it; one with none fails when any product could not be
	// checked, since that product may have one.
	if (!result.available && !failure.empty()) {
		error = failure;
		return false;
	}
	error.clear();
	worker.status(result.available ? "Operation completed successfully!" : "No Update Needed!");
	return true;
}
} // namespace opennova::updater
