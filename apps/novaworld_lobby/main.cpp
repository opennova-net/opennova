// opennova-novaworld-lobby: one lobby instance (program/program.h), as many as the
// deployment runs. The NovaWorld UDP listener (ONNET_NW_UDP_PORT, 64206)
// registers its instance, a row of its own, at the endpoint every instance
// announces (ONNET_LOBBY_ENDPOINT: the deployment's one lobby address, on ECS
// the load balancer's, the gate's Elastic IP and the lobby port, which spreads
// the clients across the lobby tasks), reaping the instances whose process is
// gone; holds its sessions in memory alone, writes their rows (lobby_sessions,
// active_hosts) and takes the commands web and the fleet address to them
// (lobby_commands). A redeploy of any other program leaves it serving. Its own
// stop drains its instance and stops at once: on ECS the load balancer has
// sent it no new client since its deregistration and ended its flows before
// the stop's signal, and a host whose session it held reconnects to the same
// address, which reaches a live lobby.

#include "core/loggable.h"
#include "nw_udp_listener.h"
#include "program/program.h"

#include <net/novaworld/connection/manager.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/unknown_tracker.h>

#include <cstdio>
#include <string>

namespace {

using namespace opennova;
using namespace opennova::novaworld_server;

std::string configure(ServerConfig &config) {
	if (config.web_host.empty())
		return "the lobby needs ONNET_WEB_HOST, the web endpoint its SessionInit names";
	if (config.lobby_endpoint.empty())
		return "the lobby needs ONNET_LOBBY_ENDPOINT, <host>:<port>, the lobby address the gates "
			   "hand its clients (on ECS the load balancer's: the gate's address and the lobby "
			   "port)";
	if (!program::is_endpoint(config.lobby_endpoint))
		return "ONNET_LOBBY_ENDPOINT is no <host>:<port>";
	return {};
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
	// ReflectedPortNumber). See program/server_config.h.
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

	// Its start registers its instance and reaps the instances whose process
	// is gone, with their rows, before it serves.
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
	// lobby rows). The instance drains first, so no gate counts it as taking
	// clients from its next read of the instances; then the NW UDP receive
	// thread (it returns its lease as it exits; the lobby's stop deletes its
	// instance), then the connections. No wait: which lobby a client reaches
	// is the load balancer's, which stopped sending this task new clients at
	// its deregistration and ended its flows before the signal (ECS stops a
	// task behind a load balancer only once its target has drained).
	nwudp.drain();
	std::printf("[shutdown] stopping listeners\n");
	nwudp.stop();
	manager.shutdown();
	return exit_code;
}

} // namespace

int main() {
	return program::run_program({ "lobby", configure, run });
}
