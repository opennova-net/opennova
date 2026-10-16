// opennova-novaworld-lobby: one lobby instance (program.h), as many as the
// deployment runs. The NovaWorld UDP listener (ONNET_NW_UDP_PORT, 64206)
// registers its instance at the endpoint it announces as it starts
// (ONNET_LOBBY_ENDPOINT, or its ECS task's public address: ecs_metadata.h),
// clearing its own previous run's rows and reaping the instances whose process
// is gone; holds its sessions in memory alone, writes their rows
// (lobby_sessions, active_hosts) and takes the commands web and the controller
// address to them (lobby_commands). The gates hand it clients by their
// address while it takes them. A redeploy of any other program leaves it
// serving; its own stop drains its instance first, and while another instance
// takes clients it serves on until every gate has read the drain.

#include "ecs_metadata.h"
#include "loggable.h"
#include "nw_udp_listener.h"
#include "program.h"

#include <net/novaworld/connection/manager.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_repository.h>
#include <net/novaworld/unknown_tracker.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

namespace {

using namespace opennova;
using namespace opennova::novaworld_server;

// How much longer than a gate's refresh (lobbydb::kGateRefreshSeconds) a
// draining lobby serves on at its stop, while another instance takes clients:
// every gate has read the drain by then. Service policy, well inside the
// deployment's stop grace (the ECS task's stop timeout).
constexpr uint64_t kDrainMarginMs = 2000;

// Whether a lobby instance other than `own` takes new clients (active, seen
// within kInstanceLiveSeconds): false with none, or when the database cannot
// say (no gate reads it then either).
bool other_lobby_takes_clients(db::ConnectionPool &pool, const std::string &own) {
	try {
		auto lease = pool.acquire();
		for (const std::string &endpoint : lobbydb::active_endpoints(*lease))
			if (endpoint != own)
				return true;
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[shutdown] WARN reading the lobby instances: %s\n", e.what());
	}
	return false;
}

std::string configure(ServerConfig &config) {
	if (config.web_host.empty())
		return "the lobby needs ONNET_WEB_HOST, the web endpoint its SessionInit names";
	// The endpoint it announces: the configured one, or its ECS task's public
	// address. The AWS CLI's two output files go to the system's temporary
	// directory (the container's own); nothing secret is in them.
	std::error_code ec;
	const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
	proc::ProcessCommandRunner runner(
			proc::ProcessCommandRunner::service_environment(), ec ? "/tmp" : temp.u8string());
	ecs::Sources sources;
	sources.configured = config.lobby_endpoint;
	sources.metadata_uri = ecs::metadata_uri_from_env();
	sources.port = config.nw_udp_port;
	sources.aws_cli = config.aws_cli;
	std::string error;
	config.lobby_endpoint = ecs::lobby_endpoint(sources, runner, error);
	return error;
}

int run(const program::Boot &boot) {
	const ServerConfig &config = boot.config;
	std::printf("[boot] lobby: announced at %s, its SessionInit naming web endpoint %s:%u\n",
			loggable(config.lobby_endpoint).c_str(), loggable(config.web_host).c_str(),
			static_cast<unsigned>(config.http_port));

	ConnectionManager manager(config.heartbeat_timeout_ms);
	manager.on_added([](const Connection &c) {
		std::printf("[conn] added id=0x%08x addr=%s pn=%s\n", c.id,
				peer_addr_to_string(c.addr).c_str(), loggable(c.pn).c_str());
	});
	// The listener thread records sightings of anything we don't handle; the
	// main loop flushes them into unknown_messages (never per packet).
	UnknownTracker unknown_tracker;

	NwUdpListener nwudp(manager);
	nwudp.set_db_pool(&boot.pool);
	// The endpoint advertised to joiners uses the client's NOVAWORLD session
	// port (client_reflect_novaworld_port, 32768), NOT the gate port
	// (client_reflect_gate_port, 49152 — that's only the gate's
	// ReflectedPortNumber). See server_config.h.
	nwudp.set_reflect_endpoint(config.client_reflect_ip, config.client_reflect_novaworld_port);
	nwudp.set_unknown_tracker(&unknown_tracker);

	// Wired before the listener starts, so the lost-handler cleans up the
	// per-connection lobby_state entry too (and its session row). It runs on
	// whichever thread drops the connection: the NW UDP receive thread, or the
	// main loop's for a timeout (manager.tick) and the shutdown; the lobby's
	// memory stays the lobby's either way. Without this, a hosting retail
	// process that exits without GOODBYE (heartbeat timeout) would stay in
	// /api/hosts forever (G.6). Keyed by the dropped Connection's PeerAddr:
	// two retail processes both send ci=0x00000001 (G.7).
	manager.on_lost([&nwudp](const Connection &c, DropReason r) {
		std::printf("[conn] lost  id=0x%08x identity='%s' reason=%s\n", c.id,
				loggable(c.identity).c_str(), drop_reason_name(r));
		nwudp.erase_lobby_state(c.addr, drop_reason_name(r));
	});

	// Its start registers its instance and clears its previous run's rows
	// before it serves: the UDP HELLO from any surviving retail process will
	// fail anyway, so those rows are dead state.
	if (!nwudp.start(config))
		return program::fatal("NW UDP listener start failed");

	// The lobby's watchdog: its receive loop.
	Watchdog watchdog;
	watchdog.watch("NovaWorld UDP receive loop", [&nwudp] { return nwudp.loop_turns(); });

	program::LoopHooks hooks;
	hooks.tick = [&manager, &unknown_tracker](db::Database &db, uint64_t now) {
		if (auto dropped = manager.tick(now); dropped > 0)
			std::printf("[tick] expired %zu connection(s)\n", dropped);
		// Flush any unknown-message sightings the listener thread recorded
		// since the last tick into unknown_messages (an additive upsert).
		try {
			unknown_tracker.flush(db);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[tick] WARN unknown_tracker flush: %s\n", e.what());
		}
	};
	hooks.heartbeat = [&manager](db::Database &db, Heartbeat &beat, int64_t) {
		beat.nw_sessions = static_cast<int64_t>(manager.registry().size());
		beat.listed_hosts = hostdb::aggregate(db).lobbies;
	};
	const int exit_code = program::run_main_loop("lobby", boot, watchdog, hooks);

	// The main loop's lease is back (what stops below leases on this thread:
	// the NW UDP listener's stop() and the manager's shutdown clean up their
	// lobby rows). The instance drains first, so no gate hands it a new client
	// from its next read of the instances. While another instance takes
	// clients, the lobby serves on for a gate's refresh and a margin, so every
	// gate has read the drain before it stops: a client a gate hands here
	// meanwhile still finds it. Alone, it stops at once (no gate has another
	// lobby to hand out). Then the NW UDP receive thread (it returns its lease
	// as it exits; the lobby's stop deletes its instance), then the
	// connections.
	nwudp.drain();
	if (other_lobby_takes_clients(boot.pool, nwudp.instance_endpoint())) {
		const uint64_t grace_ms =
				static_cast<uint64_t>(lobbydb::kGateRefreshSeconds) * 1000 + kDrainMarginMs;
		std::printf("[shutdown] the lobby serves on %llu ms while the gates read its drain\n",
				static_cast<unsigned long long>(grace_ms));
		std::fflush(stdout);
		std::this_thread::sleep_for(std::chrono::milliseconds(grace_ms));
	}
	std::printf("[shutdown] stopping listeners\n");
	nwudp.stop();
	manager.shutdown();
	return exit_code;
}

} // namespace

int main() {
	return program::run_program({ "lobby", configure, run });
}
