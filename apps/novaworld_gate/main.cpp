// opennova-novaworld-gate: the gate (program/program.h). Retail's first contact on
// UDP 7597 (ONNET_GATE_UDP_PORT): each probe answered with the lobby address
// every lobby instance announces, while one takes clients (gate_listener.h);
// the same port takes the hosts' status blobs. Stateless beyond its cached
// read of the lobby instances: as many gates as the deployment runs behind one
// address, and a redeploy of any other program leaves it serving.
// ONNET_LOBBY_ENDPOINT, optional, is the lobby it hands out while no instance
// takes clients; with none, such a probe goes unanswered (logged, and counted
// in the heartbeat's probes_unanswered, which the deployment alarms on), and
// the client tries again.

#include "core/loggable.h"
#include "gate_listener.h"
#include "program/program.h"

#include <net/novaworld/unknown_tracker.h>

#include <games/games.h>

#include <cstdio>
#include <string>

namespace {

using namespace opennova;
using namespace opennova::novaworld_server;

std::string configure(ServerConfig &config) {
	if (!config.lobby_endpoint.empty() && !program::is_endpoint(config.lobby_endpoint))
		return "ONNET_LOBBY_ENDPOINT is no <host>:<port>";
	return {};
}

int run(const program::Boot &boot) {
	const ServerConfig &config = boot.config;
	std::printf("[boot] gate: %s%s\n",
			config.lobby_endpoint.empty() ? "no fallback lobby" : "fallback lobby ",
			loggable(config.lobby_endpoint).c_str());

	// The receive thread records sightings of anything it does not handle; the
	// main loop flushes them into unknown_messages (never per packet).
	UnknownTracker unknown_tracker;
	GateListener gate;
	gate.set_unknown_tracker(&unknown_tracker);
	gate.set_db_pool(&boot.pool);
	if (!gate.start(config))
		return program::fatal("gate listener start failed");

	// The gate's watchdog: its receive loop.
	Watchdog watchdog;
	watchdog.watch("gate receive loop", [&gate] { return gate.loop_turns(); });

	program::LoopHooks hooks;
	hooks.tick = [&unknown_tracker](db::Database &db, uint64_t) {
		try {
			unknown_tracker.flush(db);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[tick] WARN unknown_tracker flush: %s\n", e.what());
		}
	};
	// Its heartbeat's fields: the probes no lobby could answer since the last
	// line, the lobby instances that take clients.
	hooks.heartbeat = [&gate](db::Database &db, Heartbeat &beat, int64_t) {
		gate.heartbeat(db, beat);
	};
	const int exit_code = program::run_main_loop("gate", boot, watchdog, hooks);

	std::printf("[shutdown] stopping listeners\n");
	gate.stop();
	return exit_code;
}

} // namespace

int main() {
	opennova::games::install(); // every game's titles, protocols and rules (games/games.h)
	return program::run_program({ "gate", configure, run });
}
