# NovaWorld server

The NovaWorld service: five programs ("Programs" below: gate, lobby, web, controller,
worker), each its own binary and container image, over this shared library
(`apps/novaworld_common`), one PostgreSQL database ("Database" below) and, for web, one
Redis ("Redis" below). Linux only: it ships as its images and runs on ECS Fargate, one
service per program (DEPLOY.md). Implements the matchmaking/backend half of NovaWorld (the gate, the
session handshake, the browser/host/play container services, and the legacy
`NW*.dll` HTTP flow). Protocol record: [`docs/net/novaworld-net-re.md`](../../docs/net/novaworld-net-re.md).

## Listeners

| Listener | Port | Protocol |
| --- | --- | --- |
| `GateListener` | UDP 7597 | `novaworld_gate` probe → gate response (NWU `GATEAPI`) from the tag's lobby ("Lobbies: one per title" below); the same port is the advertised `POSTIPPORT`, the sink for a retail host's plaintext status heartbeat (`Lobby_UpdateServerInfo`), folded into `active_hosts` by `HostKey` |
| `NwUdpListener` | UDP 64206 | NWU framing -> session handshake (0x41/0x42/0x43/0x46) -> PN dispatch: NOVAWORLDUDP containers or JointOperations game runtime |
| `HttpListener` (Crow, `BUILD_NOVAWORLD_HTTP`) | TCP 8080 | `NW*.dll` routes + `/api/*` backend + web portal fallback |
| `InternalListener` (`hosted/internal_listener.*`) | TCP 8070 | the machines' agent routes alone (`hosted/agent_route.*`), on a private address |
| `HealthListener` (`health_listener.*`) | TCP 8081 | `GET /livez` alone, from the program's stall watchdog ("Health" below) |

The gate listener is the gate's (`apps/novaworld_gate`), the NW UDP listener the lobby's
(`apps/novaworld_lobby`), the HTTP listener web's (`apps/novaworld_web`), the internal
listener the controller's while it leads (`apps/novaworld_controller`), and a health
listener each program's but web's, whose `/livez` is on its HTTP port.

## Programs

Five programs, each its own binary (`opennova-novaworld-<program>`), its own image
(`ghcr.io/<owner>/opennova-novaworld-<program>`, a target of [`Dockerfile`](Dockerfile))
and its own ECS service, over this shared library and the databases. Nothing selects
what a process is: each binary is one program, constructs its own parts alone, and
scales on its own. `program.*` is the boot every program shares
(`program::run_program`): the log sink, the config, the boot line, the program's own
settings checked before the database is touched (a FATAL exit 1 when one is missing),
the database pool, the migrations and the seeds under the migration lock (whichever
program boots first migrates; the others find nothing to apply), then the program's run;
and the main loop every program ends in (`program::run_main_loop`: its tick, its
heartbeat, its stall watchdog, its health listener, the shutdown signal).

| Program | Its parts | Scales by |
| --- | --- | --- |
| gate (`apps/novaworld_gate`) | the gate listener: each client's lobby, the one address every lobby instance announces while one takes clients, and the hosts' status blobs ("Lobbies: one per title" below) | more gate tasks behind the gate's one address (stateless but for its cached read of the instances) |
| lobby (`apps/novaworld_lobby`) | one lobby instance: the NW UDP listener, its `ConnectionManager` tick, its lobby command drain, its session rows; its instance's registration, a row of its own, at the one lobby address ("Lobby instances" below) | more lobbies behind the one lobby address, each its own instance |
| web (`apps/novaworld_web`) | the HTTP listener (the retail `NW*.dll` and `.gsb` routes, the website's API, `GET /livez` and `GET /healthz`), billing's routes and webhook, the Redis pool (a `PING` at boot), the mail it queues; the first site admin (`ONNET_BOOTSTRAP_ADMIN`) | replicas behind one load balancer |
| controller (`apps/novaworld_controller`) | the election ("The controller" below); while it leads the internal listener (bound only then), the hosted servers' supervisor with its admin links, the console drain and the periodic jobs' scheduler | one leader (a standby waits) |
| worker (`apps/novaworld_worker`) | the job pool and its handlers ("Background jobs" below): the prunes, the account jobs, the mail it sends, the billing sync, the fleet's machine driver | more workers, more threads each |

Each program's own code lives in its directory, a static library its tests drive in
process (`opennova_novaworld_<program>_core`; the worker has its `main.cpp` alone) and
its executable; what more than one program uses lives here.

- **They share state only through PostgreSQL** (durable) **and web's Redis**
  (ephemeral), never through memory: the lobby's sessions and commands, the lobby
  instances, the listings, the jobs, the machines and servers, the admin links' states
  and the console's answers are rows each program reads; `GET /api/unknowns` reads
  `unknown_messages`, where every process flushes its sightings. What several programs
  decide from the same settings they decide alike (`parts.*`): whether hosting is on
  (web's routes, the controller's supervisor, the worker's driver), whether billing and
  mail are, the periodic jobs.
- **Every address a program announces comes from the config**, never from a port another
  bound: the gate's `POSTIPADDRESS` is `ONNET_PUBLIC_HOST` (where status blobs land), its
  `udpnovaworld` the one lobby address every lobby instance announces (its row), and its
  `startupurl`,
  the lobby's SessionInit web domain and the menus' `HOST_URL` and `GSB_SERVER` all name
  the one web endpoint, `ONNET_WEB_HOST` with `ONNET_HTTP_PORT`, so all three agree. The
  hosted servers' master address and gate port are `ONNET_PUBLIC_HOST` and
  `ONNET_GATE_UDP_PORT` (every machine, a local stack's container included, lists through
  the gate's public address); the machines reach the controller at `ONNET_INTERNAL_URL`, a
  name that follows it.
- **What an ECS task finds from itself** (`ecs_metadata.*`), where no setting can name
  it before the task runs: the controller, with `ONNET_INTERNAL_HTTP_BIND` unset and the
  task metadata endpoint there (`ECS_CONTAINER_METADATA_URI_V4`), binds its task's
  private IPv4 at 8070.
- **Each process logs `{"boot":{"program":"<program>"}}` once as it starts** (stdout, its
  first line, so a crash loop on a missing setting counts too: the log pipeline counts
  boots per program), keeps printing the `[boot] migrations:` line, logs its own
  heartbeat with its program and the fields it knows, and runs its own stall watchdog
  over its own loops ("Health" below): a stall exits that process alone.
- **A redeploy of the gate, web, the controller or the worker leaves the lobby
  serving**: only a lobby's boot clears lobby rows, and only its own instance's; the
  retail login's `active_user_sessions` rows are never wiped at boot (their 2 h idle prune
  and their account's next login end them); a restarted controller resumes from rows (its
  reconciliation), a restarted worker's leases run out and its jobs run again. A lobby's
  restart drops its own sessions and listings alone, and its hosts list again at the one
  lobby address ("Lobby instances" below).
- **Deployment**: one ECS Fargate service per program (`infra/aws/programs.tf`), the
  gate (UDP 7597), the lobby (UDP 64206) and web (TCP 443 through the portal's nginx in
  web's task, TCP 8080) behind one network load balancer at the gate's address, the
  controller registered in Cloud Map; DEPLOY.md. The local stack (`deploy/compose/`,
  `scripts/dev_stack.sh`; DEVELOPING.md) runs the five, each its own container, plus
  PostgreSQL, Valkey, the portal in web's network as in web's task, and with a data set
  the hosted servers' machines as containers of their own (the docker machine driver,
  "Hosted servers" below).

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_WEB_HOST` | none | The one load-balanced web endpoint (a name or an address) the gate's `startupurl`, the lobby's SessionInit web domain and the menus' `HOST_URL` / `GSB_SERVER` name, with `ONNET_HTTP_PORT`. Required by the gate, the lobby and web. |
| `ONNET_PUBLIC_HOST` | `127.0.0.1` | The gate's public address: its `POSTIPADDRESS` (where the hosts' status blobs land), the hosted servers' master address, the switcher's (web's `GET /api/server-info`). The gate requires it set in its environment (an explicit `127.0.0.1` too): a gate left on the default would announce loopback to every client. |
| `ONNET_LOBBY_ENDPOINT` | none | The lobby: the one lobby address every lobby announces, `<host>:<port>` (its instance row, the gates' `udpnovaworld`; on ECS the load balancer's, the gate's address and 64206); required. The gate: optional, the lobby it hands out while no instance takes clients; with none either, a probe goes unanswered (the client asks again) and the gate logs it, once every 10 s at most. |
| `ONNET_INTERNAL_URL` | none | The controller's internal listener as the machines' agents reach it, `http://<host>:<port>` (`controller.<deployment>.opennova.internal:8070`, the deployment's Cloud Map name, in a deployment; the controller's compose name in the local stack). Required with hosting on. |
| `ONNET_HEALTH_PORT` | 8081 | The TCP port `GET /livez` answers on in the gate, the lobby, the controller and the worker (web's is its HTTP port); 0 lets the OS pick (the tests). |

## Thread model

- **The main thread owns the boot and the program's main loop.** It migrates and seeds
  the database, starts its program's parts, then ticks every `tick_interval_ms` (the
  lobby's `manager.tick(now)` and its `UnknownTracker` flush, the gate's and web's flush)
  and writes the heartbeat. The prunes (the host-staleness sweep among them) are periodic jobs
  ("Background jobs" below).
- **Each listener runs on its own internal thread(s).** They mutate shared state
  through the `ConnectionManager` verbs (add / refresh / drop) and the
  `NwUdpListener`'s `lobby_states_` map (guarded by `lobby_states_mu_`); the
  higher-level invariants (the per-peer session-state map, the unknown
  accumulator) carry their own mutexes.
- **Nothing outside the lobby touches the lobby's memory.** The NW UDP listener's
  sessions, its `ConnectionManager` registry and its socket are its receive thread's
  (and the main thread's `manager.tick`, whose drops land in `erase_lobby_state`). The
  lobby keeps a row per session in `lobby_sessions`, and takes the commands addressed
  to it from `lobby_commands` ("The lobby's tables" below): the HTTP routes and the
  hosted servers' supervisor read the one and insert into the other, never calling
  into the listener: they run in other processes (web and the controller).
- **Every thread has its own database connection.** A `db::Database` is used by
  one thread at a time: its row counts, its error text and any open transaction
  belong to the connection. Each process owns one `db::ConnectionPool` on
  `DATABASE_URL` (at most `DATABASE_POOL_MAX` connections). A thread holds one
  lease at a time, and code holding one hands its `db::Database&` to what it calls
  (a second lease on one thread can wait on itself once the pool is out; a debug
  build asserts it). The gate and NW UDP receive threads each lease a connection
  for their lifetime (each thread leases its own, and `start()` waits for it, so a
  database that cannot be reached stops the boot); the main thread holds one for
  the boot's work and one for its main loop, and none while it starts or stops the
  listeners and workers; each HTTP request leases one for the handler; and
  `erase_lobby_state` (the `on_lost` sink, run on the NW UDP thread and on the main
  thread) writes on the calling thread's own. A `db::Error` carries the server's SQLSTATE
  (`unique_violation()`, `lock_timeout()`, `connection_lost()`) and its text
  names the failing statement, never a bound value, so it goes to the server
  log alone: a reply to a database failure carries only its error code
  (`db_error`). A write that spans statements (`clear_unheld`, `replace_roster`,
  `apply_status_blob`, the lobby's host row with its roster, a registration's
  checks and insert) is one `db::Transaction`, which takes the narrow locks its
  invariant needs ("Database" below), and a read that spans them (`/api/hosts`
  and the `.gsb` feeds: the rows, then each roster) is one `db::ReadSnapshot`, so
  a host never goes out beside another write's roster.
- **The background jobs run on the job workers' threads** (`ONNET_WORKER_THREADS`,
  "Background jobs" below), and the periodic jobs' scheduler on a thread of its own;
  a worker leases a pooled connection to claim a job and to finish it, and its handler
  leases its own around its database work, never across a slow call.
- **The controller runs on a thread of its own** ("The controller" below): elected
  among the processes on the database, it starts the hosted servers' supervisor, the
  console drain and the periodic jobs' scheduler while it leads; a standby's thread
  waits for the election's lock.
- **No request reads back what another request left in this process's memory.**
  The HTTP layer keeps what a later request reads (the retail `NW*.dll` login, join
  and host-key sessions, the retail login pins, every rate limiter's buckets) in
  Redis ("Redis" below), so several HTTP listeners on one database and one Redis
  serve the same clients whichever one each request reaches. A handler leases a
  Redis connection for one command or script at a time (`RedisPool`, at most 16).
- **The hosted servers' supervisor runs on a thread of its own** (with
  `ONNET_HOSTED_PROVIDER` set; "Hosted servers" below), started once the controller
  leads (after the gate has its port) and stopped before the listeners, so the local
  provider's servers deregister while the service still serves. Each pass leases a
  pooled connection, and never calls the cloud: its machine driver's calls are jobs. Each running
  hosted server's admin link is a thread of its own too (at most `ONNET_HOSTED_CAP`),
  ended after the supervisor and before the provider, so its session ends with `QUIT`
  while its server still runs; a link leases a pooled connection to write its console.
  The console drain claims the routes' commands every 20 ms and runs each on a thread of
  its own until its link answers.
- **The `UnknownTracker` records from listener threads** into an in-memory,
  mutex-guarded accumulator and is flushed to the DB only on the main tick — never
  per packet (the gates' and the lobbies' sightings, web's 404s). `GET
  /api/unknowns` (admin) reads the `unknown_messages` rows every process flushed into
  (each flush adds its sightings since the last to the row's count, so several gates,
  lobbies and web replicas sum up rather than overwrite one another),
  the latest seen first (500 at most, `?channel=` one channel's): each signature's
  first sample as hex and its sender's address. What a process keeps is bounded: a
  signature is its printable form cut at 128 bytes (a longer one ends in a hash of the
  whole), a channel keeps 1024 distinct signatures and counts every later new one in its
  `(other)` row, the times are the wall clock's, and `unknown_messages.prune` deletes
  the rows unseen for 30 days.
- **Each listener's `start()` holds its port before the boot goes on.** The gate and
  NW UDP listeners bind their socket in `start()` and move it into the receive
  thread (which leases its connection), so the port they report is the one they serve,
  also with port 0 (the OS's pick). The HTTP listener's `start()` returns once Crow
  serves (Crow's first tick tells it, with the port Crow bound). A bind that fails
  stops the boot.
- **A gate hands out the lobby address only while a lobby serves**: a lobby registers
  its instance (and so counts for the gates) only once its socket is bound, and its stop
  drains the instance first ("Lobby instances" below). What each listener advertises is the
  config's ("Programs" above), never a port another bound; the HTTP menus' `HOST_URL` /
  `GSB_SERVER` name `ONNET_WEB_HOST` and the port Crow serves (`ONNET_HTTP_PORT`; a
  test's port 0, the OS's pick).
- **Each process owns SIGINT and SIGTERM** (Ctrl+C; `docker stop` and an ECS task's
  stop send SIGTERM). The handler only raises the shutdown flag and puts the signal's
  default action back, so the same signal again ends a shutdown that is stuck. Not when
  the program is a container's PID 1: the kernel drops a default-action SIGINT or
  SIGTERM sent to a PID namespace's init (pid_namespaces(7)), so the ECS task
  definitions (`initProcessEnabled`) and the compose files (`init: true`) run it under an
  init, which is PID 1 and forwards both signals; a bare `docker run` of an image needs
  `--init` for the same. The main loop sees the flag within one tick and its program
  stops in order: the main loop's lease back to the pool and its health listener closed,
  then the gate's receive thread, the lobby's drain and NW UDP receive thread (each
  returns its lease) and its connections (`Shutdown`), web's HTTP listener (Crow's
  threads joined, a held-open request included), the controller's parts in reverse and
  its lock, the worker's pool (each worker once its job has) and its driver. As `main()` returns, its
  locals go in reverse declaration order, so the pool (declared first) closes last. The HTTP
  listener clears Crow's own signal set (`signal_clear()`), which would otherwise
  take both signals and stop only Crow.

## Health: the watchdog, `/livez`, `/healthz` and the heartbeat

Service policy (`health.*`), what the container runtime and the uptime checks read:

- **The stall watchdog, one per process, over its program's loops alone.** The gate's
  receive loop and the lobby's NW UDP receive loop (each wakes at least every 250 ms with
  nothing to read); the
  controller's supervisor (a pass every second, a turn per server it steps; it reads and
  writes rows alone, the cloud's calls being jobs) and job scheduler (a tick every
  second), each with the election's turns standing in while it waits as a standby; the
  worker's job pool (its workers' claims and idle waits and its keeper's passes: a slow
  job, an AWS CLI call, Stripe or SES each bounded by its own timeout, occupies its
  worker alone, so only a pool whose every thread stands still stalls); and in every
  program its main loop (its tick and its heartbeat, which read the database). Each counts
  its turns, and a thread of the watchdog's own samples the counters every second. A
  loop whose count has not moved for 30 s is stalled: the process logs `[watchdog]
  FATAL the <loop> made no progress for 30 s; exiting 70` and exits 70 at once (nothing
  in the process can unstick a thread; an orderly shutdown could hang on it), and the
  runtime starts it again (ECS replaces the task; compose's `restart: unless-stopped`);
  the other programs' processes are untouched.
- **`GET /livez`**, the process's liveness: `ok`, or `503 stalled` while a watched loop
  has stood still for 30 s or the watchdog's last sample is that old. The gate, the
  lobby, the controller and the worker answer it on their health port
  (`ONNET_HEALTH_PORT`, 8081; `health_listener.*`: one thread, one connection at a time,
  each request read for 2 s at most, `HEAD` answered alike, another path a 404, the port
  bound as the program becomes ready), web on its HTTP port. Each program's ECS
  container health check asks it (`curl -fsS --max-time 2`, every runtime image carries
  curl), and so does each image's `HEALTHCHECK`; the gate's load balancer target group
  checks it too. A cache outage fails no web task's liveness, since no restart and no
  new task mends it, and a new web task must still come up healthy while it lasts.
- **`GET /healthz`** (web's, on its HTTP port), whether it serves a retail client: `/livez`'s
  answer, else `503 redis unreachable` while Redis does not answer a `PING` (every
  retail login route needs it; one `PING` a second at most, its answer reused meanwhile,
  and a line in the log when the answer changes). The load balancer's target group and
  the Route 53 check use it, so the balancer routes around a web task that cannot
  reach the cache.
- Both give no other detail (`Cache-Control: no-store`) and sit outside `/api`, so
  `ONNET_API_PROXY_ONLY` never refuses them.
  `GET /api/health` stays, a static `{"status":"ok"}` behind the proxy rule.
- **The heartbeat**, one line on stdout every 60 s, for the log pipeline's metric
  filters: `{"heartbeat":{"program":"<program>",...}}`, the program first, then the
  fields that program knows in this order, then every program's lock failures:
  `{"heartbeat":{"program":"controller","hosted_live":N,"hosted_failed":N,"machines_lost":N,"launch_failures":N,"limits_held":N,"db_lock_timeouts":N,"db_deadlocks":N}}`.
  The lobby logs `nw_sessions` and `listed_hosts`; web `webhook_rejects` (with billing
  on); the controller, with hosting on, `hosted_live`, `hosted_failed`,
  `machines_lost`, `launch_failures` and `limits_held`; the worker `billing_sync_age_s`;
  the gate `probes_unanswered` and `lobbies_live`. A metric aggregated over the programs
  so counts each once. `nw_sessions` counts a lobby's own NovaWorld UDP sessions, `listed_hosts` the
  deployment's listed servers (every lobby logs the same count: aggregate it with the
  maximum, not the sum),
  `hosted_live` the live hosted servers; `hosted_failed` and `launch_failures` count the
  hosted servers that ended failed and the launches that failed (refused, or a machine
  that never booted, as the supervisor's passes count them) since the last line;
  `machines_lost` counts the fleet's machines lost now (their agents silent); `billing_sync_age_s` is the age
  in seconds of the oldest webhook event the billing sync has not synced (0 with none,
  -1 with billing off), `webhook_rejects` the webhook deliveries refused since the
  last line, `limits_held` is 1 while the hosted servers' limits hold is on (0
  otherwise), `probes_unanswered` the gate's probes left unanswered since the last line
  for want of a lobby (no instance takes clients and no fallback: a gate that answers no
  one while its loops turn and its `/livez` says ok, which the deployment alarms on),
  `lobbies_live` the lobby instances that take clients (the deployment's count: every
  gate logs the same, so aggregate it with the maximum), and `db_lock_timeouts` and `db_deadlocks` count the database statements,
  on any of the process's connections, that failed since the last line waiting on a lock
  past the lock timeout (`55P03`) or ended by the server to break a deadlock (`40P01`).
- **Every image runs its program as UID 10002** (`novaworld`, no home, no shell): no
  program keeps state in its container, and each writes only temporary files under
  `/tmp`.

## Background jobs

The work no request or pass waits on runs as jobs on one queue in the database
(`jobs/`, `backend/migrations/0007_jobs.sql`, the river/Oban/good_job pattern): the
fleet's cloud calls, the billing sync, the outbound mail and the prunes. Service
policy, not a port.

- **A job** is a `jobs` row: its kind, a JSON payload, its state (`queued`, `running`,
  `done`, `failed`), when it may run, its attempts out of `max_attempts`, the worker
  holding it and its lease, why it last failed. What the payload must not keep (a
  machine's one-time bootstrap token, a mail's body with its reset link) is the row's
  `secret`, dropped when the job ends (an empty text where it was, so the row still says
  it carried one, never what). A job is enqueued on the caller's connection, so
  inside its `db::Transaction` it commits with the state change that causes it (a
  machine's row and its launch), never one without the other.
- **Dedupe and schedule keys.** A dedupe key is unique among the queued and running jobs
  (a machine has at most one describe waiting; an enqueue that finds one is no job); a
  schedule key is unique for good: a periodic job's kind and its time bucket
  (`<kind>@manual-<ms>` for an admin's run now).
- **Claims.** An idle worker claims the oldest due job of the kinds it handles under the
  claim lock (an advisory lock of its own): the kinds at their cap
  of running jobs are left out (at most 8 EC2 launches at once, for the AWS API's rate
  limits), then `UPDATE ... WHERE id = (SELECT ... FOR UPDATE SKIP LOCKED LIMIT 1)`. It
  polls every 500 ms, backing off to 2 s while idle; no LISTEN/NOTIFY.
- **Leases.** A claim holds the job for 60 s; the pool's keeper extends the leases of the
  jobs its workers run every 10 s and puts back every job whose lease ran out (its worker
  went away): queued again, its attempt counted, or failed once its attempts are used up.
  A lapsed claim's finish changes nothing, so the handlers are idempotent: a launch run
  again names the same machine id, so the same client token in each subnet, and calls
  first in the subnet its row recorded before the earlier run's call
  (`fleet_machines.launch_subnet`, written before each subnet's first `run-instances`;
  one that cannot be written calls nothing), so EC2 answers it with the machine it
  launched then, however late EC2 lists that machine. A run again whose row recorded none
  reads where an earlier run launched from a `describe-instances` of its machine id (one
  that cannot read it launches nothing).
- **Retries.** A handler answers done, a retry (an exception is one) or a failure for
  good. A retry waits its kind's backoff, doubling per attempt to its cap, jittered to
  between half and all of it (or the wait the handler names); past `max_attempts` the job
  is `failed`, its error on the row, and the admin's jobs page lists it.
- **Periodic jobs** (`jobs/scheduler.*`): each tick (every second) enqueues each periodic
  kind once per time bucket, keyed `<kind>@<bucket>`, and none beside one still queued or
  running; some also run at the scheduler's start, and the billing sync's bucket is
  enqueued only while an event waits.
- **The prune**: finished jobs are deleted 14 days after they end (`jobs.prune`, hourly;
  an admin's prune names a shorter keep in its payload, `keep_s`, a day at the least).

| Kind | What | Enqueued by | Cap |
| --- | --- | --- | --- |
| `machine.launch` | the row's launch (`hosted/fleet_jobs.*`) | the supervisor, with the machine's row | 8 |
| `machine.describe` | the cloud's word on a live machine, onto its row | the supervisor's boot and lost checks, deduped per machine | 4 |
| `machine.terminate` | a cloud machine ended | the supervisor's ends | 4 |
| `fleet.sweep` | the driver's machines against the rows | every 5 min, and at the supervisor's reconciliation | 1 |
| `billing.reconcile` | the full reconcile with Stripe | at the start, every `ONNET_BILLING_RECONCILE_MINUTES`, an admin | 1 |
| `billing.sync` | the customers with an event to sync | each fresh webhook event; every minute while one waits | 1 |
| `mail.send` | one mail, at most once | the welcome and reset routes | 4 |
| `hosts.prune`, `host_keys.prune`, `web_sessions.prune`, `active_user_sessions.prune`, `login_addresses.prune`, `password_resets.prune`, `billing_events.prune` | the prunes (`active_user_sessions.prune`: the retail logins' rows unused for 2 h) | every `HOST_SWEEP_INTERVAL_MS` | 1 |
| `stats.prune` | the hosted servers' samples past 48 h, their hours past 90 days, the machines' months past 13, the fleet's machines ended or failed 14 days ago | hourly | 1 |
| `jobs.prune` | finished jobs past 14 days (or its payload's `keep_s`) | hourly, an admin | 1 |
| `unknown_messages.prune` | the unknown-message rows no process has seen for 30 days | hourly | 1 |

**The admin's view** (`jobs/job_admin.*`, `jobs/job_routes.*`; the site's admin page
**Background jobs**, `/admin/jobs`): what the rows show, read on web from the jobs
table and the controller's row alone, so every replica answers alike. A worker process
shows by the claims its threads left on the rows (their ids, their jobs' starts and
leases): one none of whose threads has run anything for an hour does not show, and a
running job whose lease ran out (its job still `running`) is one whose process has gone
away, queued again at the next keeper pass. The failures to look at come grouped by the
database: one group per kind and **failure key**, the error with its UUIDs, ISO 8601
times, tokens of 8 or more letters, digits, `_` or `-` holding a digit (request ids, cloud
machine ids) and other numbers taken out (`<id>`, `<time>`, `#`), its first 300
characters, so fourteen launches that failed alike are one row with a count. The schedules
are the controller's, read from the same settings (`parts.*` `periodic_jobs`). Each action
is one compare-and-set on the job's state (a group's retry, one per job), so none changes a
job a worker holds; each is logged as every admin state change is (its actor and status)
and once more with the job (`[jobs] job <id> (<kind>) ...`). The page shows four counts
over three tabs (`?tab=`): Overview (the failure groups, each linking to its jobs and with a
retry of the ones that can run again; the worker processes; each kind's counts), Jobs (the
filtered, paged list, its filter in the page's query, and the prune) and Schedules (each
periodic kind, run now). It reads the overview every 15 s (every 5 s while a job runs or
waits) and shows times relative and in UTC, durations in seconds, minutes and hours.

| Route | Auth | Reply |
| --- | --- | --- |
| `GET /api/admin/jobs/overview` | admin | `{"at", "totals", "kinds", "attention", "attention_total", "workers", "schedules", "controller"}`: per kind (and in `totals`) `queued` (due or not), `due`, `running`, `done_hour`, `done_day`, `failed_hour`, `failed_day`, `failed` (kept until the prune), `oldest_wait_seconds` (the oldest due job's wait: the queue's latency; null with none) and `mean_run_seconds` (the last hour's done jobs, claim to end); `attention` the newest 20 failure groups, the jobs of one kind failed in the last day with one failure key, an admin's cancels left out (`kind`, `key`, `count`, `retryable` (how many a retry takes), `last_job_id`, `last_error` (the newest one's text), `last_at`), by their newest failure; `attention_total` every job the groups count, shown or not; `workers` each worker process with a thread holding a job or done with one in the last hour (`process`, `threads`, `busy`, `lapsed` (its running jobs whose lease ran out), `lease_left_seconds` (the least left of its others, null with none), `done`, `failed`, `last_active_at`); `schedules` each periodic kind (`kind`, `interval_seconds`, `at_start`, `conditional`, `last` and `last_finished` (jobs or null), `next_at`, `pending`); `controller` (`leader`, `elected_at`, `last_scheduled_at`). A job, here and below: `id`, `kind`, `state`, `attempts`, `max_attempts`, `last_error`, `run_at`, `created_at`, `started_at`, `finished_at`, `lease_until`, `lease_left_seconds` and `running_seconds` (a running job's, else null), `worker_id`, `dedupe_key`, `schedule_key`, `manual`, `secret_state` (`none`, `held` or `dropped`, never the secret), `retryable`, `cancellable`. Times ISO 8601 UTC; `Cache-Control: no-store` |
| `GET /api/admin/jobs` | admin | `?state=` (`queued`, `running`, `done`, `failed`), `?kind=`, `?from=` and `?to=` (Unix seconds, on when a job was created; `to` excluded), `?q=` (within the dedupe or schedule key, any case), `?failure=` (a failure key, exact: an attention group's `key`, 300 bytes at most), `?before=<id>` and `?limit=` (1..200, 50): `{"jobs", "total", "next_before"}`, the jobs newest first, how many the filter takes, and the next page's `before` (null on the last); 400 `invalid_field`; `Cache-Control: no-store` |
| `GET /api/admin/jobs/<id>` | admin | `{"job"}` with its `payload`; 404 `not_found` |
| `POST /api/admin/jobs/<id>/retry` | admin | a failed job queued again to run now with one more attempt than it has used (`max_attempts` becomes `attempts` + 1; its last error kept until it runs): 200 `{"job"}`; 404 `not_found`; 409 `not_failed` (with its `state`), `secret_dropped` (its secret went as it ended: a mail, a machine's launch), `already_pending` (a job with its dedupe key waits or runs) |
| `POST /api/admin/jobs/retry-group` | admin | `{"kind", "key"}`: each job of that attention group (of `kind`, failed in the last day with failure key `key`, an admin's cancels left out) queued again as its own retry would, oldest first: 200 `{"retried", "secret_dropped", "already_pending", "changed"}` (how many each outcome took: queued again, its secret gone, a job with its dedupe key waiting or running, no longer failed); 400 `invalid_json`, `missing_field`, `invalid_field` |
| `POST /api/admin/jobs/<id>/cancel` | admin | a queued job failed for good, `cancelled by an admin (<actor>)` its error, its secret dropped: 200 `{"job"}`; 404 `not_found`; 409 `not_queued` (with its `state`: a running job is its worker's) |
| `POST /api/admin/jobs/run` | admin | `{"kind"}`, a periodic kind this deployment schedules, enqueued now as its schedule would, keyed `<kind>@manual-<ms>`: 202 `{"job"}`; 400 `invalid_json`, `missing_field`, `invalid_field`; 404 `not_found` (no schedule names it); 409 `already_pending` (one of the kind waits or runs) |
| `POST /api/admin/jobs/prune` | admin | `{"keep_days": 1..14}` (14 when absent): a `jobs.prune` job with that keep, now: 202 `{"job"}`; 400 `invalid_json`, `invalid_field`; 409 `already_pending` |

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_WORKER_THREADS` | 4 | The job workers' threads (1..64), and at most `DATABASE_POOL_MAX` less two (the pool's keeper's connection and the one the process holds): asked for more, that many start, with a boot warning. |

## The controller

`apps/novaworld_controller/controller.*` (migration `0008_controller_state.sql`): of the
controller processes (`apps/novaworld_controller/main.cpp`), the one that runs the machines' internal
listener, the hosted servers' supervisor (with its admin links), the console drain and the
periodic jobs' scheduler, elected among them. Service policy, not a port.

- **The election.** A session advisory lock (`db::SessionLock`, its own key) on a
  connection of the controller's own, outside the pool, tried again while another
  process holds it (each try waits the lock timeout): a standby waits there. Once it
  holds the lock, the controller moves `controller_leader.generation` on and names
  itself (`holder`, a random `controller-<hex>`), then starts its parts: the internal
  listener (bound only now, so the deployment's internal name reaches the process that
  renders the start files; a bind that fails ends the process), the supervisor (which
  reconciles), the console drain, the scheduler (whose start enqueues the billing
  reconcile).
- **The fence.** Every check (each second) it reads the row on its lock's connection.
  The connection gone (and the lock with it) or a generation not its own is the
  leadership lost: the server logs `[controller] FATAL ... lost the leadership` and
  exits 71, and its container starts it again as a standby. Every supervisor pass and
  scheduler tick first asks that it still leads (its flag, and the row's generation its
  own), so a stale leader that has not noticed yet starts nothing once another leads; and
  a pass asks again inside each write that enqueues cloud work (a machine's row with its
  launch, an end with its terminate), the row share-locked (`FOR SHARE`) to that write's
  end, so an election waits for the write (the new leader reads it) and a write after an
  election writes nothing: two leaders never launch two machines for one server.
- **A stop** (the shutdown) stops its parts in reverse and lets the lock go, so a
  standby leads within its lock timeout; a new leader resumes from the rows: the
  machines' timers and each kind's launch backoff (`fleet_machines`,
  `fleet_launch_backoff`), the jobs, the limits hold. What it keeps in memory alone is
  its own: the servers' runtimes (a new leader's reconciliation treats them as a
  restart's), the started servers' files, the admin links.
- **What the other programs read of it is rows**: the machines and servers, the owners'
  limits' hold (`hosted_limits_hold`), each admin link's state (`hosted_admin_links`),
  the console's answers (`hosted_console_commands`, `hosted_console_replies`).

## Lobbies: one per title

Each `games` row is a lobby ([`catalog_repository.h`](catalog_repository.h)). The rows
are `backend/seed/0001_games.sql`'s, which every boot applies after the migrations
(`ON CONFLICT DO NOTHING`, so a row added there reaches every database at its next
boot); `title` is the engine's game profile code the row's clients speak
(`engine/base/gameprofile`), and the gate's per-row answer (`startup_query`,
`glsvss_*`).

| Gate tag | Slug | LobbyName | Title | Start page | Browser list | pfid | Expansion bits |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `jop:cus2`, and `jopd:cus4` (the JO demo's) by alias | `jop_2_consumer` | `jop_2_consumer` | `jo` | `jop_2_start.htm` | `/jop_2.gsb` | 28 | 3 |
| `dfx2:0:cus:buffy` | `dfx2_consumer` | `dfx2_0_consumer` | `dfx2` | `dfx2_0_start.htm` | `/dfx2_0.gsb` | 38 | 1 |
| `opennova:0` | `opennova_0_consumer` | `opennova_0_consumer` | `jo` | `jop_2_start.htm` | `/opennova_0.gsb` | 28 | 1 |

The OpenNova row (ON data) speaks JO's wire identity, so its title, version pair
(3 / 2345), product family, pages and gate answer's shape are JO's; its gate tag,
LobbyName and browser list are its own, so anyone running ON data lands in its lobby.
Expansion bits: bit 0 is the base game and bit 1 JO's `jox01`; DFX2 and ON have no
expansion.

- **The gate** answers a probe from the row its tag selects: `LobbyName`, the
  `startupurl`'s query (`startup_query`) and the GLSVSS leg (`glsvss_*`; a row with none
  takes the configured `ONNET_GLSVSS_*`, none by default). JO's reply is the bytes it
  always was. DFX2's answers what NovaLogic's live gate answered the DFX2 tag on
  2026-10-14 (`dfx2_0_consumer`, `pfid=38` in the `startupurl`, `[DFX2]KILLS` /
  3600000 / 45000), in this gate's shape: the `POSTIPADDRESS` / `POSTIPPORT` pair, which
  the live DFX2 reply lacks (its `udpnovaworld` line lets a client skip them), and this
  service's address in the `startupurl` where the live one has `[domainname]`; its
  `TimeOnGateServer`, a key no client reads, is not sent. A tag no row (nor the alias)
  answers to gets no reply, and the unknown tracker records it. The gate reads the table
  once in `start()`, so its receive thread never touches the database for a probe; a change to
  the table takes a restart.
- **Listings** carry the LobbyName the gate gave their host (`active_hosts.game`), so a
  host lands in its own game: `/api/lobbies` groups the hosts by it (one group per row),
  and each row's `.gsb` route lists its own alone, showing the row's `ver1` and
  `exp_bits` in a column the host reported nothing for.
- **The `NW*.dll` pages** fill in the requesting client's game's values: its browser
  list as `GSB_SERVER`, its pages where the request names none, and its expansion bits
  (the login's `EXPBITS`, the join's requirement). The game is the gate tag the client
  sends, the `gt` query `/nwprepare.dll` carries (the `[GT]` the gate's `startupurl`
  has it substitute) and, on every later request, the `GT` cookie that page sets (a stock
  client sends its cookies back on every request); else its product family (the
  request's `pfid`, else the `NWPF` cookie `/NWJoin.dll` and `/NWHost.dll` set); else the
  prefix of the page it names (`dfx2_0_`); else JO's. JO and ON share pfid 28 and JO's
  pages, so only the tag tells them apart. The HTTP listener also reads the table once in
  `start()`, where it registers the `.gsb` routes.

## Connection lifecycle

A connection is created on `ClientHello`/`ClientAuth` and dropped for one of four
reasons (`ConnectionManager::DropReason`):

| Reason | Trigger |
| --- | --- |
| `Logout` | `ClientGoodBye` (0x46) or `ClientStopHosting` |
| `Timeout` | receive silence strictly longer than the cs[0] timeout the 0x82 SessionInit advertises (240 s, `novaworldudp_session_timeout_ms()`; not configurable, so the reap can never disagree with the contract the peer was told) |
| `Replaced` | a new `ClientHello` from the same address evicts the prior entry |
| `Shutdown` | the server is stopping |

Every drop, regardless of reason, fires `on_lost` → `NwUdpListener::erase_lobby_state`,
which removes the in-memory per-peer session state, the matching `active_hosts` /
`host_players` rows (the host row by RID, the player row by peer address) and the
session's `lobby_sessions` row (with the commands addressed to it). So a host that quits,
times out, or is replaced disappears from the browser the same way.

A **backstop sweep** runs as the periodic `hosts.prune` job (every
`host_sweep_interval_ms`, default 30 s): it prunes `active_hosts` rows whose `updated_at` (refreshed by every
`ClientHostUpdate` heartbeat) is older than `host_stale_window_ms` (default 5 min).
This only catches rows the normal teardown somehow missed; `host_players` rows
cascade. The same job reaps the lobby instances whose process is gone ("Lobby
instances" below), with their rows. A lobby's start clears the rows no instance holds
(`hostdb::clear_unheld`: the dev inject's listing); every instance's stay (each start
registers an instance of its own: a previous run's rows went at its clean stop, or go
with its stale row), and no other program's boot touches them.

**Lobby instances** (`backend/migrations/0009_lobby_instances.sql`,
`0012_lobby_instances_share_endpoint.sql`, `lobbydb::register_instance` and its neighbours;
service policy). Each lobby process is one instance, a row of its own, and as many lobbies
run as the deployment wants, all behind one lobby address (`ONNET_LOBBY_ENDPOINT`), which
every lobby announces alike: on ECS the network load balancer's, the gate's Elastic IP and
UDP 64206, which spreads the clients across the lobby tasks by flow (a client's address and
port) and keeps each flow on one task.

- **Why one address**: a stock host's NovaWorld session reconnects only to the lobby
  address the gate gave it at its first connect (it never asks the gate again) and drops a
  silent lobby only after 240 s [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290C0,
  the target walk @0x629183..0x6291B4; CNapiNPConnection_PumpStateMachine @0x6292e0, case 5
  @0x6295a2..0x62961c]. With one address, a host whose lobby went away times out,
  reconnects to that address, lands on a live lobby, re-verifies with CurrentlyHosting=1
  and is listed again, about 271 s at most after its listing went (240 s of silence, the
  1 s first delay, a 10 s probe window, a 20 s join); retail self-hosts recover the same
  way. The new lobby holds no session for the host's datagrams before that and drops
  them unanswered, so the host's own timeout is what moves it.
- **Its row** (`lobby_instances`: id, endpoint, `active` or `draining`, `started_at`,
  `last_seen_at`). The NW UDP listener's start registers a new row, deletes every instance
  not seen for 5 min with its rows (`reap_stale_instances`, `kInstanceStaleSeconds`: well
  past a database failover, 60 to 120 s, when no lobby can move its row), and clears the
  rows no instance holds, all before it serves. Its receive thread moves `last_seen_at`
  every 5 s; a row found gone all the same (reaped while the lobby could not reach the
  database for longer than that) is registered again under a new id, draining still when
  it was (its own stop's drain, or an admin's, which each move of `last_seen_at` reads
  back), and the lobby writes its session rows and listings anew from its memory and
  serves on (a command the reap took is lost, as any command may be); a clean stop
  drains it, then deletes it, and its rows with it. Every row the lobby writes names its
  instance (`lobby_id`): its session rows, its listings (`active_hosts.lobby_id`), and
  the commands addressed to them.
- **The gate hands every client the one lobby address** while at least one instance takes
  clients (active and seen within 15 s; read every 5 s, `lobbydb::kGateRefreshSeconds`,
  on the gate's own connection: `lobbydb::live_lobbies`, the newest live instance's
  endpoint, which every instance shares, and their count). After a read that failed its
  fallback lobby serves every client, or none is answered, since the instances it read
  before may all have stopped since, and the next read comes six refreshes later, so a
  database outage does not hold the gate's thread for a connect timeout at every
  refresh. With none live, its fallback `ONNET_LOBBY_ENDPOINT`, and with no fallback no
  reply at all (the client asks again; the gate logs it every 10 s at most). The reply is
  the gate's one send, to the probe's source at once: it sends no datagram of its own,
  so behind a UDP load balancer every datagram it sends rides the probe's flow back.
  `POSTIPADDRESS` stays this gate (the hosts' status blobs land there,
  `apply_status_blob` finds the listing by its key whichever lobby holds it) and the web
  fields stay `ONNET_WEB_HOST`.
- **The lobby behind the balancer**: its target group (UDP 64206, IP targets, client
  addresses and ports preserved, so a lobby lists a host at its own address and port)
  checks each task's `/livez` on the health port. A deployment starts the new lobby task
  first; once it passes, ECS deregisters the old one: the balancer sends it no new flow,
  keeps its flows 10 s (the deregistration delay), then ends them, and ECS signals the
  task only then. A lobby always answers on the flow a datagram came in on, so its
  replies and pushes ride that flow back through the balancer.
- **Draining**: an instance marked `draining` (`POST /api/admin/lobbies/<id>/drain`, or its
  own lobby as it stops) no longer counts for the gates: they hand out the lobby address
  while another instance is live, and nothing with none. Its sessions stay until they
  leave. Which task a client reaches is the balancer's, which a task leaves by its
  deregistration (its stop); an admin's drain does not take it out of the balancer. Its
  row reaped while its lobby runs, it registers again draining. A lobby's stop drains its
  instance and stops at once: the balancer drained its task before the signal.
- **RIDs come from one sequence** (`lobby_rid_seq`, `lobbydb::next_rid`: 0x0A000000 and
  1..0xFFFFFF, cycling, a suffix a listing or a session row still holds passed over), so
  two instances never mint one RID, and none is reused short of the cycle nor while it
  is held: a hosting key's claim on a RID no listing holds is free already, and no boot
  resets the claims. A host that relists on another instance gets a new RID.
- **Pushes need no instance named**: a push or an end takes its session row's `lobby_id`
  as it is inserted, each instance takes only its own instance's commands, and a
  `forget_owner` goes to every instance (one row each, active or draining).

| Route | Access | Replies |
| --- | --- | --- |
| `GET /api/admin/lobbies` | admin | `{"lobbies"}`, by id, each `id`, `endpoint`, `state` (`active`, `draining`), `started_ms`, `last_seen_ms` (Unix epoch ms) and `sessions` (its session rows) |
| `POST /api/admin/lobbies/<id>/drain` | admin | 200 `{"status": "draining", "id"}`; 404 `not_found` for no such instance |

**The lobby's tables** (`backend/migrations/0006_lobby_commands.sql`,
`engine/net/novaworld/lobby_repository.h`; service policy):

- **`lobby_sessions`**, one row per lobby session the listener holds, which it alone
  writes: inserted with the `ClientHello` that admits the address, replaced under a new
  id by a `ClientAuth` that is not the accepted one's retransmit (a new logical
  connection), updated as the session's statements change what the row holds (its
  retail login's tag, its RID, hosting and listing), and deleted with the drop. Its
  connection half is the registry's (the id, the PN, the NA, the state, when it
  connected, as wall-clock times); `last_seen_at` is written every 10 s at most, so it
  lags by as much. The receive thread writes the row before it answers the statement
  that changed it, and with the times it deletes its instance's rows of no session it
  holds (a drop forgets its session before it deletes the row, so a delete that failed
  would leave a row answering pushes for a RID no session holds).
- **`lobby_commands`**, what the routes and the supervisor address to the lobby that
  holds a session: `server_command` and `stop_hosting` (the pushes below),
  `end_session` (the admin's end) and `forget_owner` (an account lost its keys). An
  insert answers from the session rows what the lobby would (a RID no session holds,
  one not hosting, another account's, a session that ended or hosts) and is made only
  when the lobby would take it. The receive thread takes them between
  receive batches (a batch ends at the empty read or at 256 datagrams, so a flood that
  never empties the socket still lets the commands, the session times and the
  instance's heartbeat run; a malformed datagram is logged once per 10 s at most, with
  the count since), every 250 ms at most, oldest first (each deleted as it is read),
  checks each again against its memory, and runs it; one whose session is gone or
  changed is dropped, and so is one that waited past its 30 s expiry. A command is best
  effort, as the pushes always were.
- Each lobby instance writes its own id on its session rows, a command takes its
  session's `lobby_id` as it is inserted, each instance takes only its own, and a
  `forget_owner` goes to every instance (one row each); "Lobby instances" above.

**The admin's view** (`connection_routes.*`, the site's admin page `/admin/sessions`):
every session the lobby instances hold, read from their `lobby_sessions` rows, who it
is, the server it lists and the instance holding it, and an end, queued for that
instance as a command. A session's account is
the listing's owner while it hosts (the one rule `GET /api/admin/hosts` applies,
`listing_owner`), else the account whose live retail login its latest `Cookie` list
named: a stock client's verify, play, host and GLSVSS requests carry every name/value
pair its pages hold, so after the web login they carry its `LOGINSESSIONTAG`
(`LobbyState::login_session_tag`, which the row keeps and no route sends; an ended login
names no one). The registry stamps the steady clock, which counts from the machine's
boot, so the lobby writes each time as the wall time less its age.

| Route | Access | Replies |
| --- | --- | --- |
| `GET /api/admin/connections` | admin | `{"count", "heartbeat_timeout_ms", "connections"}`, oldest first, each `session_id` (its row's: unique across the instances, what the end names), `lobby_id` (the instance holding it), `id` (the registry's, unique within its instance alone), `state` (`handshaking`, `active`, `closing`), `pn`, `identity` (the ClientAuth `NA`, the game tag: `jop:cus2`), `addr`, `created_ms` and `last_seen_ms` (Unix epoch ms; the last within 10 s), `account` (`{id, username, via}`, `via` `host_key` or `login`; null while neither names one) and `host` (while it hosts: `rid`, `server_name`, `mission_name` (UTF-8), `players`, `max_players`, `listed` (false while it waits out of the browser for a HostKey naming an account) and `hosted_server_id` (null unless it is a live hosted server's listing); else null) |
| `POST /api/admin/connections/<session id>/end` | admin | No body: the session the list names by its row's id (never reused, where a CI and an address are one instance's alone). 202 `{"status": "queued", "session_id", "id", "addr"}`, an `end_session` command for the instance holding it: its receive thread latches the pending-disconnect record `{1, 8, 0, 0, "", 0, "NP.C:PMP:PDESTME"}`, sends the client its `ServerGoodBye` burst (CS field 1 times, keyed by its CK; none to a session still signing in) and drops the connection (`Logout`), as the NAPI server role ends a connection it asked to disconnect (the service's end itself is service policy). 404 `not_found` for a session that ended; 409 `hosting` (with its listing's `rid`: `POST /api/admin/hosts/<rid>/stop` ends it, as ending its session would only park the listing for the reconnect's re-host). A session that changed or began hosting by the time the lobby takes the command is left as it is. Logged as every admin state change is, with the actor and status, and with the session's id, address and game tag; no rate limit, as no admin write has one |

The page lists them sortable by session, account, connection time (newest first) and
last heard; it shows when each connected in UTC and for how long, links the account to
its admin page and a hosted server to its fleet page, and ends a session (or, on a
hosting one, stops its server) after a confirmation.

## Pushing statements to a listed server

The service can send a hosting connection retail's two administrative statements over
its own NovaWorld session: `ServerCommand` (one `Cmd` verb line; the host runs it, so
the service can punt a player, rename the server, change its message, cycle the map
and so on) and `ServerStopHosting` (`MsgCode` / `MsgParam1` / `MsgParam2`).
`lobbydb::queue_server_command` / `queue_stop_hosting` resolve the RID to the hosting
session's `lobby_sessions` row and insert the statement as a command for its lobby
("The lobby's tables" above); the lobby's receive thread, which owns the socket, takes
it within 250 ms and frames it exactly as a reply (one reliable record on the
connection's sequencing, retained until the client's ACK covers it) and answers a 0x44
for it from those records. A connection replaced or dropped, or no longer hosting, by
then loses the push. Between receive batches the
thread also runs the ACTIVE send-interval leg the NAPI server role runs (CS field 5,
1000 ms): a connection with records still unACKed gets one header-only packet per
interval for as long as they stay unACKed, whose fresh sequence shows the client a gap
if the push was lost. A header-only packet draws no ACK from a stock client, so if the
client's ACK itself is lost that is about one packet a second until the client's next
packet carries the ACK (its 60 s keepalive) or the connection is reaped (240 s). The
server role's own 60 s keepalive is not run (D-NET-393: the service already answers
every packet the client sends).

`ServerStopHosting` is the sysop punt. A stock host drops back to verified and, if it
is running a NovaWorld match, ends that match for everyone within 62 frames, goes to
the main menu and resets its NovaWorld session (the RE record's "The exit"); the
service also takes the server out of the browser (and its session row's hosting) once
the statement is framed, before it goes out, since the host answers it with no
`ClientStopHosting`.

The routes live in `host_routes.*` (`HostRoutes`, which the HTTP listener owns beside
its `WebAccess`). Both families compose a command through one helper,
`compose_server_command`: the body `{"verb", "target", "args"}` (`target` one of
`None`, `ByIndex`, `ByIpAndPort`, `ByName`, `ByPCID`, or absent) goes through
`server_command_text`, so a line the host's reader would drop is a 400 with the
composer's reason, and so is an arg holding a control byte (below 0x20, 0x7F, or a C1
control U+0080 to U+009F, which a host would save into its `game.cfg` or relay in chat).

The body is UTF-8 (JSON) and a stock host reads its `Cmd` in its own code page, cp1252
for JO's audience (D-NET-382), so the composer converts each arg to cp1252 first
(`utf8_to_cp1252`, `base/io/cp1252.h`): "café" reaches the host as `caf` and 0xE9. An
arg holding a character cp1252 has none for (an emoji, CJK) is a 400 naming its
codepoints (`U+1F600`), never substituted, and bytes that are not UTF-8 are a 400 too.
Crow's JSON reader decodes each `\uXXXX` escape on its own, so an escaped surrogate pair
arrives as two halves; the composer joins them into the character first, so an escaped
emoji is named like a raw one, and an unpaired half is refused as not UTF-8.
`SetServerName` and `SetServerMsg` texts are counted in the host's bytes: a stock host
keeps 31 and 127 of them (`SERVER_COMMAND_NAME_CAP` / `SERVER_COMMAND_MSG_CAP`,
`Napi_CopyString`'s 32 and 128), so a longer one is a 400 rather than clipped there. A
reply's `cmd` and the log show the composed line back in UTF-8. A host reports its text
back the same way (a name set to "café" comes back with 0xE9), so every JSON list of
listed servers (`/api/lobbies`, `/api/hosts`, `GET /api/me/hosts`, `GET /api/admin/hosts`)
sends a host's text as UTF-8 (`retail_text_to_utf8`: valid UTF-8 as it is, anything else
read as cp1252). The GSB browser blobs and every other retail wire output keep the host's
bytes, which stock clients read as cp1252.

**The admin's pushes** (the Bearer `ADMIN_API_TOKEN`, or an admin-role website session
with the CSRF header; see "Website sessions and roles"), each logged with its actor and
the composed line (control bytes escaped):

| Route | Body | Replies |
| --- | --- | --- |
| `POST /api/admin/hosts/<rid>/command` | `{"verb": "SetServerName", "target": "None", "args": ["New Name"]}`, any of the eighteen verbs | 202 queued (with the composed `cmd`); 400 with the composer's reason; 404 unknown RID; 409 not hosting |
| `POST /api/admin/hosts/<rid>/stop` | none | 202 queued (MsgCode 7, `NWUSERVERMSGCODE_NOVAWORLDSYSOPPUNT`): ends a stock host's running NovaWorld match for everyone and drops the host from NovaWorld (the browser row at once, its session reset with the match's exit); 404; 409 |

**The owner's pushes** (service policy): the account that listed a server ("Listed
servers and owners" below) administers that server, and only it, from the website's
"My servers" page (`/me/servers`).

| Route | Body | Replies |
| --- | --- | --- |
| `POST /api/me/hosts/<rid>/command` | as the admin's, any verb but `SetMPReset` | 202 queued (with the composed `cmd`); 400 with the composer's reason; 403 `verb_not_allowed` for `SetMPReset`; 404 unless the server is the caller's; 409 the caller's server is not hosting; 429 |
| `POST /api/me/hosts/<rid>/stop` | none | 202 queued (the admin stop's MsgCode 7); 404 unless the server is the caller's; 409; 429 |

- **Who.** `require_user`: a live website session of an active account (401 otherwise,
  and a session whose account went inactive is deleted as it is used), with the CSRF
  header (403 `csrf_header_required`). The listing must be the caller's now
  (`hostdb::listing_owned_by`): the RID is listed and one of the caller's keys stands
  claimed for it, the live claim `active_hosts.host_user_id` follows, read at the
  request rather than as of the row's last write (so a reset, a ban or a delete, each
  taking the account's keys, closes the routes at once). A RID no listing holds and
  another account's listing get the same 404 body (`unknown_rid`, "no server of yours
  is listed under this RID"), so the routes never tell one from the other. Ownership is
  checked again at push time: the push carries the caller's account to the queue,
  which resolves the RID only to a session whose row names that account as its
  listing's owner (`lobby_sessions.host_user_id`, the lobby's `LobbyState::host_user_id`),
  and the receive thread drops the statement if the connection's owner changed before
  it went out (a re-host whose key names another account or none) or the account's
  claim is gone (`hostdb::listing_owned_by`). That miss at the queue is the same 404. An
  admin password reset, an inactive status or a delete takes the account's keys, so a
  push it queued just before is dropped at the send, not sent, and also queues a
  `forget_owner` that takes the account off its listings' owner in the lobby's memory
  and rows (beside dropping its retail login pins).
- **The rate limit.** Each owner push, command or stop, takes one token from the
  account's bucket (`HostRoutes::kOwnerPushPerUser`, in Redis): 8 at once, then one
  every 2 s. Past it: 429 `rate_limited` with `Retry-After` in seconds. The token is
  taken after the session and CSRF checks and before the body is read, so a refused
  body or another account's RID spends it too: a player can flood neither their
  server's chat nor the service.
- **The verbs.** Every `ServerCommand` verb acts only on the server it is sent to, so
  an owner may send all of them but `SetMPReset`, which stays an admin's: it stores the
  host's `mpreset` word in its `game.cfg`, and a nonzero word ends the host process at
  its next session create (D-NET-385), a change saved past the run that the owner's page
  could neither show nor undo once the server left the list. The stop is the owner's
  way to end the hosting.
- **What reaches everyone.** The stop, and `Cycle`, `EndMission` and `GameOver` (each
  ends the running round with the winner its arg names, a team name or number, none
  for no winner, and the server moves on through its rotation), act on every player on
  the server, not only the owner. The page confirms each one first, the stop with an
  explicit "ends the match for everyone" acknowledgement. `PuntPlayer` aimed at a listen
  host's own slot (slot 0) ends its hosting too.
- **The log.** Every owner push past the session check is logged once with the
  account's username (through `loggable()`) and id, the RID, the composed line and the
  outcome: `queued`, `not_hosting`, `unknown_rid` (the queue's push-time miss),
  `no_listing_of_theirs` (the route's own look-up), `invalid_command`,
  `verb_not_allowed` or `rate_limited`.

## Listed servers and owners

Every listed server can name the account that hosted it (`active_hosts.host_user_id`),
through the HOSTKEY a stock client fetches from `/NWHost.dll` and copies into its `Host`
var list's `HostKey`. Service policy throughout: how the retail service checked a
HostKey is unwitnessed.

- **The key.** `/NWHost.dll`'s first call mints the 48-letter A..P key (24 OS CSPRNG
  bytes, a nibble each) and, when the request's own retail login names an account,
  records the key for it in `host_keys`, as its
  SHA-256 only, the way `web_sessions` keeps its tokens. The account is the one the
  request's `LOGINSESSIONTAG` cookie names while that retail login's
  `active_user_sessions` row lives (a stock client sends its whole cookie jar on every
  GET), else the one its `PERSISTENTEXPRESSLOGINDATA` pin names (the pin a login made);
  both count only at the credentials generation that login read ("The credentials
  generation"), the same identity `/NWJoin.dll` and `/NWAccount.dll` take, and the
  account must be active. The
  `NWHANDLE` cookie never counts here: the service sets it in the clear to the
  account's handle, so any client can send any account's. With no such login nothing is
  recorded. Either way the relay page, the HOSTKEY page and their cookies are the same
  bytes a stock client parses. An account keeps its newest 20 keys (and any a listed
  server holds).
- **The claim.** Where the connection presents a `HostKey`, the service looks it up by
  hash and claims it for the connection's RID (`hostdb::claim_host_key`); the row then
  carries the account, and the connection's later updates and roster statements keep
  it. A stock host's fresh `ClientHostRequest` carries no `HostKey` (its `Host` list is
  rebuilt without one), so such a host is listed with no owner until the first
  `ClientHostUpdate` that carries the key (the full `Host` list a stock host sends at
  its mission's start, its host result publishing nothing, D-NET-465; the game and
  `opennova-serve` do the same). A key backs one listed server at a time: another connection
  presenting it while that listing lives links to no account. A dropped connection
  parks its lobby state and RID (`kParkedLobbyTtlMs`, 240 s), and its re-host after the
  NWU reconnect (`CurrentlyHosting=1`) re-sends its kept `Host` list, key included, so
  it claims the same key under the same RID and keeps the owner; a key whose listing
  is gone can be claimed again. A boot releases every claim with the previous run's
  rows.
- **Expiry.** A key lasts 24 h from its last use, its claim or the periodic
  `host_keys.prune` job while a listed server holds it (counted at most once an hour);
  the job deletes the rest.
- **Revocation.** A row stores its owner only while one of the owner's keys stands
  claimed for the row's RID; every write (a `ClientHostUpdate`, a roster statement, the
  gate's status blob) checks it, else stores no owner. What that does to a live listing:
  - An admin password reset or a status other than `active` deletes the account's keys
    (with its retail login's `active_user_sessions` row). The listing stays listed; its
    next write stores no owner, its next re-host after an NWU reconnect links to no one
    (refused under strict mode), and a banned account's key claims nothing.
  - A deleted account takes its keys (`ON DELETE CASCADE`), and its listings' owner and
    its rows in servers' player lists are set NULL (`ON DELETE SET NULL`). The listing
    stays listed with no owner, and its later writes store none. `players.id` is
    `AUTOINCREMENT`, so no later account gets that id.
  - In each case `POST /api/admin/hosts/<rid>/stop` ends the listing at once, and the
    account's own `GET /api/me/hosts` and push routes drop it at once: they read the
    live claim (`hostdb::listing_owned_by`), not the row, and so does the receive thread
    as it sends an owner push, so one the account queued just before is dropped rather
    than sent. A reset, a status change or a delete made through `PUT` / `DELETE
    /api/admin/users/<id>` (or a reset by link) also queues a `forget_owner` for the
    lobby, which takes the account off its listings' owner in its memory and rows.
- **Strict mode.** `ONNET_REQUIRE_HOST_KEY=1` lists no server whose `HostKey` claims no
  account (empty, unknown, expired, revoked, or held by another listing). The code is
  200, the retail table's `NWUSERVERMSGCODE_NEEDTOLOGIN`.
  - A `ClientHostRequest` carrying such a key (a re-host after the NWU reconnect) is
    refused with `ServerHostResult` `Success=0`, `MsgCode=200`. A stock client stores
    the code and shows every code but its six (1001, 1004, 1005, 1008, 1009, 1011) as
    NWEC58: "NovaWorld has denied your request to host for some unspecified reason.
    Please check your host options and try again." A refused re-host ends that host's
    match, as any failed re-host does.
  - A request carrying no key (a stock host's fresh one) is answered as usual, since
    the key has not come yet, but held out of the browser (no `active_hosts` row, no
    roster; a connection already listed has its row withdrawn) until an update
    presents a key. A key that claims an account lists it with
    the owner; one that claims none ends the hosting with `ServerStopHosting`
    `MsgCode=200` (a stock host latches `NWUSERVERMSGCODE_NEEDTOLOGIN`; in a match it
    ends the match at the next exit check and returns to the menu).

  Off (the default), such a host is listed with no owner, so `opennova-serve` without
  `--credentials` and a game host on no account keep listing.

| Route | Access | Replies |
| --- | --- | --- |
| `GET /api/me/hosts` | a website session (`require_user`) | `{"hosts": [...]}`, the caller's listed servers, oldest first: `rid`, `gsid`, `game`, `server_name`, `mission_name`, `game_type`, `players`, `max_players`, `host_ip`, `host_port` (the endpoint joiners are sent to), `peer_ip` (the address its session comes from), `created_at`, `updated_at` (UTC), `msg` (the server's message) and `roster`, the host's PlayerList as of its last update (`slot`, `player_name`, `team`, `type`; the slot is what a player verb's `ByIndex` target names); 401 with no live session; `Cache-Control: no-store` |
| `GET /api/admin/hosts` | admin (the Bearer token or an admin session) | `{"count", "hosts": [...]}`, every listed server with the same fields plus `owner`, `{id, username}` or `null` |
| `GET /api/hosts` | admin | `{"count", "hosts": [...]}`, every listed server as the lobby stores it: its browser columns, `roster`, and `player_list`, the joiners `/NWJoin.dll` recorded (`nwhandle`, `user_id`, `peer_ip`) |

The public `/api/lobbies` and the GSB name no owner, and no joiner's account or
address; `GET /api/hosts`, which names both, is an admin route. The owner pushes to
its own servers through `POST /api/me/hosts/<rid>/command` and `/stop` ("Pushing
statements to a listed server" above).

## Hosted servers

An account with a hosting entitlement can have the service run as many `opennova-serve`
servers for it as the entitlement allows, each in a slot of one of the fleet's machines
(a shared machine, or an own machine of its own), until it stops them (ADR 0053,
proposed; `hosted/`; the `hosted_servers`, `hosted_server_events` and `fleet_machines`
tables). Each server runs the data set its owner picks (and an expansion of it), pinned to the
data it started on (ADR 0064). Everything here is service policy.

- **The hosting entitlement** (`billing/entitlement.*`): how many hosted servers an account
  may have live, the larger of its support tier's servers (a monthly Stripe subscription:
  tier 1, 2 or 3 grants 1, 2 or 3 servers; `billing/tiers.h`) and an admin's comp grant
  (`players.comp_servers`, 0..16, `PUT /api/admin/users/<id>/comp-servers`). Only an active
  player's account has one. It is read from the billing tables (`backend/migrations/
  0002_billing.sql`, the cache of Stripe the billing sync keeps), whether or not billing
  is configured, so a deploy that breaks billing never stops a payer's servers. A
  subscription grants while Stripe calls it `active`, `trialing` or `past_due`, and only
  until 48 h past its period's end (a sync that stopped for good fails closed); `past_due`
  (Stripe still retries the card) keeps the account's servers running but starts no new
  one. Every other status grants nothing. The own-machine add-on (each own machine a
  subscription of its own, quantity 1, bought through Checkout and canceled in the
  Portal; the granting ones' quantities summed are the machines it pays for) lets that
  many of the account's servers run
  on a machine of their own (`hosted_servers.own_machine`), and counts only while a tier
  also grants (a comp grant buys no machine).

- **Data sets** (`hosted/data_set_repository.*`, `hosted/data_set_files.*`,
  `backend/migrations/0011_data_sets.sql`; ADR 0064). A data set is a row an admin makes:
  one product, a **base** game (its `title`, the `gameprofile` code `opennova-serve` speaks
  with `/game`, `jo` or `dfx2`, and its `game`, the `games` row whose lobby it lists in) or
  an **expansion** of a base (its `expansion_code`, `/exp jox01`). The migration seeds the
  four the deployment ran: Joint Operations (`jo`), Escalation under it (`jox01`), Delta
  Force Xtreme 2 (`dfx2`) and OpenNova's own data (`jo`). Its data lives in the object
  store (`ONNET_OBJECT_STORE_URL`, below) as **trees**, each one immutable version of the
  data set's files (a base: `resource.pff`, `localres.pff`, `language.pff`, with
  `version.txt`, `score.ini`, `_NSTMOUT.TXT` and `gt.ssc` when present; an expansion:
  `<code>.pff`, `<code>L.pff`, `version.txt`, `gt.ssc`; `hosted/data_set_files.h`) at
  `trees/<id>/`, uploaded by an admin or built by applying a published update release,
  with the multiplayer missions its archives list. A base is **offered** (a start may
  pick it) while it has a ready current tree, with each of its expansions that has one
  (`offered_data_sets`); the host-screen limits are the title's (`hosted/host_limits.h`:
  Joint Operations' 2..64 players, a time limit of 1..300, kill and score limits of
  1..500, respawn and delay of 0..300; Delta Force Xtreme 2's the same but 2..100
  players, its host screen's cap). Hosting is on only with a store. A start names a data
  set and an expansion by id (`hosted_servers.data_set_id`, `expansion_id`); at its
  placement the server is **pinned** to the data sets' current trees (`base_tree_id`,
  `expansion_tree_id`), which a crash restart or a move keeps, until its owner asks for
  the update (`update_requested`: placed again on the current trees, no restart counted).
  One whose data set has no data at its placement (no ready current tree, and no pins to
  keep) fails there, as does one whose row is gone (no restart mends either).

- **The fleet** (`hosted/placement.*`, `hosted/fleet_repository.*`,
  `backend/migrations/0003_fleet.sql`). Hosted servers run on **machines**: a **shared
  machine** packs up to `ONNET_FLEET_SHARED_SLOTS` (12) servers of any account, an **own
  machine** runs exactly one, the own-machine add-on's, launched for it and ended when it
  stops. A **slot** is one server's position on a machine (`hosted/slots.h`): slot k is
  the game's UDP port 32768 + k (open to everyone), the remote admin's TCP port 32800 + k
  (open to the service alone), the container's user 10001 + k and the working directory
  `/srv/slots/slot-k`; an own machine has slot 0 alone. `fleet_machines` holds one row per
  machine (its driver, its kind, its state, its cloud id, its type, its slots, its private
  and public addresses, the serve image it was launched with, the server an own machine
  is reserved for, when its agent last reported, why it ended), each move a
  compare-and-set: `launching` → `booting` (the cloud runs it) → `ready` (its agent
  reports) → `draining` → `ending` → `ended`, or `failed`, and `lost` once its agent has
  been silent 60 s. The live servers' slots are `hosted_servers.machine_ref`, `slot`,
  `game_port` and `generation` (its starts; its container is `srv-<id>-g<generation>`),
  two live servers never holding one slot (a unique index), and an ended server's row
  keeping where it last ran. An ended or failed machine's row is deleted 14 days after
  its end by the hourly `stats.prune` job, with its slot reports and egress months; an
  ended server that last ran on it keeps no machine (`machine_ref` null), and a machine a
  live server still names stays.
- **Placement** (`hosted/placement.h`, pure functions): a shared server goes to the
  fullest shared machine that takes servers (launching, booting or ready; never one
  draining, ending or gone) with a free slot, its lowest free slot, so machines fill one
  at a time and an emptied one can end; with none, a shared machine is launched while
  the fleet has fewer than `ONNET_FLEET_MAX_SHARED` (2) taking servers, and at that
  maximum the server waits in `provisioning` for a slot to free. A server on the add-on
  goes to the machine reserved for it, else to a new own machine (at most
  `ONNET_FLEET_MAX_OWN`, 2). A launched machine takes the server whose placement launched
  it at once, as it boots. The start request runs the same planner: placed after every
  live server still waiting for a slot (`fleet_has_room`), a start the fleet could not
  place is `at_capacity`.
- **The rows.** `hosted_servers` holds one row per start: its owner, its service account,
  its state, its data set and expansion (`data_set_id`, `expansion_id`) and the trees it is
  pinned to (`base_tree_id`, `expansion_tree_id`, `update_requested`), the owner's settings
  (range-checked columns, never a file), its place in the fleet (above), its container's
  name and the machine's private
  address (`provider`, `machine_id`, `machine_address`), the serve image its start pinned
  (`serve_image`: a deploy's new image reaches a server at its next start), the
  remote-admin login rendered for the service, the restarts used and why it failed.
  `hosted_server_events` keeps every transition with what caused it. An owner has as many
  live servers (any state but `stopped` and `failed`; a stopping one counts until it has
  stopped) as its hosting entitlement allows, as many of those on own machines as its
  add-on pays for, and at most `ONNET_HOSTED_CAP` servers are live at once (a backstop
  beside the fleet's room); a request checks the entitlement (some servers, none past
  due), the account's live servers against it, its own-machine servers against the
  add-on, the cap and the fleet's room in the transaction that writes it
  (`hosted::request_server`), which holds the hosted-start lock (an advisory lock every
  start takes), so no two starts both pass a count. When a server is about to be placed (`provisioning`, before its credentials are
  minted) its place is read again: its owner's live servers that are not stopping keep
  their places oldest first, and one past the entitlement is stopped instead (`its
  owner's support tier allows N hosted servers`).
- **The owners' limits.** Every supervisor pass opens by stopping what an owner's hosting
  entitlement no longer covers (`Supervisor::enforce_limits`; an account deleted or not
  active is the stop's own case, below): its newest live servers past the entitlement's
  servers (a tier lowered at its period's end, a comp grant cut, a tier canceled or
  unpaid), then its newest own-machine servers past the add-on's quantity, a stopping
  server counting toward neither, each stop's event naming why (`its owner's support
  tier allows 1 hosted server`, `its owner's own-machine add-on covers 1 machine`). A
  past-due tier keeps its servers (no new one starts). The brake on the brake: a pass
  that would stop the servers of more owners than max(2, half the owners with live
  servers) reads as the billing cache gone wrong, not as that many owners lapsing at
  once, so it stops none, logs `[hosted] ERROR the owners' limits held: ...` and holds
  (no limit stop, and no start refused for its place; the heartbeat's `limits_held` is 1).
  It releases itself once a full billing reconcile has completed after it took its
  owners and a pass then finds the same owners (or some of them) still past their
  entitlements: the cache is confirmed against Stripe, and their stops apply. Such a
  reconcile has the prices checking out and every price our subscriptions sell
  configured: a sold price missing from the `STRIPE_PRICE_*` lists grants its
  subscribers nothing, the misconfiguration the hold is for, so while one is left
  (`unknown_prices` in the admin view) no reconcile releases it. New owners past theirs
  meanwhile widen the hold, which then waits for the next reconcile. An admin's override (`POST /api/admin/billing/reconcile` with
  `{"force_limits": true}`) makes the next pass apply the stops past the brake, once.
  `GET /api/admin/billing` shows it (`limits_hold`, `limits_hold_reason`,
  `limits_hold_owners`, `limits_hold_servers`), billing on or off. The hold is one row
  (`hosted_limits_hold`, `hosted/limits_hold.*`), so the supervisor, the billing
  sync's clean reconciles and the admin's routes share it wherever they run.
- **The supervisor** (`hosted/supervisor.*`) is a thread of its own, started once the
  controller leads (the internal listener serves by then); each
  pass (every second) asks that the controller still leads, leases a
  pooled connection, steps every live machine (below), then walks every live server:
  `requested` → `provisioning` (placed; on its slot the trees are pinned, the service
  account gets a new password, a new admin password is minted, and the files are
  rendered for the slot's ports) → `booting` (its container wanted on its slot until its
  machine's agent, the trees fetched, reports
  it running) → `listing` (until a listed RID holds one of the service account's
  HostKeys) → `running` → `stopping` → `stopped`, or `failed`. Every move past a request
  or a stop is the supervisor's, a compare-and-set on the row's state, so a stop that
  lands while a pass places a server wins (it lets the slot go). A pass reads and writes
  rows alone and never calls the cloud: a launch is the machine's row and its
  `machine.launch` job written in one transaction, a check of a booting, lost or
  unaddressed machine enqueues a deduped `machine.describe` (at most every 10 s) whose
  answer lands on the machine's row (`cloud_state`, `described_at`, its addresses), an
  end enqueues `machine.terminate`, and the workers run them ("Background jobs" above),
  so a surge of starts launches its machines side by side. A machine the cloud has not
  answered for yet takes its servers at the pass after its launch, and within a pass the
  slots its waiting servers will take are held, so twenty servers needing twenty
  machines launch them all at once. What a pass remembers of a machine (since when it is
  in its state, idle, the server its drain moved last, a lost one that was draining) and
  each kind's launch backoff are rows too (`fleet_machines`, `fleet_launch_backoff`), on
  the supervisor's clock (Unix milliseconds), so a supervisor started again resumes its
  timers.
- **A stop** is the supervisor's own act: the row goes to `stopping` (asked by the owner
  or an admin, or by the supervisor when the owner's account is deleted or no longer
  active, read every pass and again right before a start mints credentials, or for the
  owners' limits above), the supervisor pushes `ServerStopHosting` to the listing (the
  sysop punt's MsgCode 7, scoped to the service account; a command for the lobby,
  inserted on the pass's connection), and stops wanting the
  container once the listing has left the browser, or 60 s after the push (its agent then
  stops it: `opennova-serve` deregisters on SIGTERM). Its container exiting while the row
  is `stopping`, whatever its code, finishes the stop and is never restarted. A stop is
  never read from an exit code: `opennova-serve` exits 0 for its rotation's end or the
  admin's `GOTO MENUSTATE`, 3 when the service ends its hosting and 1 for a SIGTERM while
  it waits to be listed.
- **A crash** is what the server itself causes: its container exiting while the server
  should run (any code), the kernel ending it out of memory, a start that failed (docker's
  125, an image that did not pull among them), its container not running within 5 min of
  its machine's ready, no listing within 3 min (on the ec2 driver: 10 min, for an image
  pull, and 5 min; the clock waits while its machine is lost). The
  server lets its slot go and goes back to `provisioning`, 15 s later, while it has
  restarts left: 3 of them, and an hour of continuous `running` resets the count (so an
  always-on server that crashes now and then over weeks is never failed for it); then
  `failed`. Its next start is a new generation: a new container name with new
  credentials, on whichever slot placement picks. **What the machine causes is no
  crash** and costs no restart. A machine launched for it that the cloud refused: the
  server waits for the next launch (the driver's own error text goes to the server log;
  the event the owner reads says `the launch of its machine failed`). A machine it was
  placed on that never booted, or its machine lost, gone, ended by an admin or drained
  (an admin's drain, a fleet roll, an agent on another protocol): the server moves off at
  once, placed again (`its machine is gone; placed again (no restart counted)`). The brake
  on a fleet that cannot launch is the launch backoff below and the heartbeat's
  `launch_failures`, not the servers' restarts.
- **No lobby live.** While no lobby instance takes clients (`lobbydb::live_lobbies`, the
  gates' own read: a lobby outage, or a deployment's first release, whose controller
  rolls before any lobby), a gate hands a starting `opennova-serve` no lobby, and it
  gives up past its 30 s probe and exits (retail's). So the supervisor starts nothing then:
  a server due to be placed waits in `provisioning`, no machine launched for it
  (`[hosted] server <id> waits for a live lobby`; the pass logs `[hosted] WARN no lobby
  instance is live` once), an exit is NovaWorld's cause and places the server again with
  no restart counted (`... while no lobby instance was live; placed again (no restart
  counted)`), and the listing's clocks wait (a listing server's timeout above, a running
  one's 8 min below: its lister reconnects to the lobby address and lists again once a
  lobby is back there). Once a lobby is live the waiting servers start, and an exit
  counts again.
- **The machines' lifecycle.** A machine whose launch job has not finished within the
  driver's boot timeout (15 min on EC2, 2 min for the local stack's containers) fails, as does a booting machine
  whose agent never reports within it (terminated); one whose describe finds it gone
  before its agent reported fails at once. A ready
  (or draining) machine whose agent has been silent 60 s is `lost`: the cloud is asked
  (a describe since it went lost; one that did not answer decides nothing); gone, it
  ends at once; still running, it is terminated 3 min later, and an agent that
  reports again first makes it ready (or draining) again. While its machine is lost a
  running server's health checks wait. A shared machine with no server on it ends after
  `ONNET_FLEET_IDLE_MINUTES` (30); an own machine ends once its server has ended. A
  draining machine moves its servers off one at a time, the next once the last moved one
  runs again elsewhere (or ended), and ends once it holds none; nothing new lands on it,
  and it counts toward neither maximum, so a roll launches the machines it moves servers
  to. A machine row moved to `ending` (an admin's end) is terminated at the next pass. A
  launch the cloud refuses (its job fails the row; the next pass counts it), or a machine
  that never booted, counts toward the heartbeat's `launch_failures` and holds every
  launch of that kind for a while that doubles with each failure in a row: 1, 2, 4, 8 and
  16 minutes, then 30 (`fleet_launch_backoff.failures`; a failure that lands while the
  backoff holds came from a launch made before it began and counts once with it). A
  machine of the kind that boots (its agent's first sync) ends the backoff and sets the
  count back to 0.
- **The desired state** (`hosted/fleet_desired_state.h`, the supervisor's answer to each
  agent's sync): the containers of the live servers placed on the machine that are
  booting, listing, running or stopping (a stopping one only once its agent has reported
  it, so a server stopped before its start never starts), each with its slot, its user,
  its pinned image, the trees it is pinned to (its base tree, bound at the container's
  `/opt/game`, and its expansion tree, at `/opt/game/expansion/<code>`),
  `opennova-serve`'s arguments, the slots' limits (`ONNET_FLEET_SLOT_CPUS`,
  `ONNET_FLEET_SLOT_MEMORY_MB`, 128 processes) and its working directory's files until
  the agent has reported it once; and, beside the containers, every tree they use, with
  the store URL the agent fetches it from (`trees/<id>/`) and its manifest (each file's
  name and size), which the agent checks the fetch against. The files are kept in the
  service's memory alone and dropped once reported: a server whose start the service's
  restart cut short (booting, never reported) is placed again with new credentials, and a
  running server's container is answered for without files. The agent routes force each
  container's user to its slot's and send `drain` while the machine's row is draining.
- **The agents and the internal listener** (`hosted/agent_route.*`,
  `hosted/internal_listener.*`, `hosted/agent_protocol.*`; `apps/hostd/README.md`). Each
  machine runs `opennova-hostd`, the machine agent: its user-data carries a one-time
  bootstrap token (kept by the service as its SHA-256 alone, 30 minutes' life, spent at
  the agent's first sync, so a redeem whose reply was lost is made again for a fresh
  secret), which it redeems at `POST /hosted/machines/<id>/bootstrap` for its long-lived
  secret (stored hashed), the serve image and its slots; then every 5 s (1 s with work)
  it posts `POST /hosted/machines/<id>/sync` with every container's state, CPU, memory
  and its slot's egress (and, for one that never started, why: its data could not be
  fetched), and runs what the reply wants, fetching the trees it lacks first. Both routes are on the
  **internal listener**, the controller's second HTTP server, bound while it leads to
  the controller task's private address at port 8070 (from its ECS task's metadata, with
  `ONNET_INTERNAL_HTTP_BIND` unset; that setting where it is set, `127.0.0.1:8070` by
  default), which the machines' security group alone reaches, never on the public port;
  the agents reach it at `ONNET_INTERNAL_URL`, the deployment's Cloud Map name for the
  controller (`controller.<deployment>.opennova.internal:8070`, which ECS registers for
  each running controller task), resolved at each request, so they follow a replaced
  controller (an agent tries again across a refused connection, a standby's). The routes answer only a TCP
  peer inside `ONNET_HOSTED_BOOTSTRAP_CIDRS` (and loopback on a loopback bind), and only
  from the machine's own recorded private address. Every refusal is one bare 404; a
  refusal inside the networks is logged without the token or the secret at most once per
  peer per minute, and Crow's per-request lines for these paths are left out of the log.
  The controller starts it before the supervisor and stops it after the supervisor's thread.
- **Stats** (`hosted/stats_sampler.*`, `hosted/stats_repository.*`,
  `backend/migrations/0004_server_stats.sql`). The first supervisor pass of each minute
  (on its wall clock) samples every live server into `hosted_server_samples`: its state,
  its listing's players, slots and mission, its admin link's scene and whether the link
  is up, its container's CPU (a share of the slot's CPUs, 0..100) and memory as its
  machine's agent last reported them, and the bytes its slot sent that minute (the
  difference of the agent's per-slot counter, which counts since the machine booted; a
  count below the last one is a counter that started over, counted from 0; a server's
  first sample on a slot counts none). Each sample rewrites its server's hour in
  `hosted_server_hourly` (live and running minutes, player-minutes, peak players and
  slots, unique players, missions, bytes, mean CPU) and adds the bytes to its machine's
  month in `fleet_machine_egress`. Counts alone: unique players are counted from the
  listing's roster names held in memory for the hour, never stored (a service restart
  loses that hour's names), and a mission is counted when a game-scene sample sees one
  other than the last such sample's; both are approximations, and the replies say so.
  Samples are kept 48 hours, hours 90 days (the hourly `stats.prune` job deletes the rest,
  never the supervisor's pass); the routes read the samples for 1h and 24h
  and the hours for 7d and 30d. The admin's usage view (`GET /api/admin/hosted/usage`)
  reads every live server's newest sample and its last hour of them, nothing sampled
  anew; the site's **Server usage** page (`/admin/usage`) shows it, read again every
  30 s: the fleet's totals as tiles, each live machine's share, and each live server by
  machine and slot, memory in binary units (MiB, GiB), the rate sent to players in bits a
  second (Mb/s) and the bytes sent in decimal units (MB, GB). The agents report what
  each slot sends, not what it receives, so the view has no inbound figure.
- **The admin link** (`hosted/admin_link.*`; ADR 0053 d5). A running server opens
  retail's remote admin on its `game.cfg`'s `remote_admin_port`, its slot's admin port,
  its `admin.cfg` granting the user `svc` (a 31-character random password, new at every
  start, rights `7F`) to the controller alone (`ip_restrict`: the controller's network,
  its private address's /16, since a replaced controller comes back on a new address
  while a running server keeps the `admin.cfg` its start rendered; on a cloud machine
  its security group too). The service is that console's only client: owners never get
  the port or the login, and reach the console only through the site's routes. The
  supervisor keeps one `AdminLink` per server in `running`, at its machine's private
  address (every pass syncs them: a link comes up once the server runs, is replaced when
  a restart rendered a new login, and goes down as the server leaves `running`, at
  `stopping` or a crash) and publishes each link's state to its row
  (`hosted_admin_links`: its state, the scene, its last poll, since when it is up, its
  error; written when it changes, every row's `seen_at` moved every 5 s, the rows of
  servers with no link deleted), which the web routes read: a row 15 s stale (no
  controller) reads as no link. The links are the controller's alone. Each link is a thread of its own (at most `ONNET_HOSTED_CAP` of
  them; the apps' sockets are blocking with timeouts, and a thread per link keeps one
  dead machine from holding up another server's commands) that keeps one logged-in
  session open, speaking the protocol as `RAT.exe` does through the engine's client codec
  (`engine/net/admin/admin_protocol.h`): the challenge, the login encrypted under it, one
  command packet at a time. The server marks no reply's end, so each command's replies
  are read by the dispatcher's per-verb count (`admin_reply_plan`, witnessed in
  `docs/net/novaworld-net-re.md` §6.9): an exact count for most verbs, the first reply
  deciding for `PLAYER BAN <#>` (a second after `OK - Banned Player`) and `GOTO
  MENUSTATE`, and a 200 ms quiet gap only where the line cannot tell (`PLAYER BAN ALL`, a
  `PLAYER` sub-verb the server does not know). Bytes between commands are read off first
  (to the console only after a recorded command), and the idle socket is looked at every
  250 ms, so a server that went away is noticed and its connection reset at once. The
  link polls `GET GAMESTATE` every 15 s, reconnects with a backoff (1 s doubling to 4 s:
  a link lives only while its server runs, and the health check counts it), runs the
  queued commands (16 at most; a caller waits its own budget, and a command still queued
  when it gives up is dropped; `QUIT` is refused, the link's own), and ends with `QUIT`
  on a logged-in session (a reset otherwise). The password is a log secret for the
  link's lifetime (`add_log_secret`, masked by `main()`'s log sink too) and never reaches
  a log line, a console line or a reply.
- **The admin listener on a machine.** The apps' TCP listeners set `SO_REUSEADDR` on POSIX
  (`apps/common/net_sockets.cpp`), so an `opennova-serve` restarted on a slot's admin port
  listens at once rather than after the closed session's TIME_WAIT. A listen that still
  fails leaves the server running without its console, as retail ignores its listen's
  result (`opennova-serve` keeps that); the supervisor's health check, which counts the
  admin link, then fails the server.
- **The console** is the table `hosted_server_console`,
  a capped ring of each server's newest 500 lines: the commands the routes ask a link to
  record (with their sender, `owner`, `admin` or `service`), each reply packet, and the
  link's own status lines (the session up, lost or refused, closed, and the server's scene
  when it changes), in UTF-8 with UTC times. The reads the site polls (the player list,
  the rotation, the chat window) are not recorded.
- **Health.** While `running`, every pass reads its agent's report and every 20 s checks
  the listing and the admin link (its session up: its last status poll or command
  answered). Three checks in a row with no answer on the admin link fail the server. A
  listing no check has found for 8 min (`listing_lost_ms`) places the server again, no
  restart counted: a lobby's replacement takes its listings, and a host's NovaWorld
  session (retail's) drops the dead lobby only after 240 s of silence, then reconnects to
  that lobby's address alone. Every lobby of a deployment shares one address (its load
  balancer's; the local stack's fixed endpoint), so a lobby's replacement lists the
  server again within about 4.5 min, no restart; the 8 min are the safety net for a
  server whose lobby address is gone, and a new start goes through the gate. While its
  machine is lost, or no lobby is live (above), the listing's clock waits. Always on:
  nothing reaps an idle server or ends one for its age.
- **Reconciliation** at the supervisor's start: a machine row still `launching` whose
  launch job is gone fails (one whose job waits or runs resumes: the job outlives the
  service); a `fleet.sweep` job runs (every cloud machine the driver lists, `list_tagged`,
  that no live row names is terminated, but one whose row still launches, its launch's
  answer not yet written; a live row the listing lacks is described, and
  one whose cloud machine is gone ends at the next pass, its servers placed again with no
  restart counted, a `stopping` one stopped); a server whose files went with the service
  (`booting`, its container never reported) is placed again; the rest resume where they
  were, their containers answered for without files. A running one gets 8 min on
  either driver (`relist_grace_ms`) to list again before its health checks count: the
  boot cleared the previous run's listings, and its `opennova-serve` drops the
  NovaWorld session the restart left dead only after 240 s of silence (the service's
  advertised timeout, retail's), then reconnects and lists again; the grace is twice
  that. A driver that cannot list ends nothing (the sweep's job fails). Every 5 min the
  sweep runs again.
- **Service accounts.** Each hosted server lists itself with an account of its own,
  `srv-<id>` (`players.kind = 'service'`), minted at its first start with a 32-character
  password from the OS CSPRNG (bcrypt, as every account), whose password is set again at
  every start (the credentials generation moves, its retail login and its keys go), and
  deleted in the transaction that ends the server (stopped or failed). Its lister logs
  in through the
  unchanged `/NWLogin.dll` and `/NWHost.dll`, so the listing's HostKey is the service
  account's. The website's login answers a service account the wrong password's 401, no
  website session is ever minted or resolved for one, the admin users list hides them,
  `PUT` / `DELETE /api/admin/users/<id>` answer 404 for one, and no person may register,
  or rename to, a username or handle starting with `srv-` (400 `invalid_field`).
- **A hosted listing's owner.** One rule, in `hosted/hosted_repository.h`: a listing
  belongs to the account whose HostKey it holds now (`hostdb::listing_owned_by`), except
  that a live hosted server's service account stands for that hosted server's owner.
  `GET /api/me/hosts` lists the caller's own listings and its live hosted servers' (each
  with `hosted_server_id`, null on one of its own); the owner pushes (`POST
  /api/me/hosts/<rid>/command`) resolve the RID to the account holding the claim and push
  scoped to it, and refuse a hosted server that is stopping (409 `server_stopping`); `GET
  /api/admin/hosts` names the hosted server's owner (and its `hosted_server_id`). The stop
  of a live hosted server's listing (`POST /api/me/hosts/<rid>/stop` or
  `/api/admin/hosts/<rid>/stop`) is the hosted server's stop, 202 `{"rid",
  "hosted_server_id", "state", "status": "hosted_stop"}`: no bare `ServerStopHosting`
  goes out, since the supervisor would read the server's exit as a crash and restart it.
  Any other listing, an ended hosted server's lingering one included, takes the plain
  push.
- **Several servers on one address.** A stock retail client lists and joins every slot of
  a machine: NovaWorld lists a server at the source address and port of its datagrams
  (`engine/net/novaworld/service/lobby_session.cpp`), `/NWJoin.dll` hands a joiner that
  `host:port`, and the lobby keys its sessions by address and port. Each slot's `game.cfg`
  pins `mpnovaworldportmin` = `max` to the slot's game port, and the containers run on
  the machine's own network (`--network host`: a bridge's NAT would rewrite the port).
- **The working directory** is rendered from scratch at every start
  (`hosted/hosted_settings.*`), in retail's formats: the host file (`server.host`:
  `GameName`, `MaxPlayers`, `MPHostGamePassword`, `Time_Limit`, `KillLimit`, `MaxScore`,
  `Respawn`, `Delay`, `TeamFF`, `Replay 1` so the rotation never ends the session, and
  one `Mission` line per map, the owner's first map last since the last line is the
  starting map; cp1252, CR LF), `game.cfg` (the defaults through the format's own writer,
  the NovaWorld network type, `mpnovaworldportmin` = `max` = the slot's game port and
  `remote_admin_port` the slot's admin port), `admin.cfg` (user `svc`, a 31-character
  random password, rights `7F`, `ip_restrict` the controller's address or network as
  the machine sees it) and the `--credentials` file (`NOVAWORLD_USER` / `NOVAWORLD_PASS`). All but
  `game.cfg` are secret (the host file carries the game password): sent to the agent
  only until it has reported the container, written owner-only and never logged. The
  admin login lives in the row's own columns, read by `hosted::admin_login_of` alone:
  `HostedServer` has no field for it, so no reply can carry it. `opennova-serve` runs with
  `/HOST server.host --credentials credentials.txt --master-host <host>
  --master-gate-port <port> /game <title>` (the base data set's) and `/exp <code>` with
  an expansion. The settings' rules (`check_settings`): a data set the service offers, a
  name of 1..31 cp1252 bytes with no control character or double quote, a password of up
  to 16 printable ASCII characters with no space or quote, the whole numbers within the
  data set's title's limits, an expansion of that data set with data, and 1..128 `.bms`
  names of letters, digits, `_` and `-`, each a mission the base's tree or the
  expansion's lists (without case); a start naming none takes every mission of both.
- **Machine drivers** (`hosted/machine_driver.h`): the fleet's one seam to the cloud,
  called by the job workers alone (`hosted/fleet_jobs.*`). A driver launches the machine
  a row stands for (the row's id and its one-time token, nothing else), describes it,
  terminates it and lists the machines it runs for this deployment; what the supervisor
  knows of it without a call (its name, its boot timeout, a slot's ports) is its
  `MachineProfile`, the controller's. Every driver's machine runs the real agent,
  `opennova-hostd`, and has an address of its own, so each slot's ports are its fixed
  ones (`hosted/slots.h`), the controller's admin links reach them at the machine's
  address, and the agent's requests come from that address, which the agent routes
  check against the machine's row. `ONNET_HOSTED_PROVIDER=docker`
  (`hosted/docker_machine_driver.*`, the local stack's: `deploy/compose/docker-compose.hosting.yml`)
  launches each machine as a container of the serve image (`ONNET_HOSTED_SERVE_IMAGE`) on
  the stack's network (`ONNET_HOSTED_DOCKER_NETWORK`) over the Docker Engine API
  (`ONNET_HOSTED_DOCKER_URL`, `http://`; the stack puts a socket proxy there that admits
  the containers API alone): `<deployment>-machine-<id>` (`ONNET_HOSTED_DEPLOYMENT`),
  labelled `opennova.hosted`, `opennova.deployment`, `opennova.machine-id` and
  `opennova.machine-kind`, the mounts of a data container (`ONNET_HOSTED_DOCKER_DATA_FROM`: the local
  store's directory at the path the programs see it, from which its agent copies the trees
  its servers need to `/opt/game/<tree>`, ADR 0064) taken read-only, docker's init its PID
  1, never restarted. The launch creates it, puts its one-time token in it as a file (the archive
  API, so neither an argument nor a variable `docker inspect` shows carries it: the
  machine's user-data), starts it and reads its address on the network; a name the engine
  holds already is a launch run again (this row's labels: started where it is not yet,
  else read), and a machine with no address is removed. Its process is the image's own
  `opennova-hostd` with the process runtime (`apps/hostd/README.md`), its servers
  `opennova-serve` children of it. A describe reads the container (created: pending;
  running: running; gone otherwise; no answer: unknown), a terminate removes it (and only
  one carrying this deployment's labels), the listing filters on both labels. Its boot
  timeout is 2 min, and the servers' clocks the supervisor's defaults (3 min to list). The
  servers list through `ONNET_PUBLIC_HOST`, which is the developer's LAN address there: the
  machines reach the gate's and the lobby's published ports at it, as the internet's
  clients do. `ONNET_HOSTED_PROVIDER=ec2` (`hosted/ec2_machine_driver.*`) launches EC2
  machines from the launch template of their kind (`ONNET_HOSTED_EC2_LAUNCH_TEMPLATES`,
  `{"shared": "lt-...", "own": "lt-..."}`; `infra/aws`: the deployment's fleet image
  (Canonical's Ubuntu, or one `fleet bake-image` made with docker and the AWS CLI
  installed), the kind's type, the machines' security group, their instance profile
  (which reads the deployment's trees in the object store), IMDSv2 required), tagged
  `opennova:hosted`, `opennova:deployment`, `opennova:machine-kind`,
  `opennova:machine-id` and `Name`, with the machine script
  (`hosted/machine/machine_user_data.sh`, shipped in the worker's image, web's and the
  controller's) as user-data: it installs docker and the AWS CLI v2 (the release the
  service's images carry, its zip checked), makes the trees root `/opt/game` (where the
  agent fetches each tree the service names into its own directory, `aws s3 sync` from
  the store, checked against the manifest), reserves the slots' ports out of the
  ephemeral range,
  fences the slots' users off the instance metadata and off the controller's internal
  port on every destination (the controller is reached by a name whose address moves
  with it, so no firewall rule names an address), installs `opennova-hostd`, copied
  out of the serve image, pointed at `ONNET_INTERNAL_URL` with `--trees-root /opt/game
  --aws /usr/local/bin/aws`, and ships the agent's log and its containers' output to the
  machines' log group (`ONNET_HOSTED_EC2_LOG_GROUP`, a stream each: `<machine id>/hostd`,
  `<machine id>/slot-<k>`). The driver runs the AWS
  CLI v2 (installed in the worker's image, pinned and checksummed) through a `CommandRunner`
  (`apps/common/command_runner.*`: an argv array, no shell, a 20 s timeout, an
  environment of the system's and the `AWS_*` variables alone), its credentials the
  worker task's IAM role; it describes from one cached `describe-instances` every 10 s,
  the describes asking while it is read waiting for its answer (a failure reads unknown:
  nothing is decided on it; a machine the listing lacks reads
  pending within 3 minutes of its launch as its row dates it, `fleet_machines.launched_at`
  set when the launch answered, read by the database's clock, so every worker judges it
  alike, the launch's or another), and a machine is terminated at its end, never
  stopped. The servers list through `ONNET_PUBLIC_HOST`
  (`--master-host`), so the listing records each machine's own public address.

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_HOSTED_PROVIDER` | `none` | The machine driver: `ec2` (EC2 machines), `docker` (containers of the serve image, the local stack's; above); `none` runs no hosted server. Any other value, fleet settings or data sets that do not read, no data set named, or a driver missing or mistyping one of its settings (or with a loopback `ONNET_PUBLIC_HOST`) is `none` with a boot warning naming why. |
| `ONNET_HOSTED_CAP` | 24 | The most hosted servers live at once, across every account: a backstop beside the fleet's own room. |
| `ONNET_OBJECT_STORE_URL` | none | The object store the data sets' trees and the update packages live in (`object_store.h`, ADR 0064): `s3://<bucket>/data` (the deployment's own ops bucket, `data/<deployment>`; the AWS CLI through the program's role) or `file:///<dir>` (a directory: a developer's machine, the tests, the local driver's trees). Required for hosting on either driver; read by web (the admin's data set and release routes, the update listener), the worker (the data sets' jobs) and the controller (the trees' URLs its supervisor hands the agents) whether or not hosting is on. |
| `ONNET_OBJECT_STORE_REGION` | none | The S3 store's region (required with an `s3://` URL). |
| `ONNET_INTERNAL_HTTP_BIND` | `127.0.0.1:8070` | The controller's internal listener, `<IPv4>:<port>`: unset in a deployment, where the controller binds its ECS task's private address at 8070 (from the task metadata); a private or loopback address, never a public or wildcard one. The controller's admin links connect from it, and the ec2 machines' `admin.cfg` admits its /16 (`10.44.*`): with `ec2`, a controller whose bind is not a private address does not boot. |
| `ONNET_INTERNAL_URL` | none | The internal listener as the machines reach it, `http://<host>:<port>` (a deployment's Cloud Map name for the controller; the controller's compose name in the local stack); every machine's agent calls it (`--service`), and the ec2 machines fence their slots off its port. Required by both drivers: without it hosting is off. |
| `ONNET_HOSTED_BOOTSTRAP_CIDRS` | none | Comma-separated private IPv4 CIDRs, the networks the agent routes answer (the machines' subnet); each inside 10.0.0.0/8, 172.16.0.0/12 or 192.168.0.0/16. On a loopback bind, 127.0.0.0/8 is admitted besides. |
| `ONNET_FLEET_SHARED_SLOTS` | 12 | A new shared machine's slots (1..32). |
| `ONNET_FLEET_MAX_SHARED` | 2 | The most shared machines taking servers at once. |
| `ONNET_FLEET_MAX_OWN` | 2 | The most own machines taking servers at once. |
| `ONNET_FLEET_IDLE_MINUTES` | 30 | How long a shared machine with no server lives on before it ends. |
| `ONNET_FLEET_SLOT_MEMORY_MB` | 512 | Each container's memory (no swap). |
| `ONNET_FLEET_SLOT_CPUS` | 1 | Each container's CPUs (a decimal). |
| `ONNET_HOSTED_SERVE_IMAGE` | none | The serve image every start pins and the machines pull (`ghcr.io/<owner>/opennova-serve@sha256:...`, pinned at deploy); required for `ec2`; for `docker`, the image every machine container runs (its agent and its servers are the image's: `opennova-serve:local` in the local stack). |
| `ONNET_HOSTED_HOSTD_IMAGE` | the serve image | `ec2`: the image `opennova-hostd` is copied out of (pinned at deploy). |
| `ONNET_HOSTED_EC2_LAUNCH_TEMPLATES` | none | `ec2`: the launch templates by machine kind, `{"shared": "lt-...", "own": "lt-..."}` (each `lt-<id>` at its default version, or `lt-<id>:<version>`). |
| `ONNET_HOSTED_EC2_REGION` | none | `ec2`: the region every AWS CLI call names. |
| `ONNET_HOSTED_EC2_LOG_GROUP` | none | `ec2`: the CloudWatch log group the machines write (`/opennova/<deployment>/machines`): hostd's log and each slot's container output, through docker's `awslogs` driver (`apps/hostd/README.md`, "Logs"); required. |
| `ONNET_HOSTED_EC2_SUBNETS` | none | `ec2`: the subnets (`subnet-...`, comma-separated, one per zone) machines launch into, each giving its machines a public IPv4. A launch takes the one holding fewer of the deployment's live machines (ties to the first listed) and, when that zone cannot take it (`InsufficientInstanceCapacity`, `Unsupported`), the next. |
| `ONNET_HOSTED_DEPLOYMENT` | none | The `opennova:deployment` tag (`ec2`) or `opennova.deployment` label (`docker`) every machine carries and every describe, list and terminate is scoped to, so two stacks in one account (or on one Docker) never touch each other's machines (letters, digits, `.`, `_`, `-`; `docker`: also its containers' names, `<deployment>-machine-<id>`). |
| `ONNET_HOSTED_AWS_CLI` | `/usr/local/bin/aws` | The AWS CLI v2, an absolute path (run as given, no `PATH` search): the `ec2` driver's, and also the mailer's SES calls and the lobby's ECS discovery. |
| `ONNET_HOSTED_DOCKER_URL` | none | `docker`: the Docker Engine API, `http://<host>:<port>` (the local stack's socket proxy, `http://docker-api:2375`). |
| `ONNET_HOSTED_DOCKER_NETWORK` | none | `docker`: the network every machine joins, its address there the machine's (the local stack's, `opennova-dev`). |
| `ONNET_HOSTED_DOCKER_DATA_FROM` | none | `docker`: the container whose mounts (each data set at `/opt/game/<data set>`) every machine takes, read-only (the local stack's `opennova-dev-machine-data`). |
| `ONNET_HOSTED_MACHINE_SCRIPT` | `/app/hosted/machine_user_data.sh` (where the images ship it) | `ec2`: the machine script, the user-data template (`hosted/machine/machine_user_data.sh`); a run outside the image names it. |

The deploy toolbox sets every `ec2` setting from the Terraform outputs (DEPLOY.md,
"Hosted game servers"); nobody copies an id by hand.

The routes (`hosted/hosted_routes.*`, the admin's fleet routes `hosted/fleet_routes.*`, and the
comp grant with the admin user routes), behind
`ONNET_API_PROXY_ONLY` like every `/api` route. Every refusal is an error code with the
service's own message, never database text.

| Route | Access | Replies |
| --- | --- | --- |
| `GET /api/me/servers` | a website session | `{"enabled", "full", "entitlement", "data_sets", "servers"}`: `enabled` is a machine driver with at least one offered data set; `full` that a start now would be `at_capacity` (`ONNET_HOSTED_CAP` servers live, or the fleet with no place for a shared server and none for one on an own machine the account still has free); `entitlement` the account's hosting entitlement (`max_servers`, `used`: its live servers, a stopping one included, `own_machines`, `own_used`: those of them on own machines, `can_start`, `source`: `none`, `tier` or `comp`); `data_sets` the ones a start may pick (the bases with a ready current tree, by id; empty when hosting is off), each `id`, `display_name`, `title`, `version` (its current tree's), `missions` (its tree's multiplayer missions, each `file` and `title`: the site's rotation picker, and the rotation a start on it takes when it names none), `limits` (its title's: `max_players`, `time_limit`, `kill_limit`, `max_score`, `respawn`, `start_delay`, each `min` and `max`) and `expansions` (its expansions with data: `id`, `code`, `display_name`, `version`, `missions`); `servers` its live servers by id, oldest first, then its newest 10 ended ones, newest first, each `id`, `state`, `live`, `own_machine`, `settings` (`data_set_id`, `expansion_id` (null for none), `game_name`, `max_players`, `game_password`, `time_limit`, `kill_limit`, `max_score`, `respawn`, `start_delay`, `team_ff`, `missions`), `data_set` (`{id, display_name}`, null once the row is gone), `expansion` (`{id, code, display_name}` or null), `game_display_name` and `expansion_display_name` (the rows' names), `data` (`base` and `expansion`, each `{tree_id, version}` of the tree it is pinned to, or null before its first start), `update_available` (its data sets have newer data than its pins, while it is live), `update_requested`, `restarts`, `failure_reason`, `created_at`, `updated_at`, `running_at`, `ended_at` (UTC, null until set) and `listing` (`rid`, `server_name`, `mission_name`, `players`, `max_players`, `host_ip`, `host_port` while listed, else null); `Cache-Control: no-store` |
| `GET /api/me/servers/<id>` | a website session | `{"server"}`, one of the account's servers in any state, as the list's plus `events` (the newest 50 transitions, oldest first: `from`, `to`, `detail`, `at`); 404 `not_found` for an id that is not the account's (another account's server is a 404, never a 403); `Cache-Control: no-store` |
| `POST /api/me/servers` | a website session, the CSRF header | the settings as JSON (`game_name` required; `data_set_id` defaults to the first offered data set, `expansion_id` to none, and `missions` to every mission of the base's tree then the expansion's; every other field to its default) and `own_machine` (`true` to run it on an own machine; default `false`, a shared one). 503 `hosting_disabled` with no machine driver or no data set with data; 429 past the account's bucket (6, then one every 20 s, shared with the stop); 400 `invalid_json`, `invalid_field`, or `invalid_settings` with the `field` (`data_set_id`, `expansion_id`, `missions` among them) and why; 403 `not_entitled` with no hosting entitlement (or an account not active), 403 `payment_past_due` while a granting subscription's payment is past due; 409 `server_limit` with `max_servers` and `used` beside the code when every server the account may run is live, 409 `no_own_machine` when each own machine it pays for already runs one, 409 `at_capacity` (`ONNET_HOSTED_CAP` servers live, or the fleet's planner finds it no place: every slot taken, the servers already waiting counted, and the fleet at its maximum machines); 202 `{"server"}`, `requested` |
| `POST /api/me/servers/<id>/stop` | a website session, the CSRF header | 429 past the bucket; 404 `not_found` for an id that is not the account's; 409 `not_live` once it has ended; 202 `{"server"}`, `stopping` (also when it already was), or `stopped` when the supervisor finished it in between |
| `POST /api/me/servers/<id>/update` | a website session, the CSRF header | the owner's move of a live server to its data sets' current data (ADR 0064): `update_requested`, which the supervisor's next pass honours (the server placed again on the current trees, a new generation; players drop). Idempotent. 404 `not_found` for an id that is not the account's; 409 `not_live` once it has ended, 409 `up_to_date` when its pins are the current trees already; 202 `{"server"}` |
| `GET /api/me/servers/<id>/stats` | a website session | one of the account's servers' stats, in any state: `?range=1h` (a point a minute), `24h` (the default; five minutes), `7d` (an hour) or `30d` (four hours), another 400 `invalid_field`; 404 `not_found` for an id that is not the account's; 429 past the caller's read bucket (60, then two a second). `{"server_id", "range", "live", "summary", "series"}`: `live` the server now, null once it has ended (`state`, `uptime_seconds` while running, else null, `players`, `max_players`, `mission`, `game_type` and `time_left` in seconds from its listing, `scene` (`game`, `menus`, `unknown`) and `link_state` from its admin link, `cpu_percent` and `memory_bytes` from its newest sample; each null where nothing is known); `summary` over the range (`live_seconds`, `running_seconds`, `availability` (running over live, 0..1, null with none), `restarts` (crash restarts), `peak_players`, `avg_players` (players at a time while it ran, null with none), `player_hours`, `unique_players`, `missions`, `egress_bytes`, `egress_bytes_month` (this calendar month, UTC, whatever the range) and `approximate`, the names of the fields counted approximately); `series` (`step_seconds`, and `points`, one per step oldest first: `t` (the step's start, ISO 8601 UTC), `players`, `max_players`, `running`, `cpu_percent`, `egress_bytes`, null where no sample fell); `Cache-Control: no-store` |
| `GET /api/me/servers/stats` | a website session | the account's servers together, as above with no `server_id` and `live` null: the summary's times added up, its peak the most players at once across them, each point's players and bytes added up and `running` true while any ran |
| `PUT /api/admin/users/<id>/comp-servers` | admin | `{"comp_servers": 0..16}`, the admin's grant; 200 `{"user"}` (which carries `comp_servers` and `max_servers`, the entitlement); 400 `invalid_json`, `invalid_field` (no whole number in 0..16) or `missing_field`; 404 for an unknown id or a service account. An entitlement down to no server stops the account's hosted server, as a ban does |
| `GET /api/admin/hosted` | admin | the fleet: `{"enabled", "provider", "cap", "live", "shared", "own", "machines", "servers"}`. `shared` and `own` are `{live, room}`: each machine kind's live servers and the most its machines hold (`ONNET_FLEET_MAX_SHARED` x `ONNET_FLEET_SHARED_SLOTS`, `ONNET_FLEET_MAX_OWN`). `enabled` a machine driver with at least one offered data set. `machines`: the live machines by id, then the newest 10 ended or failed, each `id`, `provider`, `kind` (`shared` or `own`), `state` (`launching`, `booting`, `ready`, `draining`, `ending`, `ended`, `failed`, `lost`), `cloud_id`, `instance_type`, `slots`, `slots_used` (the live servers on it), `private_address`, `public_address`, `bootstrapped`, `agent_version`, `template_version`, `serve_image`, `reserved_for` (an own machine's server, else null), `last_report_at`, `launched_at`, `ready_at`, `ended_at`, `end_reason` and `egress_bytes_month` (its slots' egress this calendar month, UTC, as the stats count it). `servers`: the newest 100 the query takes, live first (`?owner=<username>`: that account's alone, none for an unknown name; `?state=live`, `ended` or `all`, the default; another state 400 `invalid_field`), each as `GET /api/me/servers`' plus `owner` (`{id, username, tier}`, `tier` its granting support tier or null; null once the account is gone), `service_account` (`{id, username}` or null), `provider`, `container` (its container's name), `machine_address`, `admin_port`, `machine_ref` and `slot` (null with none; an ended server's where it last ran), `game_port` (null before a placement), `generation`, `serve_image`, `base_tree_id` and `expansion_tree_id` (its pins, null before its first start), `outdated` (newer data than its pins, while live), `update_requested` and `players` (its listing's, null when not listed). The background jobs (the machines' launches and ends among them) are `GET /api/admin/jobs/overview`'s ("Background jobs" above). Never the admin password or an agent's credential; `Cache-Control: no-store` |
| `GET /api/admin/hosted/<id>` | admin | `{"server", "machine"}`: one server in any state as the fleet view's plus `events` (the newest 50 transitions, oldest first), and the machine it is on or last ran on (as above, or null); 404 `not_found`; `Cache-Control: no-store` |
| `POST /api/admin/fleet/machines/<id>/drain` | admin | the machine takes no new server and the supervisor moves its servers off it one at a time (no restart counted), then ends it; 202 `{"machine"}` (one draining already included); 404 `not_found`; 409 `not_live` once it is ending or ended |
| `POST /api/admin/fleet/machines/<id>/end` | admin | the machine to `ending`: the next pass terminates it and places its servers again (no restart counted); its agent is refused from then on; 202 `{"machine"}` (one ending already included); 404 `not_found`; 409 `not_live` once it has ended |
| `POST /api/admin/fleet/roll` | admin | a fleet roll: every machine that takes servers (launching, booting, ready) drains, its servers moving to machines launched for them, so a new serve image or launch template reaches the fleet; 202 `{"draining": <how many>, "machines": [<their ids>]}` |
| `POST /api/admin/hosted/update-all` | admin | the force of every live server whose pins are behind its data sets' current trees onto them: body `{}` for every data set, or `{"data_set_id": n}` for that data set's servers (as base or expansion) alone; each marked `update_requested` for the supervisor's next pass; 400 `invalid_json`, `invalid_field`; 202 `{"updating": <how many>}` |
| `GET /api/admin/hosted/<id>/stats` | admin | any server's stats, as the owner's; 404 `not_found` |
| `GET /api/admin/hosted/stats` | admin | the fleet's: every server together, as the account's |
| `GET /api/admin/hosted/usage` | admin | what every live server (any state but `stopped` and `failed`, on any machine) uses of its slot, from the samples: `{"at", "window_seconds", "slot_memory_bytes", "slot_cpus", "totals", "machines", "servers"}`. `at` now (ISO 8601 UTC); `window_seconds` 3600, the last hour; `slot_memory_bytes` and `slot_cpus` each container's limits (`ONNET_FLEET_SLOT_MEMORY_MB`, `ONNET_FLEET_SLOT_CPUS`); `totals` the fleet's `servers_live`, `machines_live`, `slots` and `slots_used` (over the live machines) and the usage fields over every live server; `machines` each live machine by id (`id`, `kind`, `state`, `instance_type`, `slots`, `slots_used` and the usage fields over the servers on it); `servers` by machine, then slot, the ones placed nowhere last by id, each `id`, `name`, `state`, `own_machine`, `owner` (`{id, username}`, null once the account is gone), `machine_ref` and `slot` (null with none), `uptime_seconds` (while running, else null), its newest sample within 3 minutes (`sample_at`, `players`, `max_players`, `cpu_percent` (0..100 of the slot's CPUs), `memory_bytes`, `egress_bytes_per_second` (that minute's bytes over 60 s), each null without one) and `window`, its samples in the window summed up (`minutes`, `cpu_percent_mean`, `cpu_percent_peak`, `memory_bytes_peak`, `egress_bytes`). The usage fields: `reporting` (the servers whose newest sample carries their agent's CPU and memory), `players`, `memory_bytes` and `memory_limit_bytes` (the reporting servers' memory and their slots' limits), `cpus_used` and `cpus_limit` (likewise, in CPUs), `egress_bytes_per_second` (the newest samples' rates added up) and `egress_bytes_window`. 429 past the caller's stats bucket; `Cache-Control: no-store` |
| `POST /api/admin/hosted/<id>/stop` | admin | 404 `not_found`, 409 `not_live`, else 202 `{"server"}` |
| `POST /api/admin/hosted/stop-all` | admin | the kill switch: every live server to `stopping`; 200 `{"stopping": <how many>}` |

**Game data** (`apps/novaworld_web/data_sets/`, ADR 0064): the data sets as rows
(`hosted/data_set_repository.h`, migrations 0011 and 0013), each one product (a base game
or one expansion of a base), the trees the admin page uploads into the deployment's object
store (`object_store.h`: web begins each file's multipart upload, takes each part's PUT
and hands it to the store, and completes the upload; the bytes pass through this program,
32 MiB a part, inside Cloudflare's request limits), and the update releases (1SFS
packages, uploaded the same way) the worker validates and applies and the update listener
serves. The jobs a route enqueues (`tree.finalize`, `release.validate`, `release.apply`,
`tree.prune`; "Background jobs" above) commit in the transaction of the state move that
causes them. Every route is the admin's (the Bearer token or an admin session); each
state change is logged with its actor and status. Without an object store (hosting off)
the store routes answer 503 `store_unavailable`; a store call that fails mid-way answers
502 `store_error` (the store's own text goes to the log alone). Timestamps are the
database's UTC `YYYY-MM-DD HH:MM:SS`, `""` until set; ids are numbers; a body with
nothing to say is `{}`. The admin's force-all update of the servers behind their data
sets is `POST /api/admin/hosted/update-all` (the fleet routes). The shapes: `DataSet` = `{id,
display_name, kind ("base" or "expansion"), parent_id, expansion_code, title, game,
updater_login, command_file, retail_updater, current_tree ({id, version, ready_at} or
null), baseline_tree_id, published_release ({id, name, sha256} or null), servers_live,
servers_outdated, trees: [Tree]}` (its trees newest first, pruned ones left out); `Tree`
= `{id, state (uploading, finalizing, building, ready, failed, pruned), source (baseline,
release), release_id, version, bytes, files (how many), missions (how many), pinned_by
(the live servers running on it), error, created_by, created_at, ready_at}`; `Release` =
`{id, status (uploading, validating, invalid, draft, published, retired), name, sha256,
bytes, package_product, error, sections: [{data_set_id, display_name, expansion,
command_file, version, tree ({id, state} or null)}], created_by, created_at,
published_at, retired_at}`.

| Route | Access | Replies |
| --- | --- | --- |
| `GET /api/admin/data-sets` | admin | `{"store" ({"kind": "s3" or "local"}; null with none), "titles" (what a base may speak: `jo`, `dfx2`), "games" (the lobbies, each `slug` and `display_name`), "data_sets"}` (bases first, each a `DataSet`); `Cache-Control: no-store` |
| `POST /api/admin/data-sets` | admin | `{display_name, kind, parent_id?, expansion_code?, title?, game?, updater_login?, command_file?, retail_updater?}`: a base (a title among the titles, a `games` slug; no parent or code) or an expansion (a base `parent_id`, an `expansion_code` of 1 to 8 of `a-z0-9`; no title or lobby); `display_name` 1 to 64 characters without control characters; the updater's product both or neither (`updater_login` 1 to 32 printable characters without spaces, `command_file` 1 to 15 ending `.CMD`); `retail_updater` (default false) names `ftp.novaworld.com` in the updater's SERVER.LST instead of `ONNET_UPDATE_HOST`. 201 `{"data_set"}`; 400 `invalid_json`, `missing_field`, `invalid_field` (each with the `field`); 409 `duplicate_login` (another data set holds the login, without case), `duplicate_expansion`, `no_such_parent` |
| `PATCH /api/admin/data-sets/<id>` | admin | any of `display_name`, `game` (a base's), `updater_login`, `command_file`, `retail_updater`, the shapes as the create's (the resulting login and command file both or neither): 200 `{"data_set"}`; 400; 404 `not_found`; 409 `duplicate_login` |
| `DELETE /api/admin/data-sets/<id>` | admin | the data set and its trees: each tree's objects go first, here and now (its uploads in flight aborted, its prefix removed, the row marked pruned as it goes), then the row; 204; 404 `not_found`; 409 `in_use` while a live server runs it or an expansion has it as parent; 503 `store_unavailable` (no store, or one that did not remove the trees: the data set stays) |
| `POST /api/admin/data-sets/<id>/trees` | admin | `{"files": [{"name", "size"}]}`: a new uploading tree and one multipart upload per file, each name canonicalised to the kind's spelling (`hosted/data_set_files.h`): 201 `{"tree" (a Tree), "uploads": [{"name", "key" (`trees/<tree id>/<name>`), "upload_id", "part_size" (33554432: 32 MiB)}]}`; 400 `invalid_json`, `invalid_field`, `invalid_files` with `missing` (the required names not given) and `unexpected` (names the kind lacks); 404 `not_found`; 409 `upload_in_flight` (a tree of it is uploading or finalizing); 503 `store_unavailable` |
| `PUT /api/admin/data-sets/<id>/trees/<t>/files/<name>/parts/<n>?upload_id=<u>` | admin | the body (`application/octet-stream`, at most 32 MiB) handed to the store as part `n` (1 to 10000) of the file's upload: 200 `{"etag"}` (what the completion reports for it); 400 `invalid_field` (`part_number`, `upload_id`); 404 `not_found` (the data set, the tree, a name the kind lacks); 409 `not_uploading`; 413 `too_large`; 502 `store_error` (an upload the store does not know among the causes); 503 `store_unavailable`; `Cache-Control: no-store` |
| `POST /api/admin/data-sets/<id>/trees/<t>/files/<name>/complete` | admin | `{"upload_id", "parts": [{"part_number", "etag"}]}` (each part's ETag as its PUT answered): the file's upload completed in the store, 200 `{"completed": true}`; 400; 404; 409 `not_uploading`; 502 `store_error` (a part missing or an ETag that differs among them); 503 |
| `POST /api/admin/data-sets/<id>/trees/<t>/finalize` | admin | the tree `finalizing` and its `tree.finalize` job (key `tree.finalize:<t>`), in one transaction: 202 `{"tree"}`; 404; 409 `not_uploading` |
| `DELETE /api/admin/data-sets/<id>/trees/<t>` | admin | an uploading tree: its uploads aborted, its objects removed, the row pruned, 204; a ready or failed one: the row marked pruned (its objects kept for the job) and a `tree.prune` job in the same transaction, 202 `{"tree"}`; a pruned one 204; 404; 409 `current` (what new starts take), `baseline` (what a release applies to), `pinned` with `servers` (how many live ones run on it), `in_progress` (the worker is finalizing or building it); 502; 503 |
| `GET /api/admin/releases` | admin | `{"releases"}`, newest first, each a `Release`; `Cache-Control: no-store` |
| `POST /api/admin/releases` | admin | `{"name"?, "size"}` (the package's bytes; the name at most 64 characters): a release `uploading` and its package's multipart upload, 201 `{"release", "upload": {"key" (`updates/upload-<id>.sfs`), "upload_id", "part_size" (33554432: 32 MiB)}}`; 400 `invalid_json`, `missing_field`, `invalid_field`; 503 `store_unavailable` |
| `PUT /api/admin/releases/<id>/parts/<n>?upload_id=<u>` | admin | the body (`application/octet-stream`, at most 32 MiB) handed to the store as part `n` (1 to 10000) of the package's upload, as a tree's parts: 200 `{"etag"}`; 400 `invalid_field` (`part_number`, `upload_id`); 404 `not_found`; 409 `not_uploading`; 413 `too_large`; 502 `store_error`; 503 `store_unavailable`; `Cache-Control: no-store` |
| `POST /api/admin/releases/<id>/complete` | admin | `{"upload_id", "parts": [{"part_number", "etag"}]}`, as a tree file's: the package's upload completed, then the release `validating` with its `release.validate` job (key `release.validate:<id>`) in one transaction (the worker parses it, maps each section to a data set by its command file, moves the object under `updates/<sha256>.sfs` and makes it a `draft`, or `invalid` with why): 202 `{"release"}`; 400; 404; 409 `not_uploading`; 502; 503 |
| `POST /api/admin/releases/<id>/publish` | admin | a draft published: its sections' data sets serve it to the updater (the release each named before is retired once no data set names it), and one `release.apply` job per section (key `release.apply:<id>:<data set id>`; the worker builds each data set's new tree from its baseline), in one transaction: 202 `{"release"}`; 404; 409 `not_draft` |
| `POST /api/admin/releases/<id>/retire` | admin | a published release retired (its data sets serve none): 202 `{"release"}`; 404; 409 `not_published` |
| `DELETE /api/admin/releases/<id>` | admin | a draft, invalid, retired or uploading (abandoned) release: its package objects removed (the upload key, with any upload in flight aborted, and the validated key once it has a sha; a missing one is no failure), the row gone (a tree built from it keeps its row): 204; 404; 409 `published` (retire it first), `validating` (the worker is on it); 502; 503 |

**The update listener** (`apps/novaworld_web/updater/`, ADR 0061 d3, ADR 0064 d9):
UPDATE.EXE's gateway and update server in one, as retail's was, over the published
releases, started by web when `ONNET_UPDATE_HOST` is set and the object store is there.
A thread of its own pumps `net/update`'s connection machine through `apps/common`'s TCP
listener on `ONNET_UPDATE_PORTS` (retail's four), watched by the stall watchdog; a second
thread refreshes its catalog (`release_catalog.*`) from the rows every 30 s: each data
set with an updater login serves the section of its published release that carries its
command file, the package fetched from `updates/<sha256>.sfs` into the cache
(`ONNET_UPDATE_CACHE_DIR`, or a scratch directory of the process's own) as `<sha256>.sfs`
and the section unpacked beside it as `<sha256>/<section index>/<UPPER NAME>`, one file
per entry, streamed from disk as a reply's socket takes it and never held whole in memory (a
connection is read, and its next replies made, only while less than 64 KiB of its replies
wait unsent, each step at most 64 KiB more); a package no published section names any more
leaves the cache. `SERVER.LST` names one host (so the client never
load-tests): `ftp.novaworld.com` for a data set marked `retail_updater` (a retail install
reaches it through the Switcher's hosts block), else `ONNET_UPDATE_HOST`. A login no
published release serves is refused. Logged as `opennova-update-tool serve` logs, under
`[update]`.

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_UPDATE_HOST` | none | The update host `SERVER.LST` sends a client of this deployment to (`update.<domain>`); unset, web runs no update listener. |
| `ONNET_UPDATE_PORTS` | `20044,20556,21068,21580` | The gateway ports the listener binds, comma-separated (retail's four, which UPDATE.EXE tries in turn); a list that does not read is a boot FATAL. |
| `ONNET_UPDATE_CACHE_DIR` | a scratch directory | Where the packages and their unpacked sections are kept between refreshes; the scratch default is fetched again after a restart. |

**The console routes** (`hosted/console_routes.*`) run the curated commands on a hosted
server's admin link, which the controller holds: a route writes its command to
`hosted_console_commands` with the deadline its budget allows and reads its row until it
is done; the controller's console drain (`hosted/console_queue.*`) claims the queued
commands (`FOR UPDATE SKIP LOCKED`), runs each on its link off the drain's thread, and
writes the outcome, whether it was sent and each reply packet (`hosted_console_replies`,
in the server's code page); the route reads them and deletes the row. A command still
queued when the route's wait ends is withdrawn, never sent; one the link holds answers
with the link's own timeout (whether it was sent) within half a second, and one with no
answer past that reads as not sent (only a done row says a command went out); a command
whose controller claimed it and went away (each claim carries its controller's
generation) the next leader's drain answers link down; the rows a route left behind are
deleted a minute past their deadline. The link's state is its row
(`hosted_admin_links`). The owner's act on one of the caller's own servers by its id
(`/api/me/servers/<id>/...`; another account's id answers as no server's does); the
admin's take any server's id. In order: the session (401; 403 without the CSRF header on a `POST`, an admin
session's too) or the admin check; the arguments (400, before anything is read or sent);
the server (404 `not_found`, 409 `not_live`), running (409 `server_stopping` while it stops, 409 `server_not_running` before
it runs) with its link up (503 `admin_unavailable`); the admission (below: 429
`console_busy`, 503 `console_full`, 409 `rotation_busy`, each with `Retry-After`); for an
owner, a token from the account's read bucket (30, then two a second) or, for a command
that changes the server, its write bucket (8, then one every 2 s, as the owner's pushes;
429 `rate_limited` with `Retry-After`, logged once a minute per account); then the link:
503 `admin_unavailable` or `admin_busy` (either hands the token back), 504
`admin_timeout` (with `sent`) past the 3 s a request has for all its commands, and the
command's own refusals below. Every refusal is an error code with the service's own words,
never the server's text. The reads answer `Cache-Control: no-store`.

**The admission.** A console request that runs commands holds an HTTP handler thread while
it waits on the link (up to 3 s; a hosted server pumps its admin port from its main loop,
so a map load stalls it). So that the console never takes every thread from the retail
pages, the `.gsb` feeds and the logins:
- the HTTP listener runs `max(hardware_concurrency(), 8)` handler threads
  (`HttpListener::handler_threads`; Crow v1.2.0 runs `concurrency - 1` of them beside its
  acceptor). Each holds a pooled connection for its request; the pool opens at most
  `DATABASE_POOL_MAX` of them, and a lease past that waits (up to 30 s) for one to come
  back;
- each account (each admin session; the Bearer token as one) has at most one console
  request in flight in each web process: a second answers 429 `console_busy` at once, with
  no wait and no token drawn;
- at most handler threads - 2 console requests are admitted in each web process (503
  `console_full` past that), so two of its threads always stay for everything else (both
  guard that process's own handler threads, so they stay its own);
- a rotation edit (an add, a remove, a next) also holds its server's one edit, so its read
  of the rotation and its edit run as a pair: a second edit meanwhile, from any web
  process, answers 409 `rotation_busy`, and two removes can never both pass the last-map
  check. The edit is a Redis key per server (`nw:rotation:<id>`, set only if absent, 15 s
  at most, deleted by its holder's token), so it holds across every web replica.

`GET .../console` reads the database alone (the ring and the link's row) and is not
admitted. The admission is each web process's own. The site serializes its
console calls per page: a server's console page, the owner's and the admin's, sends every
admitted call (their polls and their commands; the console log's read is not admitted, so
it runs beside them) through one queue (`web/src/utils/consoleQueue.ts`), a command ahead
of the waiting polls, so a page never meets its own `console_busy`.

| Route (owner; the admin's: `/api/admin/hosted/<id>/...`) | Command | Replies |
| --- | --- | --- |
| `GET /api/me/servers/<id>/players` | `PLAYER LIST` | `{"server_id", "players"}`: `slot`, `name`, `team`, `class`, `kills`, `deaths`, `ping` (the server's own slot-0 row left out); 409 `not_in_game`; any other `ERROR` reply 502 `admin_error` |
| `POST /api/me/servers/<id>/players/<slot>/punt`, `/ban`, `/swap`, `/kill` | `PLAYER PUNT` / `BAN` / `SWAPTEAM` / `KILL <slot>` | `{"server_id", "slot", "result"}` (`punted`, `banned` with `listed`: the PCID listed, a NovaWorld host's, `swapped`, `killed`); 400 `invalid_slot` (1..255), 404 `player_not_found`, 409 `no_pcid` or `not_connected` (a ban) |
| `GET /api/me/servers/<id>/missions` | `MISSION AVAILABLE` | `{"server_id", "missions"}`: `index`, `file`, `title` |
| `GET /api/me/servers/<id>/rotation` | `MISSION LIST` (and `GET GAMESTATE` when it lists none) | `{"server_id", "rotation"}`: `index`, `file`, `swap_sides`, `flipped`, `one_shot`, `current`, `next`; 409 `not_in_game` between matches (retail lists nothing there) |
| `POST /api/me/servers/<id>/rotation` | `MISSION LIST` (and `GET GAMESTATE` when it lists none), then `MISSION ADD <file> <0\|1>` | body `{"mission": "<file>.bms", "swap_sides": false}`, appended; `{"result": "added"}`; 400 `invalid_mission`, `missing_field`, `invalid_field`; 404 `mission_not_found`; 409 `rotation_full` (128 maps, the start settings' cap: policy, not a server limit), `not_in_game` (retail answers an add OK between matches and appends nothing) |
| `POST /api/me/servers/<id>/rotation/<index>/remove`, `/next` | `MISSION LIST` (and `GET GAMESTATE` when it lists none), then `MISSION REMOVE` / `SETNEXT <index>` | `{"result": "removed"}` / `"next_set"`; 400 `invalid_index` (0..31); 404 `no_such_entry` (the server answers OK for any index, so the rotation is read first); 409 `last_mission` (the last map stays: an empty rotation ends the session at the round's end), `not_in_game` |
| `POST /api/me/servers/<id>/rotation/cycle` | `MISSION CYCLE` | `{"result": "cycling"}`: the round ends now and the next map loads; 409 `not_in_game` |
| `GET /api/me/servers/<id>/chat` | `CHAT GET` | `{"server_id", "lines"}`, the server's chat window oldest first |
| `POST /api/me/servers/<id>/chat` | `CHAT SEND <text>` | body `{"text"}`: cp1252 text, no control character, not blank, at most 59 characters (all a server chat line keeps); `{"result": "sent"}` (a repeat within about 20 s is dropped by the server's flood control, and the server answers OK all the same); 400 `invalid_text` |
| `GET /api/me/servers/<id>/console` | none | the server's console ring, in any state (an ended server's last lines included): `{"server_id", "link", "lines"}`, `link` its link's `state`, `game_state`, `last_poll_at`, `up_since` and `error` (null with none), `lines` the newest `limit` (200; at most 500) with an id past `after` (0), oldest first: `id`, `kind`, `actor`, `text`, `at`; 404 `not_found` |
| `GET /api/admin/hosted/<id>/settings` (admin only) | `GET GAMESETTINGS` | `{"server_id", "settings"}`: `key`, `value`, in the server's order (the game and side passwords among them) |

**The console's commands are a curated list (policy, ADR 0053 d9;
`hosted/console_commands.h`).** Owners get exactly the commands above but `GET
GAMESETTINGS`; admins get that one wider read besides. There is no passthrough of an
arbitrary admin line: every line is composed from checked arguments (a slot 1..255, since
the server compares `(u8)atol` and 256 would be its own slot 0; an index 0..127, a rotation being at most 128 maps (policy); a mission
a `.bms` name of letters, digits, `_` and `-`; chat text as above), so nothing reaches the
server's files or config (`SET`, `WEAPON SET`, `CMD`), its admin users or ban files
(`ADMINUSER`, `BANLIST`), or its session's end (`GOTO`, `MISSION CLEAR`, `QUIT`) outside the
supervisor's stop. `MISSION ADD` takes no insert position (retail's insert leaves the
next-map cursor unset when the list grows) and no `ONESHOT`, and a `PLAYER ... ALL` form
is not on the list. The commands that change the server are recorded in the console with
their sender; the reads are not.

## Billing (monthly support tiers)

A support tier is a monthly Stripe subscription that supports the project and sets the
account's hosting entitlement ("Hosted servers" above): tier 1, 2 or 3 lets it run 1, 2 or
3 hosted servers (`billing/tiers.h`), and the own-machine add-on, one subscription of
quantity 1 per own machine (bought through Checkout, canceled in the Portal, which
changes no quantity), moves that many of them onto machines of their own while a tier
also grants. The service makes Stripe Checkout and Customer Portal
sessions and hands the browser their URL; no publishable key and no Stripe.js reach the
site. Everything here is service policy (`billing/`).

- **The transport** (`billing/stripe_api.*`): every call runs `curl -q --config <file>`
  through the apps' command runner, the key, the headers and the form body in an
  owner-only (`0600`) file of the call's own in the service's owner-only scratch
  directory (`/tmp/opennova-billing-<random>`, removed at the shutdown), deleted when
  curl ends, so no secret is on a command line or in the child's environment (`PATH`,
  `LANG`, `LC_*`, `TZ` alone). Each request pins the API version
  (`kStripeApiVersion`, basil), carries an idempotency key on a POST, and is tried once
  more on no answer, a 429 or a 5xx. The key and the webhook secret are log secrets.
- **The cache** (`billing/billing_repository.*`, `backend/migrations/0002_billing.sql`):
  each account's Stripe customer (`billing_customers`), every subscription of it as
  Stripe last answered (`billing_subscriptions`: its item, `tier`, `own_machine` or
  `unknown` for a price no setting names, read from its price; its status and since when;
  its period end and whether it cancels at it), the webhook events
  (`billing_events`) and the database's instance id (`service_meta`, 32 hex digits from
  the OS CSPRNG at the first boot). The entitlement reads only this cache, so it holds
  whether or not billing is configured.
- **The instance id** is stamped on every customer and subscription the service makes
  (`opennova_instance`, with `opennova_user_id` and `opennova_username`), so a reset
  database, or a developer's local one on a shared Stripe test account, never adopts a
  subscription that is not its own: another instance's is an orphan the admin page lists
  and an admin may relink.
- **The webhook** (`POST /api/billing/webhook`, no session and no CSRF header; still
  behind the site's proxy): a body past 1 MiB is 413; the `Stripe-Signature` header must
  hold a `v1` HMAC-SHA256 of `<t>.<body>` under `STRIPE_WEBHOOK_SECRET` within 300 s of
  the service's clock (any of several `v1`s, so a secret's roll needs no second setting),
  else 400 `bad_signature` and one WARN line naming the check's class and the clock skew,
  never the body or the header; a signed body that is no event is 400 `invalid_json`; an
  event of the other mode is answered 200 and dropped. The event is recorded once (a
  redelivery is a no-op) with the customer it names, a `billing.sync` job is enqueued (one waiting already takes it), and the reply
  is 200 `{"received": true}`. The payload is never trusted beyond its customer's id.
- **The sync** (`billing/billing_sync.*`, `billing/billing_jobs.*`): two jobs on the
  service's queue ("Background jobs" above) that any worker runs, their state rows
  (`billing_sync_state`: the prices as last checked, under the price lists and mode they
  were checked with, the last reconcile and what it found; `billing_sync_backoff`), read
  by the routes and the heartbeat. `billing.sync`, enqueued by each fresh webhook event
  and every minute while an event waits: each customer with an unrecorded event is read
  back from Stripe with all its subscriptions, then written in one transaction (no Stripe
  call ever runs inside one), the longest-waiting first; a customer Stripe could not be
  asked about waits 60 s, doubling to 15 min. A deleted customer's subscriptions are
  canceled here and its row goes. `billing.reconcile`, the full reconcile, runs at the
  controller's start, every `ONNET_BILLING_RECONCILE_MINUTES` and when an admin asks
  (again 15 min after one that did not complete): every
  subscription, a page of 100 at a time with its customer, written when it is this
  instance's (a missing customer row made again from its metadata), listed as an orphan
  when it grants and is another instance's or names no account here, and a granting row
  Stripe no longer lists read alone and canceled when Stripe no longer knows it; accounts
  with two granting tiers are listed as duplicates. The configured prices are checked
  then (and by a `billing.sync` while they are unchecked): each a monthly recurring price in the key's mode, the one
  each list sells active and all sold in one currency; else checkout answers 503
  `billing_misconfigured` until they are fixed.

| Setting | Default | Meaning |
| --- | --- | --- |
| `STRIPE_SECRET_KEY` | none | `sk_` or `rk_` (a restricted key is the recommended one), `test` or `live`: the mode everything else must match. |
| `STRIPE_WEBHOOK_SECRET` | none | The webhook endpoint's signing secret, `whsec_...`. |
| `STRIPE_PRICE_TIER1`..`3`, `STRIPE_PRICE_OWN_MACHINE` | none | Comma lists of `price_...` ids: the first one sold, the rest still recognized (grandfathered); each price in one list alone. The amounts are Stripe's, never the service's. |
| `ONNET_SITE_URL` | none | The site's origin (no path): Checkout returns to `/me/support?checkout=success&session_id=...` or `?checkout=cancel` there, the portal to `/me/support`. `https://` with a live key. The dev compose stack sets `http://localhost:5173`. |
| `ONNET_STRIPE_CURL` | `/usr/bin/curl` | The curl binary, an absolute path. |
| `ONNET_BILLING_AUTOMATIC_TAX` | off | `1` turns Stripe Tax on in Checkout (it collects the customer's address). |
| `ONNET_BILLING_RECONCILE_MINUTES` | 360 | The full reconcile's period, 5 to 10080. |

A Stripe call a route makes that fails (no answer, or a refusal) is 503
`stripe_unavailable`, never 502: Cloudflare answers an origin's 502 or 504 with a page of
its own, so the site's page would not see the reply's code. The route's log line names
Stripe's status, error code and message (`[billing] account '<name>' (id=<n>) checkout ->
no session: rejected 400 (<code>): <message>`); a refusal of every Checkout session is a
setting of the Stripe account, such as the terms of service URL Checkout's consent box
needs (DEPLOY.md, "Monthly support tiers (Stripe)").

Billing is on only when the whole set is complete and well-formed (`REPLACE_ME`, the
vault's placeholder, counts as unset); otherwise the boot logs `[boot] WARN billing:
<what is missing>; billing off`, the routes below answer 503 `billing_disabled` (the tiers
and the admin's view answer `{"enabled": false}`), and the hosting entitlement still reads
the cache. The dev
compose stack takes the test-mode set from the vault through
`deploy/env/billing.dev.env.tpl` (`op run --env-file=...`).

| Route | Access | Replies |
| --- | --- | --- |
| `GET /api/billing/tiers` | public | `{"enabled", "currency", "tiers": [{tier, max_servers, amount, interval: "month"}], "own_machine": {amount, interval} \| null}`: `enabled` is billing on with its prices checked out; each `amount` the price sold now in the currency's smallest unit, null until read |
| `GET /api/me/billing` | a website session | `{"enabled", "max_servers", "comp_servers", "own_machines", "used", "stopping", "can_start", "subscription", "own_machine_subscription", "can_manage", "can_subscribe"}`: `used` the account's live hosted servers, `stopping` how many of those are stopping (the owners' limits count the rest); `subscription` the support tier shown (a granting one, the highest tier, else the newest still open): `{tier, status, entitled, current_period_end (ISO 8601 UTC), cancel_at_period_end}` or null; `own_machine_subscription` `{quantity, status, current_period_end, cancel_at_period_end}` or null; `can_manage` the account has a Stripe customer; `can_subscribe` the prices check out, the account is an active player's and no tier grants. `Cache-Control: no-store` |
| `POST /api/billing/checkout` | a website session, the CSRF header | `{"item": "tier1" \| "tier2" \| "tier3" \| "own_machine"}`; 429 past the account's bucket (6, then one every 20 s, shared with the portal); 400 `invalid_json` or `invalid_item`; 503 `billing_misconfigured`; 409 `checkout_in_progress` while another checkout of the account is running (one at a time); 403 `not_eligible` (an account not active, or the add-on with no tier granting); 409 `already_subscribed` while a tier grants (the portal changes it); 409 `own_machine_limit` for the add-on once the account has an own machine for every server it may run (each add-on checkout sells one; several are fine below that); 503 `stripe_unavailable`; 200 `{"url"}`. The account's previous session is settled first: expired while open; once complete (paid, the webhook perhaps still on its way), the customer is synced from Stripe and the checkout judged again, so nothing is sold twice (503 when Stripe cannot say). Its customer is made at its first checkout (idempotency key `opennova-<instance>-customer-<id>`), and made again in place of a row of the other Stripe mode; the session asks for the terms of service, lives an hour from its creation (Stripe refuses under 30 min) and stamps the instance and the account on the subscription it makes |
| `POST /api/billing/portal` | a website session, the CSRF header | 429 (the checkout bucket); 404 `no_billing_account`; 503 `stripe_unavailable`; 200 `{"url"}` |
| `POST /api/me/billing/sync` | a website session, the CSRF header | the account's customer read back from Stripe now (the success page's, before the webhook comes); 429 past its bucket (3, then one every 10 s); 404 `no_billing_account`; 503 `stripe_unavailable`; else as `GET /api/me/billing` |
| `POST /api/billing/webhook` | Stripe's signature | above |
| `GET /api/admin/billing` | admin | `{"enabled", "mode" ("test" \| "live"), "prices_ok", "prices_error", "last_reconcile_at" (ISO or null), "last_reconcile_error", "pending_events", "subscriptions" (each row with an account: `user_id`, `username`, `stripe_customer_id`, `stripe_subscription_id`, `item`, `tier`, `quantity`, `status`, `entitled`, `current_period_end`, `cancel_at_period_end`), "duplicates" (account ids), "unknown_prices" (the prices of our subscriptions that would grant but that no `STRIPE_PRICE_*` list names, as the last reconcile found them), "orphans" (`stripe_customer_id`, `stripe_subscription_id`, `username_hint`, `status`), "recent_events" (the newest 50: `event_id`, `type`, `received_at`, `outcome`), "limits_hold", "limits_hold_reason", "limits_hold_owners" and "limits_hold_servers" (the hosted servers' limits hold and the stops it keeps back, "Hosted servers" above)}; with billing off `{"enabled": false}` and the limits hold alone; `Cache-Control: no-store` |
| `POST /api/admin/billing/reconcile` | admin | no body or `{}`: a full reconcile (a `billing.reconcile` job, at once): 202 `{"reconcile": "requested", "force_limits": false}`; `{"force_limits": true}` also has the supervisor's next pass apply the stops the limits hold keeps back (`"force_limits": true`); 400 `invalid_json`, `invalid_field`; with billing off 503, but an override still applies (202 `{"reconcile": "off", "force_limits": true}`) |
| `POST /api/admin/billing/link` | admin | `{"user_id", "stripe_customer_id"}`: the customer (an orphan's) made the account's, its metadata stamped with this instance and the account, then synced; 400 `invalid_json`, `missing_field`, `invalid_field`; 404 `not_found` (no such player, or a customer Stripe does not know); 503 `stripe_unavailable`; 200 `{"user_id", "stripe_customer_id", "outcome"}` |

`DELETE /api/admin/users/<id>` answers 409 `active_subscription` while a subscription of
the account still grants: Stripe would bill an account that is gone, so it is canceled
first. `GET /api/admin/users` carries each account's `entitlement` (`max_servers`,
`source`, `tier`), `live_servers` and `subscription_status`; `GET /api/admin/users/<id>`
the account (`{"user", "entitlement"` in full, with `comp_servers`, `tier_servers` (the
servers its tier alone grants), `own_machines` and `can_start`, `"servers"`, `"billing":
{livemode, stripe_customer_id, subscriptions}}`), 404 for no such player.

## CD keys (DFX2 activation)

Delta Force Xtreme 2 activates against NovaWorld; Joint Operations does not
([`docs/titles/dfx2-vs-jo-re.md`](../../docs/titles/dfx2-vs-jo-re.md) section 9). JO only
reads its registry `CDKIID` into the `NWCDKIID` form field, which NovaLogic's service
accepted empty, and its host checks no key, so JO's flow takes no key and keeps taking an
empty `NWCDKIID` / `NWCDKIIDEXP1`. DFX2's boot and STARTUP menu gate on a registry
`CDKIID` that NovaLogic's (now dead, `NWNODB01`) service handed out as the `NWACT`
browser variable, and its LAN host checks the wire `CDKIID` that value decodes to
(DFX2-13, codes 10 to 13). The codec is `engine/net/novacrypto/cdkey.h`; the keys are
`cd_keys.h` (the `cd_keys` and `cd_key_activations` tables) and the routes
`cd_key_routes.h`.

- **A key** is its serial, `DFX2-XXXX-XXXX-XXXX-XXXX` (64 bits from the OS CSPRNG, the
  service's own format: NovaLogic's issuer is unwitnessed), and its wire key, the serial
  under DFX2's wire chain: the `CDKIID` an activated copy keeps and a LAN host decodes.
  Every serial is unique, so two accounts' copies never meet at codes 12 and 13. No `/MP`
  tail and no `CDKIIDEXP1` are issued (nothing in `dfx2.exe` writes a `CDKIIDEXP1`). An
  account holds at most one live key per title; a revoked key activates no machine.
- **The activation** of one machine is `NWACT = mix_up(encode(wire key, registry chain),
  NWPSSK)`: machine-bound, A to Z, and the same every time that machine asks
  (`cd_key_activations` keeps one row per key and machine). The client's own leg (the mix
  down under its fingerprint, the registry chain decoded) gives back the wire key.
- **The hand-off.** A DFX2 client sets `NWCDKIID` (the wire key its boot read decoded,
  empty when unactivated) and `NWPSSK` (its 23-letter fingerprint) as browser variables
  when it connects to NovaWorld, so every web request carries them as cookies
  [orig: CNapiGameSession_OnNovaWorldConnected @0x4d18dd, @0x4d1914 (dfx2.exe)].
  `GET /NWStart.dll` answers a DFX2 client whose `NWCDKIID` is no wire key with the start
  page's `ACT` page (`dfx2_0_actlogin.htm`, "Please login to activate this product")
  instead of `OUT`. Its completed login (`GET /NWLogin.dll`) then carries
  `Set-Cookie: NWACT=<value>` for the posted `NWPSSK`: the response's `SET-COOKIE`
  lines land in the browser's one cookie jar, and the NovaWorld screen's idle update reads
  `NWACT` from it, writes it to the registry `CDKIID`, validates it, and clears it [orig:
  HTTP_ResponseHandler @0x6e6925, CookieJar_UpdateFromURL @0x6dc9e0;
  @0x557539..0x55757e (dfx2.exe), CDKey_WriteActivationFromNWACT @0x411e30 (dfx2.exe)].
  The registry write needs an administrator. An activated client, a JO client and a
  client posting no `NWPSSK` get no `NWACT`.
- **Minting.** An account that has never held a DFX2 key gets one minted onto it at its
  first activation (`created_by` `auto`) while `ONNET_CD_KEY_AUTO_MINT` is on (the
  default; `0` turns it off, and then only a key an admin assigned activates). An account
  whose keys were all revoked gets none on its own.
- **The admin routes** (each `require_admin`): `GET /api/admin/cd-keys` (`?title=`,
  `?state=` unassigned / assigned / revoked, `?q=` a part of the serial, wire key or
  username, `?limit=` 1 to 500, `?offset=`), `POST /api/admin/cd-keys` `{"title",
  "count"}` (1 to 100), `POST /api/admin/cd-keys/<id>/assign` `{"username"}` or
  `{"user_id"}`, `POST /api/admin/cd-keys/<id>/revoke`, `GET
  /api/admin/cd-keys/<id>/activations`, `POST /api/admin/cd-keys/<id>/activations`
  `{"nwpssk"}` (a manual activation: the NWACT for that machine, recorded) and `GET
  /api/admin/users/<id>/cd-keys` (an account's keys and machines). The site's admin page
  is `/admin/cd-keys`.

Tests: `cdkey` (the codec, the RE record's section 9.7 vectors, the LAN join codes),
`cd_keys` (the keys and activations on a migrated database of their own),
`backend_schema` (the tables and their indexes as the baseline makes them), and the route harness's
`cd_key_routes` and `dfx2_activation` cases (the admin routes, and a DFX2 client's whole
activation through `NWStart.dll` and `NWLogin.dll`, its NWACT read back to the account's
wire key).

## Infrastructure (the admin's view)

The site's admin page **Infrastructure** (`/admin/infrastructure`) shows the deployment as
it runs, read on web (`apps/novaworld_web/infra/`) in one route. From the database, which
every replica reads alike: the lobby instances (`lobby_instances`, each with its session
rows and listings, live when seen in the last 15 s), the controller that leads
(`controller_leader`) and the fleet's machines (`fleet_machines`, the live ones then the
newest ten ended; the fleet's own page, `/admin/fleet`, acts on them). From AWS, with web's
task role (`infra/infra_status.*`): each program's ECS service (its desired, running and
pending counts, its deployments and their rollout state, its task definition and whether
that is the revision the recorded release names, its newest events), its tasks (running,
and the stopped ones ECS still lists, about an hour, with their stop reasons and exit
codes: status, health, start, zone, private address, size, each container's image and the
digest ECS pulled), the one-off tasks (the backup, the toolbox's database tasks), each of
the load balancer's target groups with its targets' health, the database's and the
cache's status (Multi-AZ, zones, class, the cache's nodes and their roles) and each
machine's zone and state as EC2 has it.

The AWS calls are curl's, signed by curl itself (`--aws-sigv4`, Signature Version 4), run
the way billing's Stripe calls are (`infra/aws_api.*`): POSTs to the JSON APIs (ECS, SSM)
and the Query APIs (Elastic Load Balancing, RDS, ElastiCache, EC2), whose XML replies
`infra/aws_xml.*` reads. Not the AWS CLI the worker runs: a CLI run spends about a second
of CPU starting its Python, and one read makes a dozen calls on web's half vCPU. The
credentials are the task role's, which the ECS agent serves each task
(`AWS_CONTAINER_CREDENTIALS_RELATIVE_URI`), read for each refresh and kept no longer; they
travel in each call's owner-only curl config file, never on a command line, and no reply or
log line holds them. One read runs its calls at once and is kept 20 s: however many admins
look, a web process reads AWS at most once in that window (a read in flight is waited for).
A call that fails leaves its section's error (AWS's own code and message: `rds answered
403: AccessDenied: ...`) and the others stand; the credentials failing stop the read.
Without AWS (the local stack: no `ONNET_INFRA_CLUSTER`, or no task role) nothing is called
and the page says why: it never shows made-up data. Web's task role holds these reads
alone, each scoped to the deployment's own resources where the API takes one
(`infra/aws/programs.tf`, `infra_read`; the role boundary holds the same). The page polls
every 30 s.

| Route | Auth | Reply |
| --- | --- | --- |
| `GET /api/admin/infrastructure` | admin | `{"at"` (the database's now), `"cloud"`, `"lobbies"`, `"controller"`, `"hosting"` (`enabled`, `provider`), `"machines"}`. `cloud`: `{"configured": false, "unavailable"}` (why AWS is not read here), or `configured` true with `read_at`, `cluster`, `region`, `error` (the read failed as a whole), `release` (`id`, `created`, `revision`; null with none), `release_none` (none recorded yet), `services` (each `name`, `status`, `desired`, `running`, `pending`, `task_definition` (`<family>:<revision>`), `on_release` (true, false, or null when the release names none), `deployments` (`id`, `status`, `rollout_state`, `rollout_reason`, `task_definition`, `desired`, `running`, `pending`, `failed`, `created_at`, `updated_at`), `events` (the newest ten: `at`, `message`), `tasks` and `stopped_tasks` (the newest three)), `missing_services`, `one_off_tasks`, `one_off_stopped_tasks`, `target_groups` (`key`: gate, lobby, http or https; `name`; `targets`: `id`, `port`, `zone`, `state`, `reason`, `description`; `error`), `database` (`identifier`, `status`, `engine`, `engine_version`, `instance_class`, `multi_az`, `zone`, `secondary_zone`, `storage_type`, `storage_gb`), `cache` (`id`, `status`, `multi_az`, `automatic_failover`, `node_type`, `engine`, `nodes`: `id`, `role`, `zone`), and each section's error (`services_error`, `tasks_error`, `release_error`, `database_error`, `cache_error`, `machines_error`; null when it read). A task: `id`, `program` (null for a one-off), `group`, `task_definition`, `on_release`, `last_status`, `desired_status`, `health`, `created_at`, `started_at`, `stopping_at`, `stopped_at`, `stopped_reason`, `stop_code`, `zone`, `private_ip`, `cpu` (units), `memory` (MiB), `containers` (`name`, `image`, `image_digest`, `last_status`, `health`, `exit_code`, `reason`). A lobby: `id`, `endpoint`, `state`, `started_at`, `last_seen_at`, `seen_ago_seconds`, `live`, `sessions`, `listings`. `controller`: `leader`, `generation`, `elected_at`. A machine: `id`, `provider`, `kind`, `state`, `slots`, `slots_used`, `cloud_id`, `instance_type`, `cloud_state` (the worker's last describe), `launched_at`, `ready_at`, `ended_at`, `end_reason`, `zone` and `ec2_state` (EC2's; null when AWS was not read or does not list it). Times ISO 8601 UTC; 500 `db_error`; `Cache-Control: no-store` |

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_INFRA_CLUSTER` | none | Web: the deployment's ECS cluster, whose services (the programs) and tasks the view reads. Unset (the local stack), AWS is not read. |
| `ONNET_INFRA_REGION` | none | Web: the region of every AWS call the view makes. |
| `ONNET_INFRA_TARGET_GROUPS` | none | Web: the load balancer's target groups, a JSON object of listener to target group ARN (`{"gate": "arn:...", "lobby": ..., "http": ..., "https": ...}`). |
| `ONNET_INFRA_DATABASE` | none | Web: the RDS instance's identifier. |
| `ONNET_INFRA_CACHE` | none | Web: the ElastiCache replication group's id. |
| `ONNET_INFRA_RELEASE_PARAMETER` | none | Web: the SSM parameter holding the release (`/opennova/<deployment>/release`), whose revisions say whether each program runs the release's. |

## The switcher's address

`GET /api/server-info` answers `{"novaworld_ip": "<ONNET_PUBLIC_HOST>"}` with
`Cache-Control: no-store`: the address the gate advertises as `POSTIPADDRESS`, which
the OpenNova Switcher ([`switcher/`](../../switcher/README.md), ADR 0054) writes into a
player's hosts file for `gs.novaworld.net`. The site's domain is behind Cloudflare's
proxy, so DNS cannot give that address. The route is public and read-only (the address
is public anyway: the gate hands it to every client); it answers 503
`{"error":"no_public_ipv4"}` when `ONNET_PUBLIC_HOST` is not a dotted quad (a hosts line
names an address) or is one no client can be sent to (0.0.0.0/8, multicast,
240.0.0.0/4). In prod it is reached as `https://<domain>/api/server-info` through the
portal's nginx, like every `/api` route. The route harness's `server_info` case pins both
answers.

## Configuration

All via environment (`server_config.cpp`): `ONNET_WEB_HOST`, `ONNET_LOBBY_ENDPOINT`,
`ONNET_INTERNAL_URL`, `ONNET_HEALTH_PORT` ("Programs" above), `ONNET_PUBLIC_HOST`,
`ONNET_{GATE_UDP,NW_UDP,HTTP}_PORT`, `DATABASE_URL`, `DATABASE_PASSWORD`,
`DATABASE_POOL_MAX` ("Database" below), `ONNET_REDIS_URL`, `ONNET_REDIS_PASSWORD`
("Redis" below), `MIGRATIONS_DIR`, `SEED_DIR`,
`TEMPLATES_DIR`, `STATIC_DIR`, `WEB_DIST_DIR`,
`TICK_INTERVAL_MS`, `HOST_SWEEP_INTERVAL_MS`, `HOST_STALE_WINDOW_MS`,
`ADMIN_API_TOKEN`. Gate extras (all optional): the MET endpoint
`ONNET_MET_IP`, `ONNET_MET_PORT`, `ONNET_MET_LABEL`, `ONNET_MET_PING`,
`ONNET_MET_EXT` (emitted as the METIPADDRESS family when `ONNET_MET_IP` is set)
and the GLSVSS leg `ONNET_GLSVSS_REQUEST`, `ONNET_GLSVSS_RIMS`,
`ONNET_GLSVSS_AGRMS` (the gate VARs for a games row with no GLSVSS leg of its own;
DFX2's row has one) plus `ONNET_GLSVSS_RESULTS` (what
`ClientGLSVSSRequest` is answered with). The gate response carries only the
nineteen keys the retail parser recognises. The host/join reflection override
for a docker bridge or NAT (leave unset in production): `ONNET_CLIENT_REFLECT_IP`,
`ONNET_CLIENT_REFLECT_{GATE,NOVAWORLD}_PORT`. The listing policy:
`ONNET_REQUIRE_HOST_KEY=1` lists no host whose HostKey names no account (default off:
listed with no owner; "Listed servers and owners" above).
`ONNET_CD_KEY_AUTO_MINT=0` stops minting a DFX2 account's first CD key at its first
activation (default on; "CD keys (DFX2 activation)" above).
`ONNET_EMBEDDED_JO_HOST=1` runs an in-match JO host in the service process on the
NovaWorld UDP port, which serves a Hello carrying JO's complete in-match identity as a
game session; the `nw_host_register_e2e` harness turns it on (default off: the service
runs no game host).
Billing takes the `STRIPE_*` set and `ONNET_SITE_URL`, `ONNET_STRIPE_CURL`,
`ONNET_BILLING_AUTOMATIC_TAX` and `ONNET_BILLING_RECONCILE_MINUTES` ("Billing (monthly
support tiers)" above). Outbound mail takes `ONNET_MAIL_FROM`, `ONNET_MAIL_REGION` and
`ONNET_SITE_URL` ("Account email and password resets" below). The job workers take
`ONNET_WORKER_THREADS` ("Background jobs" above).
Seeding: `SEED_DEV_USERS=1` applies the dev-only `0002_dev_users.sql` (the
`test`/`foo` accounts) — leave unset in production. Build + run via Docker:
[`Dockerfile`](Dockerfile) (one target per program) and the compose files under
[`deploy/`](../../deploy/). What each program reads of these is its own; a setting a
program does not use is ignored.

The website's sessions (next section) take four more:

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_COOKIE_SECURE` | off | `1` makes the session cookie `__Host-opennova_session` with `Secure`. Set it wherever browsers reach the site over https: prod sits behind Cloudflare's TLS while nginx and web speak plain http, so no request header can tell (web's ECS task definition sets it, `infra/aws/programs.tf`). Leave it off for the http://localhost dev site, where a Secure cookie is never sent back; the cookie is then `opennova_session`. |
| `ONNET_TRUSTED_PROXIES` | none | Comma-separated exact addresses of the reverse proxies whose `X-Real-IP` (else the last `X-Forwarded-For` hop) names the client, for the website's rate limits and the session's recorded address. Prod's nginx (`web/nginx.conf`, the portal container in web's ECS task) reaches web from `127.0.0.1`; behind Cloudflare it takes the visitor's address from `CF-Connecting-IP` for Cloudflare's own ranges only (`web/cloudflare-realip.conf`, regenerated by `web/cloudflare-realip.sh`). That header is not proof: a Cloudflare Worker on any zone can fetch the origin with a value of its choosing until edge authentication is on (DEPLOY.md), so these addresses brake only the website's routes, never the retail login. Unset, the TCP peer is the client and those headers are ignored. |
| `ONNET_RETAIL_PROXY` | none | The exact address the retail HTTP port's proxy reaches web from: prod's portal nginx runs a retail server (`web/nginx.conf`, port 8082, where the balancer's retail listener lands) that refuses a request body past 16 KiB (413) before web buffers any of it and forwards the rest from `127.0.0.2`, naming the client (its own TCP peer, whose address the balancer keeps) in `X-Real-IP`. Web takes that header as the request's address before any check or route reads it, so the retail routes, their cookies, the login brake and every record see the client as they would straight on the port. It is no trusted proxy: the retail port's `/api` is refused as any client's is. Unset, no such proxy. |
| `ONNET_API_PROXY_ONLY` | off | `1` answers every `/api` request (`/api` and everything under it, a route or not) with 403 `{"error":"proxy_required"}` unless its TCP peer is one of `ONNET_TRUSTED_PROXIES`, so the website's API is reached only through the proxy that applies the edge checks, never straight on the HTTP port. The retail routes a stock client reaches on that port directly (the `NW*.dll` pages, the `.gsb` feeds, the templates and static files) stay open. Prod sets it (web's ECS task definition): its nginx reaches web from `127.0.0.1`, the one trusted proxy. With it on and no trusted proxy, every `/api` request is refused (logged at boot). Leave it off for the dev site. |
| `ONNET_BOOTSTRAP_ADMIN` | none | An existing account's username, given the `admin` role at boot while no account is an admin; with an admin present it does nothing (logged either way, a name no account has is a warning). How the first site admin is made; later ones through `PUT /api/admin/users/<id>` `{"role":"admin"}`. Pass it through the vault-injected secrets file, never a committed one, and remove it once it ran (DEPLOY.md). |

## Database

PostgreSQL 17, reached through libpq
([`engine/net/novaworld/db/database.h`](../../engine/net/novaworld/db/database.h), the
`opennova_pq` target the service alone links). In prod it is a managed instance (RDS);
locally, the dev compose stack's `postgres` service or `scripts/dev_postgres.sh up`.

| Setting | Default | Meaning |
| --- | --- | --- |
| `DATABASE_URL` | `postgresql://novaworld@127.0.0.1:5432/novaworld` | A libpq connection URI: host, port, role, database and options. Against RDS it carries `sslmode=verify-full&sslrootcert=/app/rds-global-bundle.pem`, the CA bundle every image pins ([`Dockerfile`](Dockerfile)). Not a secret: the boot log prints it, a password in it masked. |
| `DATABASE_PASSWORD` | none | The role's password, kept apart from the URI because it is a secret (the vault's). Unset, libpq's own sources apply (a password in the URI, `~/.pgpass`). |
| `DATABASE_POOL_MAX` | `16` | The most connections the server opens. A thread that needs one past them waits up to 30 s for a lease to come back, then its request fails `db_error`. A deployment's infra.env sets it (`db_pool_max`, 7: `infra/aws/database.tf` sizes the database's `max_connections` for it). |

- **The database itself** is made with the SQL_ASCII encoding and the C locale
  (`CREATE DATABASE ... TEMPLATE template0 ENCODING 'SQL_ASCII' LC_COLLATE 'C'
  LC_CTYPE 'C'`), and the server refuses to migrate any other: a retail client's text
  (a server name, a roster, a player name) arrives in its code page and is stored and
  handed back byte for byte, and text sorts byte by byte ([`backend/README.md`](../../backend/README.md)).
- **Each connection** runs in UTC (a time reads `YYYY-MM-DD HH:MM:SS`, as the JSON
  routes send it) with a 5 s lock timeout, a 15 s statement timeout and a 60 s limit on
  sitting idle inside a transaction. A connection the network or the server dropped is
  reopened before its next statement when it held no transaction; a failed statement is
  never retried.
- **Narrow locks, no writer lock.** A `db::Transaction` is `READ COMMITTED` and takes
  no lock of its own, so transactions on other connections and processes run beside
  it, and the lobby's and the gate's writes never queue behind an unrelated web
  write. A check and the write it decides take the lock their invariant needs, across
  processes too ([`database.h`](../../engine/net/novaworld/db/database.h) lists the
  advisory keys):
  - a start request (`hosted::request_server`): the hosted-start advisory lock, from
    its first read to its commit (the account's servers, the global cap and the
    fleet's room are counted by one start at a time); a job's claim and the
    controller's election hold keys of their own, so neither waits on a start;
  - a website session, a hosting key or a known address minted at a credentials
    generation: the account's row (`FOR NO KEY UPDATE`), so a password reset or a ban
    runs wholly before the mint (which then mints nothing) or wholly after (and
    deletes what it minted);
  - the status blob's read-modify-write: the host's row (`FOR NO KEY UPDATE`), which
    the lobby's own write of the row and its roster locks first too;
  - a username or handle a registration, an admin's edit or a service account
    writes: an advisory lock per name (`hashtext(lower(name))`), taken for each name
    it checks or writes, since a username may equal no account's username or handle
    and no unique index spans the two columns;
  - the first admin (`bootstrap_admin`): the migration lock;
  - a PCID, an email, a CD key's assignment and an account's one live key per title,
    a machine's one activation: the unique indexes, whose `23505` maps to the
    caller's own answer (`pcid_exists`, `AccountHasKey`, a reactivation), and the
    key's compare-and-set, whose lost race reads the key again.
  A lock wait past the lock timeout fails the write (`55P03`), and of two
  transactions waiting on each other the server ends one (`40P01`); the heartbeat
  counts both. Nothing slow runs inside a transaction (bcrypt, the AWS CLI, Stripe):
  its locks are held to its end. A `db::ReadSnapshot` is `REPEATABLE READ, READ
  ONLY`: its reads see one snapshot and take no lock.
- **Migrations.** Each boot applies the `backend/migrations` files (`MIGRATIONS_DIR`)
  that `_schema_migrations` does not name yet, each in its own transaction, recording
  its SHA-256, then the seeds (`SEED_DIR`), all under a second advisory lock, so two
  servers booting at once apply each file once. Migrations are append-only: a recorded
  file whose SHA-256 has changed stops the boot, so a fix is a new file
  ([`backend/README.md`](../../backend/README.md)). Until the cutover nothing has applied
  the program's new migrations in prod, so they may still be edited in place; a
  development database that applied an older text is recreated
  (`scripts/dev_postgres.sh reset`; on a deployment `deploy/bin/on-deploy db reset-db`,
  which takes a `pg_dump` first and refuses while any support tier subscription grants).

## Redis

The HTTP layer's shared state, through hiredis
([`redis_client.h`](redis_client.h): `RedisPool`, the `opennova_redis` target only this
shared library links). In prod it is ElastiCache for Valkey over TLS with an AUTH
password; locally `scripts/dev_redis.sh up` (Valkey 8). The HTTP listener needs it: a
web process that cannot reach it at boot (a `PING`) logs `[boot] FATAL: redis
unreachable` and exits 1 before any listener starts. No other program opens it.

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_REDIS_URL` | `redis://127.0.0.1:6379` | `redis://[user[:password]@]host[:port][/db]`, or `rediss://` for TLS (the server's certificate checked against the system's CAs and its name against the host). Not a secret: the boot log prints it, a password in it masked. |
| `ONNET_REDIS_PASSWORD` | none | The AUTH password (ElastiCache's auth token), kept apart from the URL because it is a secret (the vault's); registered as a log secret. |

- **What it holds, every key with a TTL** (all under `nw:`):
  `login:` / `join:` / `host:<tag>`, the retail `NW*.dll` dance's sessions
  (`session_store.*`), 5 minutes from their write; `/NWHost.dll`'s second leg takes
  its session with `GETDEL`, so a HOSTKEY is served once across every replica.
  `pin:<cookie>` and `pins:<user_id>`, the retail login pins and each account's index
  of them (`retail_pins.*`), 24 hours from their last use (the login, or a request
  that reads the pin, which moves both out again); an account's revocation deletes
  its pins. `rl:<limiter>:<key>`, the rate limiters' buckets, each until it would be
  full again. Nothing with an invariant lives here: accounts, listings, sessions of
  the website and everything durable stay in PostgreSQL.
- **Each command** waits at most 2 s for its reply (5 s for a pooled connection, at
  most 16 open), then the request fails. A connection the server dropped while it
  sat idle (a failover, a restart) is opened again, and the command that found it
  dropped is sent once more only when a second run does no harm (a read, a `SET`, a
  `DEL`, a rate bucket's draw): a take (`/NWHost.dll`'s `GETDEL`, a logout's, the
  rotation edit's `SET NX`) fails its request instead, as the drop may have met the
  reply of a command the server ran. A command that timed out is never sent again.
  The server ignores `SIGPIPE`, so a write to a closed connection fails rather than
  ending the process.
- **The server's settings.** No persistence is needed (a restart loses only
  sessions minutes old, pins a login makes again and spent buckets); a memory limit
  with `maxmemory-policy volatile-lru` (every key has a TTL), never `noeviction`,
  which would fail every write once full. Cluster mode off: a pin and its account's
  index are written by one script, so they must share a node.

## Website sessions and roles

The NovaWorld site logs players in with their NovaWorld account (`web_access.*`,
`web_session.*`, `rate_limiter.*`, `http_json.h`; the tables `web_sessions` and
`login_addresses`, and `players.role` and `players.credentials_gen`). The retail
`NW*.dll` login keeps its own flow and shares only the login brake's per-username cap,
the known addresses and the credentials generation below.

- **Routes.** `POST /api/login` `{username, password}` answers `{id, username, role}`
  and sets the session cookie; `POST /api/logout` ends the session and clears the
  cookie (204, also with no live session); `GET /api/me` answers `{id, username, role}`
  or 401. A body that is not a JSON object is 400 `invalid_json`, a field of the wrong
  type 400 `invalid_field` (also on every admin route that takes a body).
- **Session.** 32 bytes from the OS CSPRNG, the cookie `opennova_session=<64 hex>;
  Path=/; Max-Age=2592000; HttpOnly; SameSite=Lax`, or with `ONNET_COOKIE_SECURE`
  `__Host-opennova_session=...; Secure` (the `__Host-` prefix makes the browser refuse
  the cookie from anywhere but this host, so a sibling subdomain cannot plant one; the
  server reads only the name its config sets). `web_sessions` stores only the token's
  SHA-256 with the account, the client address and the user agent; an account keeps
  its newest 20 sessions, and a login past them ends the oldest. A session lasts 30
  days from its last use: a use more than a minute after the last recorded one moves
  the expiry forward and renews the cookie on that reply (any route), so a busy
  session writes at most once a minute. The periodic `web_sessions.prune` job prunes
  expired rows.
- **Revocation.** A deleted player takes its sessions and its known login addresses.
  An admin password reset or a status other than `active` (`PUT
  /api/admin/users/<id>`) moves the account's credentials generation
  (`players.credentials_gen`) and deletes its website sessions, its
  retail login's `active_user_sessions` row and its hosting keys in the same
  transaction (a server it listed loses its owner at its next refresh; "Listed servers
  and owners"), and the HTTP layer drops the retail login's
  `PERSISTENTEXPRESSLOGINDATA` pins to the account from Redis, for every replica
  (which would resume a retail login with no password), so a reset locks out whoever
  held the old one and a lifted ban revives nothing. The known login addresses stay: forgetting them would bare the
  owner to the per-username cap just when someone else is guessing, and the
  per-(username, address) bucket still brakes a guesser at one of them. A session
  whose account went inactive any other way is refused and deleted when next used. An
  empty status is refused, by the update (400 `invalid_field`) and by the schema (a
  `CHECK` on `players.account_status`), so `active` alone is active everywhere.
- **The credentials generation.** A login reads the account's generation with its
  password hash, before bcrypt runs, and everything it then grants is written only
  while the account is still at it, tested by the insert itself: the website session
  (also only while the account is active), the retail login's `active_user_sessions`
  row (likewise) and the known address. So a reset or a ban that commits while a
  password is being checked lets that login grant nothing: the site answers the
  generic 401, the game login a bad password's page. The retail login's session row
  and its pin carry the generation they were made under and count only while the
  account still has it, so a reset ends them even where nothing deletes them.
- **Usernames and handles.** A username: not empty, at most 64 bytes, no control byte;
  a handle (`nwhandle`, the player's name in the game and the `NWHANDLE` / `CHAR`
  cookies): not empty, at most 30 bytes, no control byte, the longest a stock host
  reads back whole (`auth.h`, `kHandleMaxBytes`). On registration (whose handle
  defaults to the username), the admin create and every admin update (400
  `invalid_field`), so every account can log in to the site and no stored name carries
  a byte into a log line, a cookie or the retail browser. Every client-supplied string
  in a log line of any listener (usernames, handles, URLs, query and form values,
  cookies, tags, ServerCommand lines, the names a NovaWorld session or gate probe
  carries, engine log messages) goes through `loggable()` (`loggable.h`): control
  bytes as `\xNN`, cut at 64 bytes.
- **Retail identity.** `/NWJoin.dll` (the joiner whose PCID and handle its PUB* cookies
  carry, and the expansion gate) and `/NWAccount.dll` / `/NWCharacter.dll` take the
  account from server-side state only: the request's own retail login, its
  `LOGINSESSIONTAG` (the login's `active_user_sessions` row), else the
  `PERSISTENTEXPRESSLOGINDATA` pin a login made. Never the `NWHANDLE` cookie: the
  service sets it in the clear to the account's handle, so any client could send any
  account's. A joiner whose account is not active is refused with the failure page a
  missing expansion gets (the `msgbase` page, NWEC11 for a ban, NWEC12 otherwise); one
  with no login standing joins with empty PUB* cookies, which a stock host refuses.
- **Retail pages.** Every value the routes fill a page with (`{{VAR}}`,
  `template_engine.*`) goes out with `& < > " '` as entities, so a request's `IN` /
  `OUT` / `MSGBASE`, a listed server's name and a maintenance message add no markup. In
  a page's text a stock client's page parser turns the entities back into the
  characters, so a server name or a message shows as itself; inside a tag (the login
  form's `VALUE="{{IN}}"`) it decodes none, which costs nothing because the values
  there are bare page names. The values the service and a stock client use hold none
  of the five and go out byte for byte.
- **Login answers.** An unknown username and a wrong password get the same 401
  `invalid_credentials`, and an unknown name still costs one bcrypt run, so neither the
  body nor the timing enumerates accounts. Only the right password learns that an
  account is banned (403 `account_banned`) or otherwise inactive (403
  `account_restricted`), the game login's NWEC11 / NWEC12. A body that is not a JSON
  object, a missing or mistyped field and a username over 64 bytes are 400s before any
  brake. A new login ends the session the browser held. Maintenance mode does not
  close the site login (an admin turns it off from there).
- **CSRF.** Every state-changing request (any method but GET, HEAD, OPTIONS) that a
  session cookie authenticates needs `X-OpenNova-Request: 1`, else 403
  `csrf_header_required`, and so do `POST /api/login`, `/api/logout` and
  `/api/register`. A cross-site form cannot set the header, and a cross-origin script
  that sets one needs a CORS preflight the server never grants (it sends no
  `Access-Control-*` header). `SameSite=Lax` already keeps the cookie off cross-site
  POSTs; the header also covers same-site origins such as `staging.<domain>` and the
  routes no cookie authenticates: a forged login would sign the visitor into the
  forger's account, a forged logout sign them out, and a forged registration (a
  `text/plain` form can carry a JSON body) create accounts from visitors' addresses.
  The header is checked before any rate-limit bucket, so forged requests drain none.
  The web client sends it on every request (`web/src/api/client.ts`).
- **The login brake** (token buckets in Redis, `rate_limiter.*`: one hash per key, read,
  refilled and written back by one script, so every replica draws on the same bucket
  and a limit holds exactly across them; a bucket's key expires when it would be full
  again, and Redis's memory limit with `volatile-lru` bounds a flood of keys), run by `POST /api/login` and the retail `POST /NWLogin.dll`. The two keep
  their address-keyed buckets apart: the site's key on the client address its trusted
  proxy names, which a Cloudflare Worker can forge until edge authentication is on;
  the retail login's on its TCP peer, or the client address the retail port's own
  proxy names (`ONNET_RETAIL_PROXY`: the portal nginx's retail server, which takes it
  from its own TCP peer and believes no header; the gate's `startupurl` names
  `ONNET_PUBLIC_HOST` and the port), so nothing the site sees can brake a retail login. An IPv6 address counts as its
  /64 (its /56 in the (username, address) buckets).
  - every well-formed attempt, per route and client address: 20 at once, then one
    every 3 s;
  - each password check, per route, username and client address: 10, then one every
    30 s, so a guesser's address is shut out while the owner elsewhere never meets it;
  - each password check per username, from any address and either route: 30, then one
    every 2 s, except from the account's known addresses (a right password from them
    in the last 30 days, `login_addresses`, at most 32 an account).
  A check takes its tokens before bcrypt runs and gets them back when the password is
  right, so the caps are exact however many checks run at once and a right password
  costs nothing. A guesser gets at most 30 tries a minute at one account from
  addresses it has not logged in from (43,200 a day), and 10 then 2 a minute from any
  one address (/56); holding the per-username cap empty takes about 15 addresses
  (/56s) failing in step, and even then locks out only logins from addresses the
  account has not used, the owner's own first login from a new address included.
  `/api/login` refuses with 429
  `rate_limited` and `Retry-After` in seconds. `/NWLogin.dll` refuses with its own
  failure reply, the msgbase page (`jop_2_msg.htm`) a stock client shows for a bad
  password, carrying "Too many failed logins. Please try again later." (no retail
  throttling reply is witnessed). `POST /api/register` takes 10 then one every 3 min
  per address (/64).
- **Roles and admin routes.** `players.role` is `player` (every account) or `admin`.
  Every `/api/admin/*` route, and `GET /api/hosts` and `GET /api/unknowns` (they name
  players' and senders' addresses), accepts the Bearer `ADMIN_API_TOKEN` (machine
  callers and the deploy toolbox; no CSRF header) or an admin-role session (with the
  CSRF header on a state change); a player's session gets 403 `forbidden`, no credential 401
  `{"error":"unauthorized"}` with the Bearer challenge. The role is read on every
  request, so a demotion takes effect on the live session. Each admin state change it
  lets through is logged once its handler has answered, with the actor (`token`, or the
  session's username) and the reply's status. The last admin
  may demote themselves: the Bearer token can always promote again, and with no admin
  left `ONNET_BOOTSTRAP_ADMIN` works once more.

## Account email and password resets

Every account a person registers has an email address (`players.email`,
`backend/migrations/0005_account_email.sql`): the welcome mail goes there, and so do the
password reset links. Service policy, not a port: retail's NovaWorld accounts had no
email the service kept.

- **The address.** `POST /api/register` requires `email` (400 `missing_field` without
  it). `normalize_email` (`auth.h`) takes at most 254 bytes with exactly one `@`, a local
  part, and a domain of at least two dot-separated labels, with no space or control byte
  (400 `invalid_field` otherwise), and stores it with its ASCII letters lowercased, so one
  address names one account however it was typed: a unique index refuses a second one
  (400 `email_exists`), and a `CHECK` refuses an upper-case or empty one. The admin's
  `POST /api/admin/users` takes an optional `email` under the same rules. Accounts older
  than the column, service accounts and an admin's accounts made without one have none
  (`NULL`); nothing is backfilled. Every admin user route answers `email` (null for
  none), read-only: no route changes it.
- **The welcome mail.** Once `POST /api/register` has made the account, it posts the
  welcome mail (who we are, the account's name, a link to the site). A mail that does not
  go out is logged, never the registration's failure.
- **`POST /api/auth/forgot`** `{email}`: the CSRF header first, then 10 per client
  address (/64) and one every 3 min, the body (400 `invalid_json`, `invalid_field`,
  `missing_field`), then 3 per email address and one every 20 min, counted whether or not
  an account has it (429 `rate_limited` with `Retry-After` past either). Then 202 with no
  body, the same for every address: when an active player's account has it, a reset link
  is minted (32 bytes from the OS CSPRNG as 64 hex digits; `password_resets` keeps its
  SHA-256 alone, an hour, one per account, so a newer request replaces the older link) and
  mailed to the address as `https://<ONNET_SITE_URL>/reset?token=<token>`. The mail goes
  out off the request's thread, so the reply's timing does not tell either.
- **`POST /api/auth/reset`** `{token, password}`: the CSRF header, 10 per client address
  and one every 3 min, the body; then the token is spent (its row deleted) before the
  password is hashed, so a link works once and a wrong token costs no bcrypt run: 400
  `invalid_token` for one that is malformed, unknown, spent or past its hour. The password
  is then set as an admin reset sets it (`update_user`): the credentials generation moves,
  and the account's website sessions, its retail login's session row, its hosting keys and
  its retail pins end. 204. The periodic `password_resets.prune` job prunes links past
  their hour.
- **The mailer** (`mailer.h`). The routes queue each mail as a `mail.send` job
  ("Background jobs" above) on their own connection, so a mail outlives a restart of the
  process that asked for it: the payload names the template and the recipient, the
  subject and the bodies are the job's secret, gone from the row once it ends. A worker
  sends it, at most once (one SES refused fails its job, never tried again), and logs it
  as one line: `[mail] sent <template> to '<address>' (message id ...)`, or `[mail] WARN
  <template> to '<address>' not sent: <why>`, never the body. A mail the queue cannot
  take is logged and dropped, never the request's failure. The
  `SesMailer` runs the AWS CLI's `sesv2 send-email` through the command runner, as the EC2
  machine driver does, on the worker's task role: the request (from `OpenNova
  <ONNET_MAIL_FROM>`, the recipient, the subject, a plain-text and a minimal HTML body) in
  a JSON file of its own, owner-only in an owner-only scratch directory and deleted once
  the CLI ends, so no address or link is on a command line. Each template's text names the
  account and carries its link; the HTML escapes the name. Infrastructure: the domain's
  SES identity, its DKIM and MAIL FROM records and the worker's task role's permission
  (`infra/aws/mail.tf`; DEPLOY.md, "Outbound mail (SES)"). Web sends none, so its task role
  holds none.

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_MAIL_FROM` | none | The sender's address, in lower case (`support@<domain>`; infra.env names it). Unset, mail is off: each mail is logged as not sent. |
| `ONNET_MAIL_REGION` | none | Amazon SES's region (infra.env's). |
| `ONNET_SITE_URL` | none | The site's origin the mails link to (billing reads it too). Mail is on only with all three; a sender without a site URL or a region boots with mail off and a warning naming why. |

## Build layout and tests

This directory builds the static library `opennova_novaworld_common`, what more than
one program uses; each program's directory builds its own code as
`opennova_novaworld_<program>_core` (gate, lobby, web, controller; mirroring
`opennova_serve_core`) and its `main.cpp` as `opennova_novaworld_<program>`
(`opennova-novaworld-<program>`). All of it, the service target and their tests exist
only with `OPENNOVA_NOVAWORLD_SERVICE=ON` (the root CMake option, off by default; the
Linux CI jobs and the images turn it on). The shared library's HTTP half (`auth`,
`web_session`, `template_engine`, `redis_client`, `mailer`, `password_reset`, `parts`,
the jobs, the hosted servers' `hosted/` and billing's `billing/`) builds only under
`BUILD_NOVAWORLD_HTTP`, which also defines `OPENNOVA_HTTP_ENABLED` PUBLIC; web, the
controller and the worker build only with it, the gate and the lobby with or without it.
The header-only `loggable.h`, the boot (`program`), the config, the health, the ECS
metadata and `catalog_repository` (the games catalogue, which the gate and web read)
build either way. The route harness, ctest
`opennova_novaworld_web_http_routes`
([`tests/novaworld/http_routes_test.cpp`](../../tests/novaworld/http_routes_test.cpp)),
drives the Crow listener in-process on port 0 (the port it reads back from
`bound_port()`, and the port a rendered page's `HOST_URL` / `GSB_SERVER` carry)
over a `db::ConnectionPool`
on a database of its own under `backend/migrations`, with `apps/common`'s `http_exchange`. It exists only
with `BUILD_NOVAWORLD_HTTP=ON` and runs in `net-linux.yml` and `ci.yml`'s
test-linux. Every test that opens a database (ctest label `pg`, registered through
`opennova_add_pg_test` in
[`tests/novaworld/service_tests.cmake`](../../tests/novaworld/service_tests.cmake))
makes its databases on the PostgreSQL server `-DOPENNOVA_TEST_PG=<uri>` names, one per
case (`nwt_<pid>_<n>`, made SQL_ASCII with the C locale and dropped after it;
[`tests/common/test_database.h`](../../tests/common/test_database.h)); with none it prints
`SKIP: needs a PostgreSQL server` and exits 77, which `--suite core` counts as a failure.
`scripts/dev_postgres.sh up` prints the value for a local server, and the Linux CI jobs
run one beside the build. The HTTP layer's tests (ctest label `redis`: the route
harness, the service e2e tests, `rate_limiter`, `redis_state`, the signal shutdown
test) also need the Redis server `-DOPENNOVA_TEST_REDIS=<url>` names, each listener in
a key namespace of its own (`nwt_<pid>_<n>:`, its keys deleted after it;
[`tests/common/test_redis.h`](../../tests/common/test_redis.h)), and skip the same way
without one (`SKIP: needs a Redis server`); `scripts/dev_redis.sh up` prints the value,
and the Linux CI jobs run `valkey/valkey:8` beside PostgreSQL. `redis_state`
([`tests/novaworld/redis_state_test.cpp`](../../tests/novaworld/redis_state_test.cpp))
drives the client and the stores as two replicas see them: the URL and connect errors,
the reconnect after the server drops a connection and which commands go again on it (a
take never, even when a server read it and closed unanswered), the retail sessions
written by one replica and read by the other with their TTL, a host key taken once by
threads racing on both, a pin's expiry moved out by each read, and the pins'
revocation holding across replicas. The ctest `db`
([`tests/novaworld/db_test.cpp`](../../tests/novaworld/db_test.cpp)) drives the wrapper
itself: the binds and the types they read back as, errors and their SQLSTATE, savepoints,
read snapshots, the pool's limit, transactions side by side, a row lock and its timeout, a
deadlock, the lock failures counted, a reconnect, and the
migration runner (its records, a failed file rolled back, a changed file refused). The
ctest `db_locks` ([`tests/novaworld/db_locks_test.cpp`](../../tests/novaworld/db_locks_test.cpp))
races two writers on two connections for each invariant the narrow locks hold ("Database"
above), holding the first's transaction open until the second waits on its lock: two
starts against one account's limit and against the cap, a session and a hosting key
minted around a password reset in either order, the status blob folded into a row the
lobby is writing, crossing usernames and handles and a shared PCID, two first admins,
and the CD keys' assignment, revocation, auto-mint and activation; and that the gate's
and the lobby's writes run beside an open web transaction without waiting. The five
binaries run whole as child processes (POSIX, `BUILD_NOVAWORLD_HTTP`;
`tests/novaworld/program_process.h`; ctest passes the five): the ctest
`opennova_novaworld_signal_shutdown`
([`tests/novaworld/signal_shutdown_test.cpp`](../../tests/novaworld/signal_shutdown_test.cpp))
runs each on a temp database with every port 0, sends SIGINT to a lobby and SIGTERM to
web, and expects exit 0 and the shutdown legs logged in order; the web run reads the
HTTP port from the `[http] listening on :<port>` line and holds a half-sent request open
across the signal. On Linux a third run checks that the first signal puts the default
action back and the same signal again ends a shutdown still pending, and a last one that
web whose Redis does not answer stops at boot. The ctest `opennova_novaworld_programs`
([`tests/novaworld/programs_test.cpp`](../../tests/novaworld/programs_test.cpp)) boots
each program alone: its boot line, the listeners it binds and no other, its `GET /livez`
(web's on its HTTP port); a gate or a lobby missing its settings, or web its own, a FATAL
exit 1 before the database; the gate handing out its fallback lobby with no instance and
answering nothing with neither; a gate beside a lobby handing out the lobby's configured
endpoint and the web endpoint rather than a bound port, and the lobby's SessionInit
naming that web endpoint; two lobbies at one lobby address, each its own instance (the
second's start reaping a stale instance with its listing), a gate handing out that address,
the first one's stop at once while the second serves on, and with none left and no
fallback the gate answering no one; web's `HOST_URL`, the controller elected,
the worker's pool. `nw_programs_e2e`
([`tests/novaworld/nw_programs_e2e_test.cpp`](../../tests/novaworld/nw_programs_e2e_test.cpp),
serial) runs the five as five processes on one database and one Redis: a retail-protocol
host lists through the gate (which hands it the lobby) and the lobby, and a client sits in
its lobby, an admin's push through web reaches the host; web, the worker and the gate
stopped and the controller killed, all four started again,
the listing and both lobby sessions survive (the same rows), the new gate hands out the
same lobby and a push still lands; then the controller's database connection frozen
behind a proxy: its watchdog exits it alone (70) and a push still lands; then the lobby
restarted, which alone drops its own sessions and listing. `health_listener`
([`tests/novaworld/health_listener_test.cpp`](../../tests/novaworld/health_listener_test.cpp))
the replies to each request and the listener over loopback, healthy and stalled, a slow
probe bounded; `ecs_metadata`
([`tests/novaworld/ecs_metadata_test.cpp`](../../tests/novaworld/ecs_metadata_test.cpp))
the controller's bind from a fake task metadata endpoint, and every refusal. The ctest
`gate_listener` ([`tests/novaworld/gate_listener_test.cpp`](../../tests/novaworld/gate_listener_test.cpp))
starts the real `GateListener` on port 0 over the migrated and seeded games table and
checks that its reported port answers a probe and comes back as POSTIPPORT, that
`UDPNOVAWORLD` and `STARTUPURL` carry the lobby endpoint and the web endpoint its config
names (not the NW UDP port beside it), that each
gate tag's reply carries its row's lobby (JO's and the JO demo's alias the reply this
gate always sent, byte for byte; DFX2's the live DFX2 gate's values in this gate's shape;
OpenNova's its row's), that a configured GLSVSS leg stands in for JO's and never for
DFX2's, and that a tag no row answers to gets no reply; `host_register_list` takes a
DFX2 host's lists (`DFXCATEGORY1`, `PBServer` `0`); the route harness's
`lobby_per_title` case drives
`/api/games`, each row's `.gsb` route and the game a retail page's `GSB_SERVER` names
(the gate tag, the `GT` cookie, the `NWPF` cookie, the page prefix), and
`backend_schema` the games columns as the baseline makes them. The website
sessions' store and the rate
limiter also have Crow-free ctests: `web_session`
([`tests/novaworld/web_session_test.cpp`](../../tests/novaworld/web_session_test.cpp),
the store and the roles on a migrated database of their own, on every service build)
and `rate_limiter`
([`tests/novaworld/rate_limiter_test.cpp`](../../tests/novaworld/rate_limiter_test.cpp),
`BUILD_NOVAWORLD_HTTP`: the buckets in Redis on a hand-stepped clock, their keys'
expiry, two replicas drawing on one bucket exactly, and the trusted-proxy client
address); the
route harness drives the cookie, CSRF, rate-limit and admin-credential paths over
the wire. The listing's owner: `host_key`
([`tests/novaworld/host_key_test.cpp`](../../tests/novaworld/host_key_test.cpp), every
build) drives the key store and the lobby session's links and strict refusal on a
migrated database of its own; the route harness pins `/NWHost.dll`'s recording (and its
unchanged bytes), `GET /api/me/hosts` and `GET /api/admin/hosts`; and
`nw_host_ownership_e2e`
([`tests/novaworld/nw_host_ownership_e2e_test.cpp`](../../tests/novaworld/nw_host_ownership_e2e_test.cpp),
`BUILD_NOVAWORLD_HTTP` only) runs two in-process services, one strict, against
opennova-serve's `Lister` with and without an account and hand-driven host
sessions (the NWU reconnect, a second connection presenting a listed key). Both e2e
tests build their service from
[`tests/novaworld/nw_service_harness.h`](../../tests/novaworld/nw_service_harness.h).
The owner's pushes: the route harness's `owner_host_push` case (its NW UDP listener
writes the pool, so a host's key links its row) drives the 401 / 403 / 404 / 400 /
429 answers, the same 404 for another account's RID and a free one, the push-time
re-check, the drain's re-check after a ban and the ban's `forget_owner` (the drain held
with `NwUdpListener::pause_command_drain`), the 409, "café" arriving as cp1252, the roster and
the stop against hand-driven hosts (the admin case adds the encoding refusals, an escaped
surrogate pair named, "€ ™" arriving as 0x80 and 0x99, and the byte caps at and past them);
`host_text_utf8`, a host-reported cp1252 name coming back as UTF-8 in `/api/lobbies` and
`/api/hosts` and as the host's byte in the GSB; and `nw_owner_admin_e2e`
([`tests/novaworld/nw_owner_admin_e2e_test.cpp`](../../tests/novaworld/nw_owner_admin_e2e_test.cpp),
`BUILD_NOVAWORLD_HTTP` only) lists an in-process `opennova-serve` with an account and
administers it from the website's routes: its match takes the owner's
`SetServerName` (and saves `game.cfg`) and runs its `TextChatServer`, another
account's push is a 404, and the owner's stop ends the listing. The lobby's tables:
`lobby_repository`
([`tests/novaworld/lobby_repository_test.cpp`](../../tests/novaworld/lobby_repository_test.cpp),
every service build) the lobby instances (a row each, two at one endpoint, touched, drained,
the live lobbies the gates read, an instance unseen past the old one-minute window left alone,
the stale reaped with their rows, a clean stop's delete) and the RIDs from one sequence (a held suffix
passed over past the wrap), the session rows and the lobby's sweep of its own,
the queue's answers and inserts (each command bearing its session's `lobby_id`, a
`forget_owner` one per instance), each instance's take in insert order with the expiry and
never another's, a session's delete taking its commands, and `clear_unheld` leaving every
instance's rows; `lobby_instances_e2e`
([`tests/novaworld/lobby_instances_e2e_test.cpp`](../../tests/novaworld/lobby_instances_e2e_test.cpp),
POSIX) real NW UDP listeners at one lobby address, each its own instance, and a real gate:
every client handed that address, a host on each with a RID of its own and a push reaching it
alone, `forget_owner` reaching both, one instance drained leaving the gate handing out the
address while both listings stay and both drained leaving it silent, an instance reaped while
its lobby runs registering again and writing its rows anew (a drained one, by its lobby or an
admin, registering again drained), a malformed-datagram flood leaving the commands and the
heartbeat running, a gate's failed read handing its fallback lobby for six refreshes, a
lobby's start reaping a stale instance (not one two minutes unseen) and clearing the rows no
instance holds, a lobby's sweep of a row it holds no session for, and a lobby replaced behind
the address: its host, its receive window run out by its clock, reconnects there and is
listed on the new instance; `opennova_novaworld_programs` two lobby processes at one
address, the first one stopping at once while the second serves on;
`nw_server_command_push_e2e` and
`opennova_serve_server_command_push` (each over a migrated database of its own) a session's row
from its Hello to its goodbye and pushes the test inserts on its own connection, taken and sent
by the listener's receive thread, an expired one dropped unsent; and the route harness's
`admin_sessions`, `admin_end_session` and `admin_lobbies` cases the view, the end by the
session's row id, and the instances with their drain through the tables. The
hosted servers:
`hosted_supervisor`
([`tests/novaworld/hosted_supervisor_test.cpp`](../../tests/novaworld/hosted_supervisor_test.cpp),
every build; it compiles `auth`, `web_session` and `hosted/` straight in) drives the
settings' rules and renders, the host limits and the offered data sets, the trees a start
pins and the sync carries, the service accounts, the request's checks
(the fleet's room among them) and every server transition over a fake machine driver
whose machines' agents the test plays through the supervisor's sync
([`tests/novaworld/hosted_fake_driver.h`](../../tests/novaworld/hosted_fake_driver.h),
[`hosted_fleet_rig.h`](../../tests/novaworld/hosted_fleet_rig.h)) on a hand-stepped clock:
two servers on one shared machine's slots, the own machine, the stops, any exit while
stopping, the crash restarts (an exit, the OOM kill, a failed start, a refused launch and
the launch backoff doubling to its cap, ended by a boot), their limit and the restart
window, the timeouts, the health checks, a lost machine
placing its server again with no restart counted, the owners' changes including a
withdrawn comp grant and a canceled support tier, the owners' limits (a tier lowered, past
due, the add-on cut) and their mass-stop hold, the reconciliation and the relist grace
after a restart, the console ring's cap, and the admin links following the running
servers; `hosted_fleet`
([`tests/novaworld/hosted_fleet_test.cpp`](../../tests/novaworld/hosted_fleet_test.cpp),
every build) the fleet's side over the same fake: the desired state each sync gets (the
files until reported, a stopping server's unstarted container, the drain flag, a
container answered for without files after a restart), a boot that never reports, a lost
machine terminated or ready again, an idle shared machine ending, a drain moving its
servers one at a time, an admin's end, the reconciliation of the cloud's machines with
the rows and the periodic sweep, a launch in flight spared (the rig runs the fleet's
jobs on the test's thread after
each pass, as the workers would within the second); `hosted_fleet_jobs`
([`tests/novaworld/hosted_fleet_jobs_test.cpp`](../../tests/novaworld/hosted_fleet_jobs_test.cpp),
every build) the fleet on real worker threads over the fake made slow: twenty servers
started at once launch their twenty machines in about one call's time, every pass short
while the calls run; a worker gone mid-launch has its job run again with the same machine
id, in its first run's subnet as a run again, and one machine results, booting with the
row's own token; a launch whose answer could not be written runs again, before the cloud
lists the machine its first run launched, in the subnet its row recorded, and one machine
results; and a stalled driver trips
no watchdog; `jobs` ([`tests/novaworld/jobs_test.cpp`](../../tests/novaworld/jobs_test.cpp),
every build) the queue itself: a job enqueued in a transaction going with it, the dedupe
and schedule keys, eight workers claiming at once each running a job once, a kind's cap
across workers, a lease run out putting a job back (or failing it past its attempts) and
the lapsed claim's finish changing nothing, the retries' backoff and jitter, the secret
dropped as a job ends, the periodic buckets, a claim beside a hosted start's lock, the prune
(an admin's keep among them) and the pool's keeper; `job_routes`
([`tests/novaworld/job_routes_test.cpp`](../../tests/novaworld/job_routes_test.cpp)) the
admin's jobs routes on the HTTP listener: every route refused without an admin, the
overview's counts, latency, failure groups (alike errors apart in their ids and times
together), worker processes with their leases (one run out), schedules and controller, the
list's filters (a failure key among them) and pages, a job's detail, the retry, group retry,
cancel, run-now and prune actions each refused in the wrong state, and never a job's
secret; `controller`
([`tests/novaworld/controller_test.cpp`](../../tests/novaworld/controller_test.cpp), every
build) two controllers on one database over supervisors of their own, the fleet's jobs on
worker threads and a hand-stepped clock: one leads and alone runs passes, its lock's
connection killed hands over within the standby's lock timeout (the generation moved on,
the old leader fenced), the new leader resumes a machine's boot timer and a kind's launch
backoff from their rows, a leader that stops hands over at once, a pass an election
overtook launches nothing (and an election waits for a write that holds the fence), and a
console command its controller claimed and never answered reads as not sent, then is
answered link down by the next leader's drain; `hosted_stats`
([`tests/novaworld/hosted_stats_test.cpp`](../../tests/novaworld/hosted_stats_test.cpp))
the sampler on a hand-stepped wall clock (the counter's differences and its restart, the
CPU's share, the unique players never stored, the missions, the rollup, the machines'
month, the `stats.prune` job's prune, which a sample never runs, the old ended machines
among it) and the reads over each range for one server, an account and the
fleet; `hosted_placement` the planner (and the request's
`place_after`); `hosted_fleet_repository` the fleet's rows (each kind's launch backoff
growing to its cap and cleared by a boot, the ended machines' prune among them);
`hosted_agent_route` the agent
routes on the internal listener, and the real `opennova-hostd` (its process runtime) end to
end against them; `hosted_ec2_machine_driver` the EC2 driver on a stub `CommandRunner` and the
docker driver against an engine in memory; `command_runner`
([`tests/novaworld/command_runner_test.cpp`](../../tests/novaworld/command_runner_test.cpp))
the real `ProcessCommandRunner` against itself; `hosted_net_cidr` the networks the agent
routes answer and the refusal log's throttle; `child_process`
([`tests/proc/child_process_test.cpp`](../../tests/proc/child_process_test.cpp),
every build) the child-process seam against itself; the route harness's
`hosted_server_routes` and `hosted_listing_owner` cases the routes' refusals, the comp grant,
the starts up to the entitlement (`server_limit` past it, a stopping server counting), the
own-machine add-on, another account's id answering 404, the list's order, the cap and the
stops, and the owner rule against a hand-driven host listing as a service account, its
`admin_fleet_routes` case the fleet view over fleet rows made by hand (the machines, the
servers' owners with their tiers, slots, ports and players, the `owner` and `state`
filters), one server with its machine, and the drain, end and roll moves, its
`hosted_stats_routes` case the four stats routes over hand-written samples (every key of
the reply, the ranges, another account's 404, the totals, the admin's), its
`no_agent_routes_on_public_listener` case that the public port answers none of the agent
routes, and its `hosted_console_routes` case the console routes over a real admin link to a
scripted remote-admin server on loopback, through the console's table and a console drain,
the link's state published to its row (the session, CSRF and owner refusals, every
command and its reply, the arguments refused before a byte is sent, the rotation's
checks (a full rotation, between matches), the admission (one request in flight per account, one rotation edit per server), hostile replies kept valid JSON, the write bucket, the console ring, the admin's routes, the refusals before
running, with no link and while stopping, and no line off the curated list reaching the
server); `hosted_console_commands`
([`tests/novaworld/hosted_console_commands_test.cpp`](../../tests/novaworld/hosted_console_commands_test.cpp),
every build) the curated list itself, its compositions and its reply mapping;
and `hosted_local_e2e`
([`tests/novaworld/hosted_local_e2e_test.cpp`](../../tests/novaworld/hosted_local_e2e_test.cpp),
`BUILD_NOVAWORLD_HTTP` only) two real `opennova-serve` children on one machine's slots, the
machine a real `opennova-hostd` process (its process runtime) bootstrapping and syncing
against the internal listener: started from the site, each listed as its service account at its slot's game
port and shown as its owner's, the first renamed by the owner's `SetServerName` (its
`game.cfg` saved in its slot), administered over the supervisor's admin link (`PLAYER
LIST`, a recorded `CHAT SEND` in the console ring, no password in a log or console line),
and both stopped from the site, the link going down with the first. Across the five
programs, hosted servers run in the local stack's smoke test (`scripts/dev_stack.sh smoke`,
`tests/novaworld/stack_smoke.cpp`; CI's `local-stack.yml`), each machine a container of the
serve image the worker's docker driver launched. `hosted_admin_link`
([`tests/novaworld/hosted_admin_link_test.cpp`](../../tests/novaworld/hosted_admin_link_test.cpp),
every build) runs the admin link against a real `opennova-serve` child on the LAN network
type with the hosted renders' `admin.cfg`: the login, `PLAYER LIST`, `CHAT SEND` read back
by `CHAT GET`, the reply framing (two packets, one, the quiet gap, each followed by a
command in step), the status poll, the reconnect after the child is killed and started
again on the same port, `QUIT` at the end, and the password in no log or console line;
`admin_protocol` and `admin_server` pin the reply-count table itself, against the
engine's own console.

Account email: the route harness's `register_ok` and `register_duplicate` cases the
required, refused and taken addresses and the welcome mail (the first listener's mail goes
to a recording mailer through its `mail.send` jobs, run as a worker would, the sent job's
body gone from its row), its `password_reset`, `password_reset_links`
and `password_reset_brakes` cases the reset routes (the same 202 for every address, the
link's token kept as its hash, its one use, its hour, a newer link replacing the older, an
inactive account getting none, the sessions and the old password ending, both brakes);
`mailer` (`BUILD_NOVAWORLD_HTTP`, database-free) the address rule, the SES mailer on a stub
`CommandRunner` (its argv, the request file owner-only and gone after, the CLI's failures)
and the templates; `backend_schema` the column, its index and rules and `password_resets`.

The support tiers: `billing_stripe` (every build) the transport, the typed client, the
webhook signature and the settings; `billing_entitlement`
([`tests/novaworld/billing_entitlement_test.cpp`](../../tests/novaworld/billing_entitlement_test.cpp))
the entitlement's truth table (the statuses, the period's net, comp against tier, the
add-on beside a tier, past due, an inactive or service account) and the billing tables;
`billing_sync`
([`tests/novaworld/billing_sync_test.cpp`](../../tests/novaworld/billing_sync_test.cpp))
the sync over the in-memory Stripe (an event synced into the entitlement, a redelivery,
another instance's customer and the orphans, a wiped cache rebuilt by a reconcile, the
backoff, a deleted customer, the price check), its jobs (enqueued, deduped, scheduled,
a failed reconcile's retry, a sync holding no connection while Stripe answers) and its
state in the database (another sync over it reads the prices, the reconcile and the
backoff, and other price lists read unchecked); and the
route harness's `billing_routes`,
`billing_webhook`, `billing_admin` and `admin_user_detail` cases the routes over the
same fake, the webhook's signatures computed with `io::hmac_sha256`, and its `healthz`
case both probes, the watchdog on a hand-stepped clock and a listener whose Redis is gone
(its `/livez` ok, its `/healthz` not).
