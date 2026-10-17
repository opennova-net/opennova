#pragma once

// The NovaWorld service is five programs (README.md "Programs"), each its own
// binary and image (apps/novaworld_<program>), over this shared library and the
// databases, constructing only its own parts:
//
//   gate        the gate (UDP 7597): each client's lobby, picked from the lobby
//               instances that take clients, and the hosts' status blobs
//               (POSTIPPORT)                          apps/novaworld_gate
//   lobby       one lobby instance (the NovaWorld UDP listener, 64206, with its
//               ConnectionManager tick and its lobby command drain), registered
//               at the endpoint it announces as it starts; as many as the
//               deployment runs                       apps/novaworld_lobby
//   web         the HTTP listener (8080: the retail NW*.dll routes, the
//               website's API, GET /livez and /healthz), billing's routes, the
//               Redis pool, the mail it queues        apps/novaworld_web
//   controller  elected: while it leads, the internal listener (the machines'
//               agent routes), the hosted servers' supervisor with its admin
//               links, the console drain and the periodic jobs' scheduler
//                                                     apps/novaworld_controller
//   worker      the job pool and its handlers: the prunes, the account jobs, the
//               mail it sends, the billing sync, the fleet's machine driver
//                                                     apps/novaworld_worker
//
// Each program's main() hands run_program its name, its configure step and its
// run. run_program is the boot every program shares: the log sink, the config,
// the boot line, the program's configure (its settings checked before the
// database is touched), the database pool, the migrations and the seeds under
// the migration lock (any program may boot first), then its run, until the
// shutdown signal.
//
// The programs share state only through PostgreSQL (and web's Redis), never
// through memory: a program reads what another wrote from rows (the lobby's
// sessions and commands, the lobby instances, the jobs, the machines, the
// admin links' states), and every address one announces comes from the config
// (ONNET_PUBLIC_HOST, ONNET_WEB_HOST with ONNET_HTTP_PORT,
// ONNET_LOBBY_ENDPOINT, ONNET_INTERNAL_URL), never from a port another bound.
// Each process logs {"boot":{"program":"<program>"}} once as it starts, its own
// heartbeat ({"heartbeat":{"program":"<program>",...}}, health.h) with the
// fields it knows, and runs its own stall watchdog over its own loops: a stall
// exits that process alone (70). Every program but web answers GET /livez on
// ONNET_HEALTH_PORT from that watchdog (health_listener.h); web answers it on
// its own HTTP port. Service policy, not a port.

#include "health.h"
#include "server_config.h"

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <functional>
#include <string>

namespace opennova::novaworld_server::program {

// What every program's run starts from: the config, the database's options
// (the controller's election opens a connection of its own) and the pool,
// migrated and seeded.
struct Boot {
	const ServerConfig &config;
	const db::ConnectOptions &db_options;
	db::ConnectionPool &pool;
};

struct Program {
	// Its name: the boot line's, the heartbeat's and its binary's
	// (opennova-novaworld-<name>). One of the five: no escaping needed.
	const char *name = "";
	// Its own settings checked, and completed where its environment leaves one
	// to be found (the controller's bind, from the ECS task metadata), before
	// the database is touched: "" to go on, else what the FATAL line says.
	// None: nothing to check.
	std::function<std::string(ServerConfig &config)> configure;
	// From its parts' start to its shutdown: the exit code.
	std::function<int(const Boot &boot)> run;
};

// A program's whole process, main()'s body: the log sink, the config from the
// environment, the boot line, the configure step (its FATAL: exit 1, nothing
// touched), the database pool, the migrations and the seeds under the
// migration lock, then the run. Logs the stop line on a clean exit. The exit
// code.
int run_program(const Program &program);

// What a program's main loop does besides the watchdog. `tick` runs every
// tick_interval_ms on the loop's connection (the lobby's ConnectionManager
// tick; the unknown-message flushes); `heartbeat` fills the program's fields of
// the line every kHeartbeatMs, `window_s` the seconds since the last one.
// `health_listener`: whether the loop serves GET /livez on ONNET_HEALTH_PORT
// (health_listener.h); web serves its own on its HTTP port.
struct LoopHooks {
	std::function<void(db::Database &db, uint64_t now_ms)> tick;
	std::function<void(db::Database &db, Heartbeat &beat, int64_t window_s)> heartbeat;
	bool health_listener = true;
};

// The program's main loop on the calling (main) thread, once its parts run:
// it starts the health listener (a port it cannot bind: FATAL, exit 1),
// installs the signal handlers, logs "[boot] ready.", then every
// tick_interval_ms asks the watchdog (a stalled loop logs the FATAL line and
// exits 70 at once), runs `tick`, and every kHeartbeatMs writes the heartbeat
// line (the program, its hooks' fields, the database's lock failures since the
// last line), on one pooled connection it holds for the loop. Returns 0 once
// the shutdown flag is up (the health listener stopped, the connection back
// in the pool), 1 when it could not lease that connection or bind the health
// port.
int run_main_loop(
		const char *program, const Boot &boot, Watchdog &watchdog, const LoopHooks &hooks);

// The FATAL line for a part a program could not start (written to stderr as
// "[boot] FATAL: <what>"), returning 1, the boot's exit code.
int fatal(const std::string &what);

// ONNET_LOBBY_ENDPOINT's shape: <host>:<port>, a port 1..65535.
bool is_endpoint(const std::string &text);

} // namespace opennova::novaworld_server::program
