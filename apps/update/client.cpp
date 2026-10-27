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
#include <formats/update/command_file.h>
#include <formats/update/nlz.h>
#include <formats/update/server_list.h>
#include <formats/update/update_crc.h>
#include <formats/update/update_wizard.h>
#include <net/update/update_wire.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <new>
#include <set>

namespace opennova::updater {
namespace fs = std::filesystem;
namespace {
using Clock = std::chrono::steady_clock;

// Prod's update service, dialed only when no OPENNOVA.WIZ names a gateway. The game zip
// always carries that wizard, naming its export mode's deployment (prod's in a release
// package, staging's in a debug one: scripts/package_godot_windows.ps1), so one updater
// build serves both flavours.
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
	// OpenNova's own products install every row of a new release (D-UPD-6): the runtime and an
	// app (native from their caller), and the games whose wizard names OP: Acid Gambit's CMD or
	// a player's game (ADR 0074).
	product.native = product.native || update::wizard_product(wizard).native;
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
// ftp.novaworld.com for the products a stock UPDATE.EXE reaches through a hosts-file line;
// this client reaches that same service at the gateway it came through instead.
std::string dialed_server(const std::string &listed, const std::string &gateway) {
	const std::string host = strutil::to_lower(strutil::trim(listed));
	const std::string_view domain = "novaworld.com";
	const bool novaworld = host == domain ||
			(host.size() > domain.size() && strutil::ends_with_icase(host, domain) &&
					host[host.size() - domain.size() - 1] == '.');
	return novaworld ? gateway : listed;
}

// The protocol's close: CLOSE sent, then the socket closed; nothing when it is closed already.
// Retail ends every connection so: the gateway's once its server list has come (before the
// list is read) and when its login or its list fails, the update server's once its fetches
// are done and on a failure.
// [orig: NLUpdateProtocol_Close @ 0x401660 (UPDATE.EXE); Cupdate2Dlg_StateTick @ 0x408170
//  (UPDATE.EXE): OPEN_GATEWAY @0x40838e (after a refused login), LOADBALANCE @0x4085b1,
//  PACK @0x4089c6, ERROR @0x408b56]
void close_protocol(net::ScopedSocket &socket) {
	if (!socket.is_valid()) return;
	const std::string close = update::encode_close();
	(void)net::tcp_send_all(socket.get(), reinterpret_cast<const uint8_t *>(close.data()), close.size());
	socket = net::ScopedSocket();
}

// A connection closed as retail's protocol closes it, however its step ends.
struct ProtocolClose {
	net::ScopedSocket &socket;
	~ProtocolClose() { close_protocol(socket); }
};

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
	Progress shown; // the dialog as this run has written it
	void show() {
		if (sink) sink(shown);
	}
	void status(const std::string &text) {
		shown.status = text;
		show();
	}
	int connect_timeout() const {
		if (!options.check) return kInstallConnectTimeoutMs;
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
	bool receive(net::Socket &socket, uint8_t *out, size_t size) {
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
		}
		return true;
	}
	// One request and its reply's header, which must echo the request's verb and id; a GET reply
	// must name the file asked for (D-UPD-3). The body is the caller's to read: `reply.size` bytes.
	// [orig: NLUpdateProtocol_ParseReplyHeader @ 0x4011d0 (UPDATE.EXE); NLUpdateProtocol_DispatchReply
	// @ 0x4017a0 (UPDATE.EXE)]
	bool request(net::Socket &socket, const std::string &text, update::Verb verb,
			int32_t id, uint64_t limit, update::Reply &reply, const std::string &name = "") {
		if (interrupted()) return false;
		if (!net::tcp_send_all(socket, reinterpret_cast<const uint8_t *>(text.data()), text.size())) {
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
		return true;
	}
	// One request and its whole reply, the body held: the small replies (a login, a CRC, SERVER.LST,
	// the command file).
	bool exchange(net::Socket &socket, const std::string &text, update::Verb verb,
			int32_t id, uint64_t limit, update::Reply &reply, const std::string &name = "") {
		if (!request(socket, text, verb, id, limit, reply, name)) return false;
		reply.payload.resize(static_cast<size_t>(reply.size));
		return receive(socket, reply.payload.data(), reply.payload.size());
	}
	// A row's GET, its reply's body written to `path` as it arrives, a buffer at a time, so a whole
	// game's release is never held in memory. [orig: NLUpdateProtocol_OnGetReply @ 0x4013e0
	// (UPDATE.EXE): the payload written to <download dir>\<name> ("wb")]
	bool fetch(net::Socket &socket, const std::string &name, int32_t id, uint64_t limit, const fs::path &path) {
		update::Reply reply;
		if (!request(socket, update::encode_get({name}, id), update::Verb::Get, id, limit, reply, name))
			return false;
		std::FILE *file = io::fopen_utf8(io::utf8_path(path).c_str(), "wb");
		if (file == nullptr) { error = "Cannot write " + io::utf8_path(path) + "."; return false; }
		std::vector<uint8_t> buffer(64 * 1024);
		const uint64_t total = static_cast<uint64_t>(reply.size);
		uint64_t done = 0;
		bool ok = true;
		while (ok && done < total) {
			const size_t count = static_cast<size_t>(std::min<uint64_t>(total - done, buffer.size()));
			ok = receive(socket, buffer.data(), count);
			if (ok && std::fwrite(buffer.data(), 1, count, file) != count) {
				error = "Cannot write " + io::utf8_path(path) + ".";
				ok = false;
			}
			done += count;
		}
		if (std::fclose(file) != 0 && ok) {
			error = "Cannot write " + io::utf8_path(path) + ".";
			ok = false;
		}
		return ok;
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
		for (size_t index = 0; index < hosts.size(); ++index) for (uint16_t port : ports) {
			if (interrupted()) return false;
			const std::string &host = hosts[index];
			// Each gateway state's status line names the gateway by its place among them and its
			// port. [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE): OPEN_GATEWAY @0x40840d,
			// LOGIN_GATEWAY @0x4084a5, FETCH_LST @0x408566]
			char where[48];
			std::snprintf(where, sizeof where, "\n(Server %d, Port %x)   ", int(index + 1), unsigned(port));
			status(std::string("Locating Gateway Server...") + where);
			net::ScopedSocket gateway;
			const ProtocolClose gateway_closing{gateway};
			if (!connect(host, port, gateway)) continue;
			status(std::string("Logging into Gateway Server...") + where);
			if (!login(gateway.get(), product, update::kIdGatewayLogin)) continue;
			status(std::string("Fetching server list...") + where);
			update::Reply list;
			if (!exchange(gateway.get(), update::encode_get({"SERVER.LST"}, update::kIdServerList),
						update::Verb::Get, update::kIdServerList, 65536, list, "SERVER.LST")) continue;
			close_protocol(gateway);
			const std::vector<std::string> servers = update::read_server_list(
					std::string(list.payload.begin(), list.payload.end()));
			if (servers.empty() || servers.size() > 64) { error = "Unable to locate an update server."; continue; }
			// Every advertised server is a failover candidate. A single-server gateway, the
			// deployed service's form, needs no LOADTEST round trip (updater-re section 5).
			for (const std::string &listed : servers) {
				const std::string server = dialed_server(listed, host);
				// [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE): OPEN_SERVER @0x40863a,
				// LOGIN_SERVER @0x408699]
				char opening[48];
				std::snprintf(opening, sizeof opening, "Opening update server...\n(Port %x)   ", unsigned(port));
				status(opening);
				if (connect(server, port, out)) {
					status("Logging into remote server...");
					if (login(out.get(), product, update::kIdServerLogin)) return true;
				}
				close_protocol(out); // a refused login, as retail's ERROR closes it
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

// A product's download directory, update\ beside its files, where the rows' GETs land as
// UPDATE.EXE's do: made at the first fetch, as the protocol's download directory is set to
// "update" and made at FETCH_FILES, and deleted with what it holds once the product is
// done, as US_DONE deletes it. A failed run deletes it too, where retail leaves it for its
// next precheck's staged-file lookups, which ours has none of (D-UPD-4); a product folder
// the first fetch made, and nothing filled, goes with it.
// [orig: NLUpdateProtocol_SetDownloadDir @ 0x401580 (UPDATE.EXE), "update" from
//  Cupdate2Dlg_StateTick @ 0x408920; Update_DeleteWorkDir @ 0x405100 (UPDATE.EXE),
//  SHFileOperation FO_DELETE of "update", from US_DONE @ 0x408ab3]
struct DownloadDir {
	fs::path path;
	fs::path product;
	bool made = false;
	bool product_made = false;
	bool make(std::string &error) {
		if (made) return true;
		std::error_code ec;
		product_made = !fs::exists(product, ec);
		fs::create_directories(path, ec);
		if (ec) { error = "Cannot create " + io::utf8_path(path) + "."; return false; }
		made = true;
		return true;
	}
	~DownloadDir() {
		if (!made) return;
		std::error_code ec;
		fs::remove_all(path, ec);
		if (product_made) fs::remove(product, ec); // only while it is empty
	}
};

// One product: the command file's CRC against the local copy's (equal: keep it, else GET it),
// the rows the precheck finds, then fetched into the download directory, then installed in one
// pass from there, update.exe landing as update.new for revupdat (D-UPD-4). The command file is
// written only once every row is in (D-UPD-5). Its dialog is a run's of its own, written as
// retail's states write theirs (Progress). [orig: Cupdate2Dlg_StateTick @ 0x408170
// (UPDATE.EXE), states 11-16; Cupdate2Dlg_InstallLooseRows @ 0x406860 (UPDATE.EXE)]
bool update_product(Worker &worker, const Product &product, Result &result) {
	if (!update::is_safe_row_name(product.command)) { worker.error = "Unsafe command file name."; return false; }
	// START: the dialog afresh, its clock from now. [orig: Cupdate2Dlg_StateTick @ 0x408170
	// (UPDATE.EXE), US_START @0x4081d1, the start tick @0x4081ee]
	worker.shown = Progress{};
	worker.shown.start = Clock::now();
	worker.status("Processing wizard file...");
	net::ScopedSocket socket;
	const ProtocolClose closing{socket};
	if (!worker.open_product(product, socket)) return false;
	// [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE): CHECK_UPDATE @0x4086f7, FETCH_UPDATE
	// @0x408747, FETCH_FILES @0x4088b4 and @0x4088f5]
	worker.status("Checking for update...");
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
		worker.status("Fetching command file...");
		if (!worker.exchange(socket.get(), update::encode_get({product.command}, update::kIdCommandFile),
					update::Verb::Get, update::kIdCommandFile, 1024 * 1024, remote, product.command)) return false;
		command_bytes = std::move(remote.payload);
		if (update::command_file_crc(command_bytes.data(), command_bytes.size()) != remote_crc) {
			worker.error = "The command file did not match the server's CRC. Please retry."; return false;
		}
	}
	worker.status("Processing command file...");
	const auto command = update::read_command_file(std::string(command_bytes.begin(), command_bytes.end()));
	if (!command || !update::validate_commands(*command, worker.error)) {
		if (!command) worker.error = "Error processing CMD file: " + product.command;
		return false;
	}
	// The product line, as the command file is read: its PRODUCT, else retail's words for none.
	// [orig: Cupdate2Dlg_ReadCommandFile @ 0x4076e0 (UPDATE.EXE), the default @0x407706, the line
	// @0x407794]
	worker.shown.product = command->product.empty() ? "Your NovaLogic Product" : command->product;
	worker.status("Checking file versions...");
	// /RUN starts a legacy run's and an app's own EXE (relaunch_command).
	if ((worker.options.legacy || product.app) && result.launch_exe.empty()) result.launch_exe = command->exe;
	result.app = result.app || product.app;
	update::CommandFile selected = *command;
	selected.rows.clear();
	std::string target = ".";
	DownloadDir download{product.dir / "update", product.dir};
	// The files fetched, by the GET's name (lower case), and each selected row's file by the row's
	// name as the install reads it (lower case): update.exe's row reads as update.new from
	// update\update.exe.
	std::map<std::string, fs::path> fetched;
	std::map<std::string, fs::path> files;
	bool revised = false;
	std::error_code revision_error;
	const bool pending_revision = product.dir == worker.options.runtime_dir &&
			fs::is_regular_file(named_file(product.dir, "update.new"), revision_error);
	// What the precheck finds to fetch, each name once, and what the dialog counts of it as retail's
	// GET per row counts: the rows and bytes of every row to fetch (a name two rows share fetched
	// once, counted twice); an archive's REMOVE rows, every one, and its TARGET rows, the archive
	// jobs retail queues. [orig: Cupdate2Dlg_PrecheckCommands @ 0x407820 (UPDATE.EXE): a GET's
	// size and step @0x407a4a..0x407a67, an archive's REMOVE queued with no check @0x407e88..0x407eb0;
	// Cupdate2Dlg_InstallPffRows @ 0x406380 (UPDATE.EXE): an archive TARGET's AddDir job @0x406530]
	struct Fetch {
		std::string name;
		int32_t id = 0;
		uint64_t size = 0;
		fs::path path;
		uint32_t rows = 0; // the rows naming it
		uint64_t bytes = 0; // their sizes
	};
	std::vector<Fetch> fetches;
	std::map<std::string, size_t> fetch_of; // a fetched name's place in `fetches`
	uint64_t fetch_bytes = 0;
	uint32_t fetch_rows = 0;
	uint32_t archive_removes = 0;
	uint32_t archive_targets = 0;
	std::vector<size_t> origin; // each selected row's index among the command file's rows
	// The precheck's line and bar: each row's place among them as it is reached.
	// [orig: Cupdate2Dlg_PrecheckCommands @ 0x407820 (UPDATE.EXE): the range @0x407890, each row's
	// "%d/%d Commands" @0x40791b and position @0x407948]
	worker.shown.lines = Progress::Lines::Commands;
	worker.shown.total = command->rows.size();
	worker.shown.range = static_cast<uint32_t>(command->rows.size());
	Precheck precheck{product};
	for (size_t i = 0; i < command->rows.size(); ++i) {
		worker.shown.done = i;
		worker.shown.position = static_cast<uint32_t>(i);
		worker.show();
		update::CommandRow row = command->rows[i];
		if (row.type == update::CommandType::Target) {
			if (strutil::ends_with_icase(row.name, ".pff")) ++archive_targets;
			target = row.name; selected.rows.push_back(row); origin.push_back(i); continue;
		}
		if (row.type == update::CommandType::Remove && strutil::ends_with_icase(target, ".pff")) ++archive_removes;
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
			// A name two rows share is fetched once: the update directory is flat.
			const auto [at, fresh] = fetched.emplace(strutil::to_lower(row.name), download.path / io::os_path(row.name));
			if (fresh) {
				fetch_of[at->first] = fetches.size();
				fetches.push_back({row.name, int32_t(i), uint64_t(row.size), at->second, 0, 0});
			}
			Fetch &counted = fetches[fetch_of[at->first]];
			++counted.rows;
			counted.bytes += uint64_t(row.size);
			++fetch_rows;
			fetch_bytes += uint64_t(row.size);
			if (updater_image) {
				row.name = "update.new";
				revised = true;
			}
			files[strutil::to_lower(row.name)] = at->second;
		}
		selected.rows.push_back(row);
		origin.push_back(i);
	}
	++result.products;
	if (!worker.options.check) {
		// FETCH_WAIT: "Fetching files...", the bar's range the fetches and an archive's REMOVE rows;
		// each file as its reply completes moves the bar, adds its row's bytes, and is the current
		// file. [orig: Cupdate2Dlg_PrecheckCommands @ 0x407820 (UPDATE.EXE), the range @0x407aa7;
		// Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE), US_FETCH_WAIT @0x408940;
		// Cupdate2Dlg_OnGetReply @ 0x406ec0 (UPDATE.EXE): the bar @0x40701f, the bytes @0x40703b,
		// the file @0x407053]
		worker.shown.lines = Progress::Lines::Download;
		worker.shown.done = 0;
		worker.shown.total = fetch_bytes;
		worker.shown.position = 0;
		worker.shown.range = fetch_rows + archive_removes;
		worker.status("Fetching files...");
		for (const Fetch &fetch : fetches) {
			if (worker.interrupted()) return false;
			if (!download.make(worker.error)) return false;
			const uint64_t bound = fetch.size + fetch.size / 100 + 65536;
			if (!worker.fetch(socket.get(), fetch.name, fetch.id, bound, fetch.path)) return false;
			worker.shown.position += fetch.rows;
			worker.shown.done += fetch.bytes;
			worker.shown.file = fetch.name;
			worker.show();
		}
	}
	// The fetches are done: the server's connection ends ahead of the install, as retail's PACK
	// ends it.
	close_protocol(socket);
	if (worker.options.check) return true;
	if (worker.interrupted()) return false;
	// The fetch's end clears the three lines (FETCH_WAIT moving to PACK), and PACK's line follows.
	// [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE): FETCH_WAIT's PACK @0x408965, the
	// clearing @0x408cb8..0x408cd8, US_PACK @0x4089d7]
	worker.shown.lines = Progress::Lines::Blank;
	worker.show();
	worker.status("Decompressing update...");
	precheck.close(); // Windows must release the source archive before replacement.
	const std::string selected_text = update::write_command_file(selected);
	if (selected_text.empty()) { worker.error = "Cannot prepare update commands."; return false; }
	const std::vector<uint8_t> selected_bytes(selected_text.begin(), selected_text.end());
	// The install reads each row's file back from the download directory, one at a time.
	const update::SectionBlobs read_fetched = [&files](std::string_view name, std::vector<uint8_t> &packed,
			std::string &why) {
		const auto found = files.find(strutil::to_lower(name));
		if (found == files.end()) {
			why = "the update has no file for the row " + std::string(name);
			return false;
		}
		return io::read_file_bytes(io::utf8_path(found->second), packed, why);
	};
	// The install's "%d/%d Files" and bar: each row's place among the command file's rows as the
	// install reaches it, the current file each COPY row's; at its end the bar's range the archive
	// jobs, at 0, and no current file; then INTEGRATE's line and its jobs, ours done with the rows.
	// [orig: Cupdate2Dlg_InstallPffRows @ 0x406380 (UPDATE.EXE): the range @0x406401, each row's
	// line @0x40648e and position @0x4064b5, a COPY row's file @0x406619, the end @0x406819..0x406838;
	// Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE), US_INTEGRATE @0x408a48, its line @0x408d34]
	worker.shown.lines = Progress::Lines::Files;
	worker.shown.total = command->rows.size();
	worker.shown.range = static_cast<uint32_t>(command->rows.size());
	const update::ApplyRowStart row_start = [&worker, &origin](size_t index, const update::CommandRow &row) {
		worker.shown.done = origin[index];
		worker.shown.position = static_cast<uint32_t>(origin[index]);
		if (row.type == update::CommandType::Copy) worker.shown.file = row.name;
		worker.show();
	};
	update::ApplyReport report;
	if (!update::apply_commands(selected_bytes, product.command, read_fetched, product.dir, report, worker.error,
				row_start))
		return false;
	const uint32_t jobs = archive_targets + archive_removes;
	worker.shown.file.clear();
	worker.shown.done = worker.shown.total = jobs;
	worker.shown.position = worker.shown.range = jobs;
	worker.status("Integrating update...");
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
// of its own; an app's folder, that app alone. [orig: Cupdate2Dlg_RunExpansionUpdates @ 0x407f10
// (UPDATE.EXE); Cupdate2Dlg_RunWizardsIn @ 0x406290 (UPDATE.EXE)]
bool discover_products(const Options &o, std::vector<Product> &out, std::string &error) {
	out.clear();
	std::error_code ec;
	// The folder's OPENNOVA.WIZ names the app it holds: the runtime (ONRUNTIME, also with no
	// wizard), which updates the game beside it, or another app (the editor's ONEDITOR), whose
	// folder holds no game and no expansion, so it updates that app alone.
	Product runtime{o.runtime_dir, std::string(update::kRuntimeLogin), std::string(update::kRuntimeCommandFile),
			{}, true};
	const fs::path wizard = named_file(o.runtime_dir, "OPENNOVA.WIZ");
	if (fs::exists(wizard, ec) && !wizard_product(o.runtime_dir, wizard, runtime, error)) return false;
	if (!strutil::iequals(runtime.login, update::kRuntimeLogin)) {
		if (o.game_explicit || o.no_runtime) {
			error = "This folder holds " + runtime.login + " alone (its OPENNOVA.WIZ): /game and --no-runtime do not apply.";
			return false;
		}
		runtime.app = true;
		runtime.gateways = service_gateways(o.runtime_dir);
		out.push_back(std::move(runtime));
		return true;
	}
	if (!o.no_runtime) out.push_back(std::move(runtime));
	// The folder's own UPDATE.WIZ names what it holds (boot_policy.h folder_product, ADR 0067
	// d2): a stock game or a player's (ADR 0074), which its own wizard's product updates. A
	// launch without /game takes that product, and a product the folder does not hold is
	// refused, so OpenNova content never lands over another game's data. A legacy launch (the
	// retail game's own call) keeps the stock wizard.
	bool native_content = false;
	if (!o.legacy) {
		const FolderProduct folder = folder_product(io::utf8_path(o.resource_dir));
		if (folder.foreign) {
			error = "This folder's UPDATE.WIZ names a game OpenNova does not update.";
			return false;
		}
		const std::string product = o.game_explicit ? o.game
				: (folder.product.empty() ? std::string("acidgambit") : folder.product);
		if (product == "acidgambit") {
			if (!folder.product.empty() && folder.product != "acidgambit") {
				error = "This folder holds another game (its UPDATE.WIZ). OpenNova content installs only into its own folder.";
				return false;
			}
			// No wizard: a fresh folder, never one already holding game archives (a DFX
			// install ships none). OpenNova's own content writes its wizard first (its
			// loose rows precede the archives), so an interrupted install resumes.
			const auto slots = vfs_boot_archive_slots(io::utf8_path(o.resource_dir));
			if (folder.product.empty() && std::any_of(slots.begin(), slots.end(), [](const std::string &n) { return !n.empty(); })) {
				error = "This folder holds game data without OpenNova's UPDATE.WIZ. OpenNova content installs only into its own folder.";
				return false;
			}
			native_content = true;
		} else if (folder.product != product) {
			error = "This folder's UPDATE.WIZ does not name /game " + product + ".";
			return false;
		}
	}
	if (native_content) {
		out.push_back({o.resource_dir, std::string(update::kAcidGambitLogin),
				std::string(update::kAcidGambitCommandFile), {}, true});
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
		// Every expansion\<n>\ holding an UPDATE.WIZ is a product, its archive installed or not:
		// a release is a whole install, so the wizard alone installs the expansion (the jointops
		// zip ships Escalation's). [orig: Cupdate2Dlg_RunWizardsIn @ 0x406290 (UPDATE.EXE)]
		const fs::path wizard = named_file(dir, "UPDATE.WIZ");
		if (!fs::is_regular_file(wizard, ec)) continue;
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

VersionMark version_mark(const fs::path &dir) {
	// The wizard's CMD; a folder with none is an app's when its OPENNOVA.WIZ names another login
	// than the runtime's (its own command file), else OpenNova's own game's (the client's rule,
	// discover_products).
	std::string wizard, ignored;
	std::string command(update::kAcidGambitCommandFile);
	if (vfs_update_wizard_text(io::utf8_path(dir), wizard)) {
		command = update::read_update_wizard(wizard).cmd;
	} else if (io::read_file_text(io::utf8_path(named_file(dir, "OPENNOVA.WIZ")), wizard, ignored)) {
		const update::UpdateWizard app = update::read_update_wizard(wizard);
		if (!app.product_login.empty() && !strutil::iequals(app.product_login, update::kRuntimeLogin))
			command = app.cmd;
	}
	std::vector<uint8_t> bytes;
	if (command.empty() || !update::is_safe_row_name(command) ||
			!io::read_file_bytes(io::utf8_path(dir / command), bytes, ignored))
		return {};
	VersionMark mark{true, update::command_file_crc(bytes.data(), bytes.size()), std::string()};
	if (const std::optional<update::CommandFile> file =
					update::read_command_file(std::string(bytes.begin(), bytes.end())))
		mark.version = file->version;
	return mark;
}

DialogLines dialog_lines(const Progress &progress, uint64_t elapsed_ms) {
	DialogLines lines;
	char text[128];
	const auto done = static_cast<unsigned long long>(progress.done);
	const auto total = static_cast<unsigned long long>(progress.total);
	switch (progress.lines) {
	case Progress::Lines::Blank:
		break;
	case Progress::Lines::Commands:
		std::snprintf(text, sizeof text, "%llu/%llu Commands", done, total);
		lines.downloaded = text;
		break;
	case Progress::Lines::Download: {
		std::snprintf(text, sizeof text, "Downloading:  %llu/%llu KBytes", done >> 10, total >> 10);
		lines.downloaded = text;
		// The rate in bytes a second, as the CRT's (int) of it: __ftol2_sse's cvttsd2si, the integer
		// indefinite for what no int holds (a NaN or an infinity, the first millisecond's).
		// [orig: __ftol2_sse @ 0x41d3b0 (UPDATE.EXE), from Cupdate2Dlg_StateTick @ 0x408170 @0x408c01]
		const double bps = double(progress.done) / (double(elapsed_ms) / 1000.0);
		const int rate = bps > -2147483649.0 && bps < 2147483648.0 ? static_cast<int>(bps) : INT32_MIN;
		if (rate <= 1024) std::snprintf(text, sizeof text, "Throughput:  %d BPS", rate);
		else std::snprintf(text, sizeof text, "Throughput:  %d KBPS", rate >> 10);
		lines.throughput = text;
		if (rate <= 0) std::snprintf(text, sizeof text, "Estimated time: -");
		else std::snprintf(text, sizeof text, "Estimated time: %2.1f seconds", double(progress.total) / double(rate));
		lines.estimated = text;
		break;
	}
	case Progress::Lines::Files:
		std::snprintf(text, sizeof text, "%llu/%llu Files", done, total);
		lines.downloaded = text;
		break;
	}
	std::snprintf(text, sizeof text, "Elapsed time: %2.1f seconds", double(elapsed_ms) / 1000.0);
	lines.elapsed = text;
	return lines;
}

bool run(const Options &options, const std::vector<Product> &products, Result &result,
		std::string &error, const ProgressSink &progress, const std::atomic_bool *cancel) {
	result = {};
	error.clear();
	if (net::startup() != 0) { error = "Unable to initialize networking."; return false; }
	Worker worker{options, progress, cancel, Clock::now() + std::chrono::seconds(10), error};
	std::string failure;
	for (const Product &product : products) {
		// An expansion's run starts here, where retail's nested updater for it would start, after
		// the products before it: its command file as that run finds it, for its box.
		// [orig: Cupdate2Dlg_RunWizardsIn @ 0x406290 (UPDATE.EXE)]
		const bool nested = product.expansion && !options.check;
		const VersionMark start = nested ? version_mark(product.dir) : VersionMark{};
		bool updated = false;
		try {
			updated = update_product(worker, product, result);
		} catch (const std::bad_alloc &) {
			// Memory a file's install could not have is that product's failure, in retail's words for
			// one it has no text of its own for, never the process's end (a worker thread's escaping
			// exception would end it). [orig: Cupdate2Dlg_ReportResult @ 0x405140 (UPDATE.EXE), the
			// default text @0x405277]
			error = "An error has occurred during operation.  Please re-run update.";
		}
		if (!updated) {
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
			result.expansions.push_back({product.dir, start, false, error});
			error.clear();
			continue;
		}
		if (nested) result.expansions.push_back({product.dir, start, true, std::string()});
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
	return true;
}
} // namespace opennova::updater
