# opennova-novaworld-fleet

The NovaWorld service's fleet program: elected among the fleet processes on the database;
while it leads, the internal listener (the machine agents' routes, TCP 8070, on its task's
private address in a deployment, found from the ECS task metadata), the hosted servers'
supervisor with its admin links and its stats sampler, and the console drain. It calls no
cloud and runs and schedules no job: what it decides it writes as rows and jobs, which the
worker runs. `GET /livez` on `ONNET_HEALTH_PORT` (8081). Built with `BUILD_NOVAWORLD_HTTP`.
The leader's heartbeat carries the fleet's health over its rows (the shared library's
`hosted/fleet_health.*`: its danger and warn findings, the machines stuck, the least free
disk) and the age of its supervisor's last pass that failed in nothing, which the
deployment's `fleet-pass-age` alarm reads (`../novaworld_common/README.md`, "Health").

The service as a whole, its programs, their settings, the database, Redis, the health
checks and the tests: [`../novaworld_common/README.md`](../novaworld_common/README.md). The
image is the `fleet` target of [`../novaworld_common/Dockerfile`](../novaworld_common/Dockerfile).

- `election.*`: the election (`fleet::Election`; the leader lock `db::kFleetLock`, its
  generation in `fleet_leader`); a leadership lost ends the process with 71.
- `hosted/supervisor.*`, `hosted/stats_sampler.*`, `hosted/fleet_desired_state.h`: the
  hosted servers' supervisor, its per-minute stats sample and the answer each machine's
  sync gets.
- `hosted/admin_link.*`: the admin links' threads (their states' rows, which web reads, are
  the shared library's `hosted/admin_link_status.*`); `hosted/console_drain.*`: the console
  queue's drain over them (the routes' side is the shared library's `hosted/console_queue.*`).
- `hosted/internal_listener.*`, `hosted/agent_route.*`: the machines' internal listener and
  the agents' routes on it.
- These build as `opennova_novaworld_fleet_core`, which the tests drive in-process; `main.cpp`
  is the program (`opennova_novaworld_fleet`, binary `opennova-novaworld-fleet`).
