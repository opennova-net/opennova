// Each of the NovaWorld service's five programs boots alone with its own parts
// (apps/novaworld_common/program.h). Each real binary runs as a child process
// (program_process.h) on an empty database of its own, which its shared boot
// migrates:
//
//   - gate: a gate missing ONNET_WEB_HOST, or ONNET_PUBLIC_HOST in its
//     environment (the default would announce loopback), or with an
//     ONNET_LOBBY_ENDPOINT that is no <host>:<port>, does not boot: a FATAL line
//     and exit 1 before the database is touched, after its boot line. Booted:
//     the gate alone (no NW UDP, no HTTP, no job pool, no election), GET /livez
//     on its health port; with no lobby instance it hands out its fallback
//     lobby, and with no fallback either it answers no probe (and says so);
//   - lobby: one missing ONNET_WEB_HOST or ONNET_LOBBY_ENDPOINT, or with an
//     ONNET_LOBBY_ENDPOINT that is no <host>:<port>, does not boot. Booted: the
//     NW UDP listener alone, its instance registered at the configured
//     endpoint, GET /livez; a gate started beside it hands that endpoint and
//     the web endpoint its config names (ONNET_WEB_HOST and ONNET_HTTP_PORT),
//     never a port bound beside it, and so does the lobby's 0x82 SessionInit
//     (its web domain);
//   - two lobbies sharing one lobby address (a deployment's load balancer's),
//     each its own instance: both register under ids of their own, the
//     second's start reaps an instance unseen past the stale window with its
//     listing, and a gate beside them hands out the shared address; the
//     first one's SIGTERM drains its instance and stops at once (its row
//     goes), while the second serves on and the gate still hands out the
//     address; with the second stopped too and no fallback, the gate answers
//     no probe;
//   - web: one with no ONNET_WEB_HOST does not boot; booted, the HTTP listener
//     alone (and no health listener of its own: GET /livez and /healthz answer
//     on its HTTP port), its menus' HOST_URL naming the web endpoint;
//   - controller: no listener but its health one, elected (it leads), its
//     parts stopped at the signal;
//   - worker: its job pool, its health listener.
//
// Every program then stops on SIGTERM with exit 0 and every database
// connection closed. POSIX only.

#include "program_process.h"

#include <net/title/title_identity.h>
#include <base/gameprofile/gameprofile.h>
#include <net/napi/envelope.h>
#include <net/novaworld/client_session.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/gate_response.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_repository.h>

#include "common/test_expect.h"
#include "common/test_redis.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace program_process;

namespace {

const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

constexpr const char *kWebHost = "web.example.test";

// Every program's environment here: the gate's public address, web's Redis,
// and `more`.
std::vector<std::string> env_of(
		const test_temp::TempDir &temp, const std::string &db_url, std::vector<std::string> more) {
	std::vector<std::string> env = {
		"ONNET_PUBLIC_HOST=127.0.0.1",
		"ONNET_REDIS_URL=" + test_redis::server_url(),
	};
	env.insert(env.end(), more.begin(), more.end());
	return program_env(temp, db_url, env);
}

bool has(const std::string &log, const std::string &text) {
	return log.find(text) != std::string::npos;
}

std::string boot_line(const char *program) {
	return std::string("{\"boot\":{\"program\":\"") + program + "\"}}";
}

// The run's end: SIGTERM, exit 0 within the deadline, the log's stop line, and
// no connection left on the database.
int stop_cleanly(ProgramProcess &process, const char *program,
		const test_db::TestDatabase &database, const std::string &log_path) {
	TEST_EXPECT(::kill(process.pid, SIGTERM) == 0);
	TEST_EXPECT(process.wait_exit(kExitDeadline));
	TEST_EXPECT(process.exit_code() == 0);
	TEST_EXPECT(has(read_text(log_path),
			std::string("==== opennova-novaworld-") + program + " stopped ===="));
	TEST_EXPECT(connections_left(database) == 0);
	return 0;
}

// A process that must not boot: exit 1 within the boot deadline, its boot line
// and `fatal` in its log, and nothing of the database's.
int refused(ProgramProcess &process, const char *program, const std::string &binary,
		const std::vector<std::string> &env, const test_temp::TempDir &temp,
		const std::string &log_path, const char *fatal) {
	TEST_EXPECT(process.spawn(binary, env, temp.root(), log_path));
	TEST_EXPECT(process.wait_exit(kBootDeadline));
	TEST_EXPECT(process.exit_code() == 1);
	const std::string log = read_text(log_path);
	if (!has(log, fatal))
		std::fprintf(stderr, "  no '%s' in:\n%s\n", fatal, log.c_str());
	TEST_EXPECT(has(log, fatal));
	TEST_EXPECT(has(log, boot_line(program)));
	TEST_EXPECT(!has(log, "[boot] migrations:") && !has(log, "[boot] ready."));
	return 0;
}

// GET /livez on the health port the program logged: 200 "ok".
int livez_ok(ProgramProcess &process, const std::string &log_path) {
	const uint16_t port = logged_port(read_text(log_path), kHealthLine);
	TEST_EXPECT(port != 0);
	TEST_EXPECT(wait_for_http(process, port, "/livez"));
	const net::HttpReply reply = net::http_exchange("GET",
			"http://127.0.0.1:" + std::to_string(port) + "/livez", {}, {}, 2000, loopback_only());
	TEST_EXPECT(reply.code == 200 && std::string(reply.body.begin(), reply.body.end()) == "ok");
	return 0;
}

// One gate probe to 127.0.0.1:`port`; its reply decoded into `out`, false when
// none came within `wait_ms`.
bool probe_gate(uint16_t port, opennova::GateResponse &out, int wait_ms = 3000) {
	net::ScopedSocket client(net::udp_bind(0));
	if (!client.is_valid())
		return false;
	const auto inner = opennova::gate_probe_build(kJo.gate_tag);
	std::vector<uint8_t> probe(inner.size() + 16);
	size_t probe_size = 0;
	if (opennova::napi_envelope_encode(
				inner.data(), inner.size(), probe.data(), probe.size(), &probe_size) != 0)
		return false;
	probe.resize(probe_size);
	const net::Endpoint to{ { 127, 0, 0, 1 }, port };
	if (net::udp_send_to(client.get(), to, probe.data(), probe.size()) <= 0)
		return false;
	uint8_t rx[2048];
	net::Endpoint from{};
	const int n = net::udp_recv_from(client.get(), rx, sizeof(rx), from, wait_ms);
	if (n <= 0)
		return false;
	std::vector<uint8_t> reply(static_cast<size_t>(n));
	size_t reply_len = 0;
	return opennova::napi_envelope_decode(
				   rx, static_cast<size_t>(n), reply.data(), reply.size(), &reply_len) == 0 &&
			opennova::gate_response_decrypt_and_parse(reply.data(), reply_len, out);
}

// A NovaWorld session to 127.0.0.1:`port` up to its 0x82 SessionInit; the web
// domain that carried goes to `web_domain`.
int session_web_domain(uint16_t port, std::string &web_domain) {
	net::ScopedSocket client(net::udp_bind(0));
	TEST_EXPECT(client.is_valid());
	const net::Endpoint to{ { 127, 0, 0, 1 }, port };
	opennova::ClientSession session{ opennova::ClientSession::Config::lobby(kJo, kJo.gate_tag) };
	const auto send = [&](const std::vector<uint8_t> &datagram) {
		if (!datagram.empty())
			net::udp_send_to(client.get(), to, datagram.data(), datagram.size());
	};
	send(session.start());
	for (int i = 0; i < 25 && session.server_web_domain().empty(); ++i) {
		uint8_t rx[4096];
		net::Endpoint from{};
		const int n = net::udp_recv_from(client.get(), rx, sizeof(rx), from, 200);
		if (n <= 0)
			continue;
		std::vector<std::vector<uint8_t>> out;
		TEST_EXPECT(session.handle_datagram(rx, static_cast<size_t>(n), out));
		session.finish_receive_batch(out);
		session.pump(out);
		for (const auto &datagram : out)
			send(datagram);
	}
	web_domain = session.server_web_domain();
	send(session.build_goodbye());
	return 0;
}

int exercise_gate(const Binaries &bin, const test_temp::TempDir &temp,
		const test_db::TestDatabase &database, const std::string &log_path) {
	const std::string web = std::string("ONNET_WEB_HOST=") + kWebHost;
	{
		ProgramProcess process;
		if (refused(process, "gate", bin.gate, env_of(temp, database.conninfo(), {}), temp,
					log_path, "[boot] FATAL: the gate needs ONNET_WEB_HOST") != 0)
			return 1;
	}
	{
		ProgramProcess process;
		std::vector<std::string> no_public_host;
		for (const std::string &e : env_of(temp, database.conninfo(), { web }))
			if (e.rfind("ONNET_PUBLIC_HOST=", 0) != 0)
				no_public_host.push_back(e);
		if (refused(process, "gate", bin.gate, no_public_host, temp, log_path,
					"[boot] FATAL: the gate needs ONNET_PUBLIC_HOST") != 0)
			return 1;
	}
	{
		ProgramProcess process;
		if (refused(process, "gate", bin.gate,
					env_of(temp, database.conninfo(), { web, "ONNET_LOBBY_ENDPOINT=nowhere" }),
					temp, log_path, "[boot] FATAL: ONNET_LOBBY_ENDPOINT is no <host>:<port>") != 0)
			return 1;
	}
	// With a fallback lobby and no lobby instance: the fallback.
	{
		ProgramProcess process;
		if (!boot(process, bin.gate,
					env_of(temp, database.conninfo(),
							{ web, "ONNET_GATE_UDP_PORT=0", "ONNET_HTTP_PORT=8123",
									"ONNET_LOBBY_ENDPOINT=127.0.0.9:64206" }),
					temp.root(), log_path))
			return 1;
		const std::string log = read_text(log_path);
		TEST_EXPECT(has(log, boot_line("gate")));
		const uint16_t gate_port = logged_port(log, kGateLine);
		TEST_EXPECT(gate_port != 0);
		// Its parts alone.
		TEST_EXPECT(!has(log, kNwUdpLine) && !has(log, kHttpLine) && !has(log, "[jobs]") &&
				!has(log, "[controller]"));
		if (livez_ok(process, log_path) != 0)
			return 1;
		opennova::GateResponse reply;
		TEST_EXPECT(probe_gate(gate_port, reply));
		TEST_EXPECT(reply.post_port == gate_port);
		TEST_EXPECT(reply.udp_novaworld == std::vector<std::string>{ "127.0.0.9:64206" });
		if (stop_cleanly(process, "gate", database, log_path) != 0)
			return 1;
		TEST_EXPECT(logged_in_order(
				read_text(log_path), { "[shutdown] stopping listeners", "[gate] loop exiting" }));
	}
	// No fallback, no lobby instance: no reply, and a line that says why.
	{
		ProgramProcess process;
		if (!boot(process, bin.gate,
					env_of(temp, database.conninfo(), { web, "ONNET_GATE_UDP_PORT=0" }),
					temp.root(), log_path))
			return 1;
		const uint16_t gate_port = logged_port(read_text(log_path), kGateLine);
		opennova::GateResponse reply;
		TEST_EXPECT(!probe_gate(gate_port, reply, 1500));
		TEST_EXPECT(wait_for_log(process, log_path,
				"[gate] WARN no lobby instance takes clients and no fallback", kBootDeadline));
		if (stop_cleanly(process, "gate", database, log_path) != 0)
			return 1;
	}
	std::printf("  gate: alone; the fallback lobby with no instance, no reply with none\n");
	return 0;
}

// The lobby, and a gate beside it handing out its endpoint.
int exercise_lobby(const Binaries &bin, const test_temp::TempDir &temp,
		const test_db::TestDatabase &database, const std::string &log_path) {
	// The lobby's port is fixed up front and its endpoint names it under another
	// address, as a deployment's announced address would: what the gate hands
	// out is the lobby's row, the configured endpoint, not what it bound.
	const uint16_t nw_port = free_udp_port();
	const std::string lobby = "127.0.0.2:" + std::to_string(nw_port);
	const std::vector<std::string> ports = { "ONNET_NW_UDP_PORT=" + std::to_string(nw_port),
		"ONNET_HTTP_PORT=8123", "ONNET_GATE_UDP_PORT=0" };
	const std::string web = std::string("ONNET_WEB_HOST=") + kWebHost;
	{
		ProgramProcess process;
		std::vector<std::string> env = ports;
		env.push_back("ONNET_LOBBY_ENDPOINT=" + lobby);
		if (refused(process, "lobby", bin.lobby, env_of(temp, database.conninfo(), env), temp,
					log_path, "[boot] FATAL: the lobby needs ONNET_WEB_HOST") != 0)
			return 1;
	}
	{
		ProgramProcess process;
		std::vector<std::string> env = ports;
		env.push_back(web);
		if (refused(process, "lobby", bin.lobby, env_of(temp, database.conninfo(), env), temp,
					log_path, "[boot] FATAL: the lobby needs ONNET_LOBBY_ENDPOINT") != 0)
			return 1;
	}
	{
		ProgramProcess process;
		std::vector<std::string> env = ports;
		env.push_back(web);
		env.push_back("ONNET_LOBBY_ENDPOINT=nowhere");
		if (refused(process, "lobby", bin.lobby, env_of(temp, database.conninfo(), env), temp,
					log_path, "[boot] FATAL: ONNET_LOBBY_ENDPOINT is no <host>:<port>") != 0)
			return 1;
	}
	ProgramProcess process;
	std::vector<std::string> env = ports;
	env.push_back("ONNET_LOBBY_ENDPOINT=" + lobby);
	env.push_back(web);
	if (!boot(process, bin.lobby, env_of(temp, database.conninfo(), env), temp.root(), log_path))
		return 1;
	const std::string log = read_text(log_path);
	TEST_EXPECT(has(log, boot_line("lobby")));
	TEST_EXPECT(logged_port(log, kNwUdpLine) == nw_port);
	// Its parts alone.
	TEST_EXPECT(!has(log, kGateLine) && !has(log, kHttpLine) && !has(log, "[jobs]") &&
			!has(log, "[controller]"));
	TEST_EXPECT(has(log, "[lobby] instance ") && has(log, " at " + lobby + " registered;"));
	if (livez_ok(process, log_path) != 0)
		return 1;
	std::string web_domain;
	if (session_web_domain(nw_port, web_domain) != 0)
		return 1;
	TEST_EXPECT(web_domain == std::string(kWebHost) + ":8123");

	// A gate beside it, no fallback: it hands out the lobby's instance.
	{
		const std::string gate_log = temp.file("gate.log");
		ProgramProcess gate;
		if (!boot(gate, bin.gate,
					env_of(temp, database.conninfo(),
							{ web, "ONNET_GATE_UDP_PORT=0", "ONNET_HTTP_PORT=8123" }),
					temp.root(), gate_log))
			return 1;
		const uint16_t gate_port = logged_port(read_text(gate_log), kGateLine);
		opennova::GateResponse reply;
		TEST_EXPECT(probe_gate(gate_port, reply));
		TEST_EXPECT(reply.udp_novaworld == std::vector<std::string>{ lobby });
		const std::string startup = std::string("http://") + kWebHost + ":8123/nwprepare.dll?";
		if (reply.startup_url.compare(0, startup.size(), startup) != 0)
			std::fprintf(stderr, "  STARTUPURL %s\n", reply.startup_url.c_str());
		TEST_EXPECT(reply.startup_url.compare(0, startup.size(), startup) == 0);
		TEST_EXPECT(::kill(gate.pid, SIGTERM) == 0 && gate.wait_exit(kExitDeadline) &&
				gate.exit_code() == 0);
	}
	if (stop_cleanly(process, "lobby", database, log_path) != 0)
		return 1;
	TEST_EXPECT(logged_in_order(
			read_text(log_path), { "[shutdown] stopping listeners", "[nwudp] loop exiting" }));
	std::printf("  lobby: alone at %s; a gate beside it hands it out and %s:8123\n", lobby.c_str(),
			kWebHost);
	return 0;
}

// Two lobbies on one database, each its own instance at the one lobby address
// both announce (a deployment's load balancer's; here an address no one serves,
// since what the gate hands out is the rows' endpoint): both register under ids
// of their own, the second's start reaps an instance unseen past the stale
// window with its listing, and a gate beside them hands out that address. The
// first one's SIGTERM drains its instance and stops at once: which task a
// client reaches is the load balancer's, which a task leaves by its
// deregistration before the signal, so nothing waits on the gates. Its row
// goes, the second's stays active, the second still answers a session, and the
// gate still hands out the address. With the second stopped too and no
// fallback, the gate answers no probe once it reads the instances again.
int exercise_two_lobbies(const Binaries &bin, const test_temp::TempDir &temp,
		const test_db::TestDatabase &database, const std::string &log_path) {
	const std::string shared = "198.51.100.20:64206";
	const uint16_t port_a = free_udp_port();
	uint16_t port_b = free_udp_port();
	while (port_b == port_a)
		port_b = free_udp_port();
	const std::string web = std::string("ONNET_WEB_HOST=") + kWebHost;
	const auto lobby_env = [&](uint16_t nw_port) {
		return env_of(temp, database.conninfo(),
				{ "ONNET_NW_UDP_PORT=" + std::to_string(nw_port), "ONNET_LOBBY_ENDPOINT=" + shared,
						"ONNET_HTTP_PORT=8123", web });
	};
	const std::string log_b = temp.file("lobby_b.log");
	ProgramProcess a;
	if (!boot(a, bin.lobby, lobby_env(port_a), temp.root(), log_path))
		return 1;
	// Each read on a connection of its own, closed before the run's end counts them.
	const auto instances_now = [&database] {
		opennova::db::Database db = database.open();
		return opennova::lobbydb::list_instances(db);
	};
	// An instance whose process went ten minutes ago (its row unseen since), with a listing.
	int64_t stale = 0;
	opennova::hostdb::HostRow stale_listing;
	{
		opennova::db::Database db = database.open();
		stale = opennova::lobbydb::register_instance(db, "198.51.100.30:64206");
		stale_listing.rid = opennova::lobbydb::next_rid(db);
		stale_listing.game = "jop_2_consumer";
		stale_listing.server_name = "Stale Host";
		stale_listing.lobby_id = stale;
		opennova::hostdb::upsert_host(db, stale_listing);
		db.exec("UPDATE lobby_instances SET last_seen_at = now() - interval '10 minutes' "
				"WHERE id = $1;",
				{ opennova::db::BindValue(stale) });
	}
	ProgramProcess b;
	if (!boot(b, bin.lobby, lobby_env(port_b), temp.root(), log_b))
		return 1;
	auto instances = instances_now();
	TEST_EXPECT(instances.size() == 2);
	TEST_EXPECT(instances[0].id != instances[1].id);
	for (const auto &i : instances)
		TEST_EXPECT(i.endpoint == shared && i.state == "active" && i.id != stale);
	{
		opennova::db::Database db = database.open();
		TEST_EXPECT(!opennova::hostdb::find_host_by_rid(db, stale_listing.rid));
	}
	TEST_EXPECT(has(read_text(log_b), "1 stale instance(s) reaped with their rows"));
	const int64_t id_b = instances[1].id; // A registered first

	const std::string gate_log = temp.file("gate.log");
	ProgramProcess gate;
	if (!boot(gate, bin.gate,
				env_of(temp, database.conninfo(),
						{ web, "ONNET_GATE_UDP_PORT=0", "ONNET_HTTP_PORT=8123" }),
				temp.root(), gate_log))
		return 1;
	const uint16_t gate_port = logged_port(read_text(gate_log), kGateLine);
	opennova::GateResponse reply;
	TEST_EXPECT(probe_gate(gate_port, reply));
	TEST_EXPECT(reply.udp_novaworld == std::vector<std::string>{ shared });

	// A stops: drained, then gone at once; B serves on.
	TEST_EXPECT(::kill(a.pid, SIGTERM) == 0);
	TEST_EXPECT(a.wait_exit(kExitDeadline) && a.exit_code() == 0);
	TEST_EXPECT(logged_in_order(read_text(log_path),
			{ " draining: no gate counts it as taking clients", "[shutdown] stopping listeners" }));
	instances = instances_now();
	TEST_EXPECT(instances.size() == 1 && instances[0].id == id_b && instances[0].state == "active");
	std::string web_domain;
	if (session_web_domain(port_b, web_domain) != 0)
		return 1;
	TEST_EXPECT(web_domain == std::string(kWebHost) + ":8123");
	TEST_EXPECT(probe_gate(gate_port, reply));
	TEST_EXPECT(reply.udp_novaworld == std::vector<std::string>{ shared });

	// B stops too: no instance left, no fallback, and the gate answers nothing from its next
	// read of the instances on.
	TEST_EXPECT(::kill(b.pid, SIGTERM) == 0);
	TEST_EXPECT(b.wait_exit(kExitDeadline) && b.exit_code() == 0);
	TEST_EXPECT(instances_now().empty());
	const auto deadline = Clock::now() + std::chrono::seconds(20);
	bool silent = false;
	while (!silent && Clock::now() < deadline)
		silent = !probe_gate(gate_port, reply, 500);
	TEST_EXPECT(silent);
	TEST_EXPECT(::kill(gate.pid, SIGTERM) == 0 && gate.wait_exit(kExitDeadline) &&
			gate.exit_code() == 0);
	TEST_EXPECT(connections_left(database) == 0);
	std::printf("  two lobbies at %s: both registered, the first stopped at once while the "
				"second served on; none left, the gate answered no one\n",
			shared.c_str());
	return 0;
}

int exercise_web(const Binaries &bin, const test_temp::TempDir &temp,
		const test_db::TestDatabase &database, const std::string &log_path) {
	const uint16_t http_port = free_tcp_port();
	const std::string port_var = "ONNET_HTTP_PORT=" + std::to_string(http_port);
	{
		ProgramProcess process;
		if (refused(process, "web", bin.web, env_of(temp, database.conninfo(), { port_var }), temp,
					log_path, "[boot] FATAL: web needs ONNET_WEB_HOST") != 0)
			return 1;
	}
	ProgramProcess process;
	if (!boot(process, bin.web,
				env_of(temp, database.conninfo(),
						{ port_var, std::string("ONNET_WEB_HOST=") + kWebHost }),
				temp.root(), log_path))
		return 1;
	const std::string log = read_text(log_path);
	TEST_EXPECT(has(log, boot_line("web")));
	TEST_EXPECT(logged_port(log, kHttpLine) == http_port);
	TEST_EXPECT(!has(log, kGateLine) && !has(log, kNwUdpLine) && !has(log, kHealthLine) &&
			!has(log, "[jobs]") && !has(log, "[controller]"));
	// The menus name the one web endpoint (ONNET_WEB_HOST) and the port, as the
	// gate does.
	TEST_EXPECT(has(log,
			"[http] HOST_URL=http://" + std::string(kWebHost) + ":" + std::to_string(http_port) +
					"/nwhost.dll"));
	TEST_EXPECT(wait_for_http(process, http_port, "/healthz"));
	TEST_EXPECT(wait_for_http(process, http_port, "/livez"));
	if (stop_cleanly(process, "web", database, log_path) != 0)
		return 1;
	std::printf("  web: the HTTP listener alone on :%u, its URLs naming %s\n",
			static_cast<unsigned>(http_port), kWebHost);
	return 0;
}

int exercise_controller(const Binaries &bin, const test_temp::TempDir &temp,
		const test_db::TestDatabase &database, const std::string &log_path) {
	ProgramProcess process;
	if (!boot(process, bin.controller, env_of(temp, database.conninfo(), {}), temp.root(),
				log_path))
		return 1;
	TEST_EXPECT(wait_for_log(process, log_path, "[controller] controller-", kBootDeadline));
	const auto leads_by = Clock::now() + std::chrono::seconds(10);
	while (!has(read_text(log_path), " leads (generation ") && Clock::now() < leads_by)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	const std::string log = read_text(log_path);
	TEST_EXPECT(has(log, boot_line("controller")));
	TEST_EXPECT(has(log, " leads (generation "));
	TEST_EXPECT(!has(log, kGateLine) && !has(log, kNwUdpLine) && !has(log, kHttpLine) &&
			!has(log, "[jobs] ") && !has(log, "internal listener on"));
	if (livez_ok(process, log_path) != 0)
		return 1;
	if (stop_cleanly(process, "controller", database, log_path) != 0)
		return 1;
	TEST_EXPECT(logged_in_order(
			read_text(log_path), { "[shutdown] stopping the controller", " stopped its parts" }));
	std::printf("  controller: elected, no listener but its health one, its parts stopped at "
				"the signal\n");
	return 0;
}

int exercise_worker(const Binaries &bin, const test_temp::TempDir &temp,
		const test_db::TestDatabase &database, const std::string &log_path) {
	ProgramProcess process;
	if (!boot(process, bin.worker, env_of(temp, database.conninfo(), { "ONNET_WORKER_THREADS=2" }),
				temp.root(), log_path))
		return 1;
	const std::string log = read_text(log_path);
	TEST_EXPECT(has(log, boot_line("worker")));
	TEST_EXPECT(has(log, "[jobs] 2 worker thread(s)"));
	TEST_EXPECT(!has(log, kGateLine) && !has(log, kNwUdpLine) && !has(log, kHttpLine) &&
			!has(log, "[controller]"));
	if (livez_ok(process, log_path) != 0)
		return 1;
	if (stop_cleanly(process, "worker", database, log_path) != 0)
		return 1;
	std::printf("  worker: its job pool alone\n");
	return 0;
}

using Exercise = int (*)(const Binaries &, const test_temp::TempDir &,
		const test_db::TestDatabase &, const std::string &);

int run_case(const Binaries &bin, Exercise exercise, const char *name) {
	test_temp::TempDir temp((std::string("nw_programs_") + name).c_str());
	const test_db::TestDatabase database({ /*migrate=*/false });
	const std::string log_path = temp.file("program.log");
	std::printf("%s:\n", name);
	const int rc = exercise(bin, temp, database, log_path);
	if (rc != 0)
		std::fprintf(stderr, "FAIL: %s\n---- last program log ----\n%s---- end ----\n", name,
				read_text(log_path).c_str());
	return rc;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	test_redis::init(argc, argv);
	Binaries bin;
	if (!bin.read(argc, argv, 1)) {
		std::fprintf(stderr, "usage: %s <gate> <lobby> <web> <controller> <worker>\n", argv[0]);
		return 2;
	}
	TEST_EXPECT(net::startup() == 0);
	int rc = run_case(bin, exercise_gate, "gate");
	if (rc == 0)
		rc = run_case(bin, exercise_lobby, "lobby");
	if (rc == 0)
		rc = run_case(bin, exercise_two_lobbies, "two_lobbies");
	if (rc == 0)
		rc = run_case(bin, exercise_web, "web");
	if (rc == 0)
		rc = run_case(bin, exercise_controller, "controller");
	if (rc == 0)
		rc = run_case(bin, exercise_worker, "worker");
	net::shutdown();
	if (rc == 0)
		std::printf("OK: each program boots with its own parts alone\n");
	return rc;
}
