# NovaWorld server

The NovaWorld service: six programs ("Programs" below: gate, lobby, web, fleet,
worker, data), each its own binary and container image, over this shared library
(`apps/novaworld_common`), one PostgreSQL database ("Database" below) and, for web, one
Redis ("Redis" below). Linux only: it ships as its images and runs on ECS Fargate, one
service per program (DEPLOY.md). Implements the matchmaking/backend half of NovaWorld (the gate, the
session handshake, the browser/host/play container services, and the legacy
`NW*.dll` HTTP flow). Protocol record: [`docs/net/novaworld-net-re.md`](../../docs/net/novaworld-net-re.md).
The hosted servers (the fleet, its supervisor, the data sets, the console) are
[`hosted/README.md`](hosted/README.md).

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
listener the fleet's while it leads (`apps/novaworld_fleet`), and a health
listener each program's but web's, whose `/livez` is on its HTTP port.

## Programs

Six programs, each its own binary (`opennova-novaworld-<program>`), its own image
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
| fleet (`apps/novaworld_fleet`) | the election ("The fleet program" below); while it leads the internal listener (bound only then), the hosted servers' supervisor with its admin links and the console drain; it runs and schedules no job | one leader (a standby waits) |
| worker (`apps/novaworld_worker`) | the job pool and its handlers ("Background jobs" below): the prunes, the account jobs, the mail it sends, the billing sync, the fleet's machine driver; the scheduler of its periodic kinds | more workers, more threads each |
| data (`apps/novaworld_data`) | the job pool over the data sets' jobs on the object store (the trees' finalize and prune, the releases' validate and apply) and the scheduler of their prune | one task (the data jobs' caps are global) |

Each program's own code lives in its directory, a static library its tests drive in
process (`opennova_novaworld_<program>_core`; the worker and the data program have their
`main.cpp` alone) and its executable; what more than one program uses lives here. A program
runs only its own job kinds and schedules only those: no kind has handlers in two programs.

- **They share state only through PostgreSQL** (durable) **and web's Redis**
  (ephemeral), never through memory: the lobby's sessions and commands, the lobby
  instances, the listings, the jobs, the machines and servers, the admin links' states
  and the console's answers are rows each program reads; `GET /api/unknowns` reads
  `unknown_messages`, where every process flushes its sightings. What several programs
  decide from the same settings they decide alike (`parts.*`): whether hosting is on
  (web's routes, the fleet's supervisor, the worker's driver), whether billing and
  mail are, the periodic jobs each schedules.
- **Every address a program announces comes from the config**, never from a port another
  bound: the gate's `POSTIPADDRESS` is `ONNET_PUBLIC_HOST` (where status blobs land), its
  `udpnovaworld` the one lobby address every lobby instance announces (its row), and its
  `startupurl`,
  the lobby's SessionInit web domain and the menus' `HOST_URL` and `GSB_SERVER` all name
  the one web endpoint, `ONNET_WEB_HOST` with `ONNET_HTTP_PORT`, so all three agree. The
  hosted servers' master address and gate port are `ONNET_PUBLIC_HOST` and
  `ONNET_GATE_UDP_PORT` (every machine, a local stack's container included, lists through
  the gate's public address); the machines reach the fleet program at `ONNET_INTERNAL_URL`, a
  name that follows it.
- **What an ECS task finds from itself** (`ecs_metadata.*`), where no setting can name
  it before the task runs: the fleet program, with `ONNET_INTERNAL_HTTP_BIND` unset and the
  task metadata endpoint there (`ECS_CONTAINER_METADATA_URI_V4`), binds its task's
  private IPv4 at 8070.
- **What a machine or container uses** (`proc_stats.*`) is read from the kernel's own
  files: the host's CPU, memory and network (`/proc/stat`, `/proc/meminfo`,
  `/proc/net/dev` less loopback), a filesystem's room (statvfs) and a cgroup's CPU time and
  memory (v2, else v1). hostd compiles it in for its machine's stats.
- **Each program samples its own task** (`program_usage.*`, `program_samples`): once a
  minute, beside its heartbeat, its main loop writes its task's CPU over the minute (in
  percent of its CPU limit), its memory and the most it may use, and its network
  namespace's bytes in and out (cumulative counters), read with `proc_stats.*` from its own
  cgroup and `/proc/net/dev`. A task is its ECS task id (the task metadata's ARN, with its
  Limits) or, off ECS, its hostname (a local container's id); its limits are its cgroup's
  where set, else the task's, else the machine's. The worker's `stats.prune` rolls the
  minutes up by the hour (`program_hourly`) and prunes both (48 h, 400 days); the admin's
  usage view reads them (Infrastructure below).
- **Where the logs are** (`program_log.*`, `ONNET_LOG_STORE`): a deployment's are its
  CloudWatch groups under `/opennova/<deployment>` (`/service` for the programs, filled by
  ECS's driver; `/machines`, hostd's; `/servers`, the hosted servers'); the local stack's a
  directory, the `logs` volume at `/var/log/opennova`. With a directory, every program
  copies each line it writes to stdout and stderr, after its time
  (`2026-10-18T14:05:03.123Z <line>`), into `programs/<program>/<container id>.log`: its
  two descriptors become pipes a thread drains into the container's own log and the
  file, so `docker logs` keeps every line in order. The file is written by a thread of its
  own, so the console never waits on the disk: past 1 MiB waiting, lines are dropped and the
  file notes how many in their place, and a file that refuses a write (a full disk) ends the
  copy with one `[log]` line on stderr naming why. The machines' hostd writes `machines/` and
  `servers/` beside them. Web reads either for the admin's log view (Infrastructure below).
- **Each process logs `{"boot":{"program":"<program>"}}` once as it starts** (stdout, its
  first line, so a crash loop on a missing setting counts too: the log pipeline counts
  boots per program), keeps printing the `[boot] migrations:` line, logs its own
  heartbeat with its program and the fields it knows, and runs its own stall watchdog
  over its own loops ("Health" below): a stall exits that process alone.
- **A redeploy of the gate, web, the fleet program or the worker leaves the lobby
  serving**: only a lobby's boot clears lobby rows, and only its own instance's; the
  retail login's `active_user_sessions` rows are never wiped at boot (their 2 h idle prune
  and their account's next login end them); a restarted fleet leader resumes from rows (its
  reconciliation), a restarted worker's leases run out and its jobs run again. A lobby's
  restart drops its own sessions and listings alone, and its hosts list again at the one
  lobby address ("Lobby instances" below).
- **Deployment**: one ECS Fargate service per program (`infra/aws/programs.tf`), the
  gate (UDP 7597), the lobby (UDP 64206) and web (TCP 443 through the portal's nginx in
  web's task, TCP 8080) behind one network load balancer at the gate's address, the
  fleet registered in Cloud Map; DEPLOY.md. The local stack (`deploy/compose/`,
  `scripts/dev_stack.sh`; DEVELOPING.md) runs the six, each its own container, plus
  PostgreSQL, Valkey, the portal in web's network as in web's task, and with a data set
  the hosted servers' machines as containers of their own (the docker machine driver,
  [`hosted/README.md`](hosted/README.md)).

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_WEB_HOST` | none | The one load-balanced web endpoint (a name or an address) the gate's `startupurl`, the lobby's SessionInit web domain and the menus' `HOST_URL` / `GSB_SERVER` name, with `ONNET_HTTP_PORT`. Required by the gate, the lobby and web. |
| `ONNET_PUBLIC_HOST` | `127.0.0.1` | The gate's public address: its `POSTIPADDRESS` (where the hosts' status blobs land), the hosted servers' master address, the switcher's (web's `GET /api/server-info`). The gate requires it set in its environment (an explicit `127.0.0.1` too): a gate left on the default would announce loopback to every client. |
| `ONNET_LOBBY_ENDPOINT` | none | The lobby: the one lobby address every lobby announces, `<host>:<port>` (its instance row, the gates' `udpnovaworld`; on ECS the load balancer's, the gate's address and 64206); required. The gate: optional, the lobby it hands out while no instance takes clients; with none either, a probe goes unanswered (the client asks again) and the gate logs it, once every 10 s at most. |
| `ONNET_INTERNAL_URL` | none | The fleet's internal listener as the machines' agents reach it, `http://<host>:<port>` (`fleet.<deployment>.opennova.internal:8070`, the deployment's Cloud Map name, in a deployment; the fleet's compose name in the local stack): the worker's alone, whose machine driver hands it to each machine it launches. Required by the driver with hosting on. |
| `ONNET_HEALTH_PORT` | 8081 | The TCP port `GET /livez` answers on in the gate, the lobby, the fleet program, the worker and the data program (web's is its HTTP port); 0 lets the OS pick (the tests). |
| `ONNET_LOG_STORE` | none | Where the logs are: `cloudwatch:/opennova/<deployment>` (the deployment's groups beneath that prefix; web's task sets it, for the admin's log view) or `file://<directory>` (the local stack's `file:///var/log/opennova`: every program copies its lines into `programs/<program>/<container id>.log` there; web reads it). Unset or malformed (a boot warning): no store, no copy, and the log view answers `no_log_store`. |

## Thread model

- **The main thread owns the boot and the program's main loop.** It migrates and seeds
  the database, starts its program's parts, then ticks every `tick_interval_ms` (the
  lobby's `manager.tick(now)`, the `UnknownTracker` flushes) and writes the heartbeat.
- **Each listener runs on its own internal thread(s)**, mutating shared state through
  the `ConnectionManager` verbs and mutex-guarded maps.
- **Nothing outside the lobby touches the lobby's memory.** The NW UDP listener's
  sessions, registry and socket are its receive thread's. The lobby keeps a row per
  session in `lobby_sessions` and takes the commands addressed to it from
  `lobby_commands` ("The lobby's tables" below); web and the fleet program read the one and
  insert into the other, never calling into the listener.
- **Every thread has its own database connection.** A `db::Database` is used by one
  thread at a time; each process owns one `db::ConnectionPool` on `DATABASE_URL` (at most
  `DATABASE_POOL_MAX`), and a thread holds one lease at a time, handing its
  `db::Database&` to what it calls (a second lease on one thread could wait on itself; a
  debug build asserts it). The gate and NW UDP receive threads each lease one for their
  lifetime (`start()` waits for it, so an unreachable database stops the boot); each HTTP
  request leases one for its handler. A `db::Error` carries the SQLSTATE and names the
  failing statement, never a bound value, so it goes to the server log alone and a reply
  carries only `db_error`. A write that spans statements is one `db::Transaction` taking
  the narrow locks its invariant needs, and a read that spans them (`/api/hosts`, the
  `.gsb` feeds) one `db::ReadSnapshot` ("Database" below).
- **The background jobs run on the job workers' threads** (`ONNET_WORKER_THREADS`), each
  handler leasing its connection around its database work, never across a slow call; the
  fleet's election, supervisor and console drain each run on a thread of their own
  ("The fleet program" below), and the worker's and the data program's scheduler each on
  one of its own.
- **No request reads back what another request left in this process's memory.** What
  a later request reads (the retail `NW*.dll` sessions, the retail login pins, every rate
  limiter's buckets) lives in Redis ("Redis" below), so web's replicas serve the same
  clients whichever one each request reaches.
- **The hosted servers' supervisor** (with `ONNET_HOSTED_PROVIDER` set) starts once the
  fleet program leads and stops before the listeners; each running server's admin link is a
  thread of its own (at most `ONNET_HOSTED_CAP`), ended after the supervisor so its
  session ends with `QUIT` ([`hosted/README.md`](hosted/README.md)).
- **The `UnknownTracker` records from listener threads** into an in-memory accumulator
  flushed to the database on the main tick, never per packet (the gates' and the lobbies'
  sightings, web's 404s); each flush adds to the row's count, so every process's
  sightings sum up. `GET /api/unknowns` (admin) reads the `unknown_messages` rows, the
  latest first (500 at most, `?channel=`): each signature's first sample as hex and its
  sender. What a process keeps is bounded (a signature cut at 128 bytes, 1024 per channel,
  the rest counted in its `(other)` row), and `unknown_messages.prune` deletes the rows
  unseen for 30 days.
- **Each listener's `start()` holds its port before the boot goes on**, so the port it
  reports is the one it serves (also port 0, the OS's pick); a bind that fails stops the
  boot. A lobby registers its instance (and so counts for the gates) only once its socket
  is bound, and its stop drains the instance first ("Lobby instances" below).
- **Each process owns SIGINT and SIGTERM** (`docker stop` and an ECS task's stop send
  SIGTERM). The handler only raises the shutdown flag and puts the signal's default action
  back, so the same signal again ends a shutdown that is stuck. A container's PID 1 never
  gets a default-action signal from the kernel (pid_namespaces(7)), so the ECS task
  definitions (`initProcessEnabled`) and the compose files (`init: true`) run each program
  under an init that forwards both; a bare `docker run` needs `--init`. The main loop sees
  the flag within one tick and the program stops its parts in reverse order, the pool last.
  The HTTP listener clears Crow's own signal set (`signal_clear()`), which would otherwise
  take both signals and stop only Crow.

## Health: the watchdog, `/livez`, `/healthz` and the heartbeat

Service policy (`health.*`), what the container runtime and the uptime checks read:

- **The stall watchdog, one per process, over its program's loops alone**: the gate's
  and the lobby's receive loops (each wakes at least every 250 ms), the fleet's
  supervisor (the election's turns standing in while it is a standby, or with hosting
  off), the worker's and the data program's job pool (only a pool whose every thread
  stands still stalls: each slow call has its own timeout) and scheduler, and every
  program's main loop. A thread of its own samples
  the loops' turn counters every second; a loop that has not moved for 30 s logs
  `[watchdog] FATAL the <loop> made no progress for 30 s; exiting 70` and the process
  exits 70 at once (nothing can unstick a thread; an orderly shutdown could hang on it),
  and the runtime starts it again. The other programs' processes are untouched.
- **`GET /livez`**, the process's liveness: `ok`, or `503 stalled` while a watched loop
  (or the watchdog's last sample) is 30 s old. The gate, the lobby, the fleet program, the
  worker and the data program answer it on their health port (`ONNET_HEALTH_PORT`; `health_listener.*`), web on
  its HTTP port. Each program's ECS container health check and image `HEALTHCHECK` ask it
  (`curl -fsS --max-time 2`), and so does the gate's target group. A cache outage fails no
  web task's liveness, since no restart mends it.
- **`GET /healthz`** (web's), whether it serves a retail client: `/livez`'s answer, else
  `503 redis unreachable` while Redis does not answer a `PING` (one a second at most).
  The load balancer's target group and the Route 53 check use it, so the balancer routes
  around a web task that cannot reach the cache. Both sit outside `/api`
  (`Cache-Control: no-store`), so `ONNET_API_PROXY_ONLY` never refuses them; `GET
  /api/health` stays, a static `{"status":"ok"}` behind the proxy rule.
- **The heartbeat**, one line on stdout every 60 s, for the log pipeline's metric
  filters: `{"heartbeat":{"program":"<program>",...}}`, the program first, then the
  fields that program knows in this order, then every program's lock failures:
  `{"heartbeat":{"program":"fleet","hosted_live":N,"hosted_failed":N,"machines_lost":N,"machines_held":N,"launch_failures":N,"limits_held":N,"attention_danger":N,"attention_warn":N,"machines_stuck":N,"machine_disk_free_pct":N,"fleet_pass_age_s":N,"db_lock_timeouts":N,"db_deadlocks":N}}`.
  "Since the last line" counts reset each line; a deployment-wide count every process of
  a program logs alike is aggregated with the maximum, not the sum.
  - lobby: `nw_sessions` (its own NovaWorld UDP sessions), `listed_hosts` (the
    deployment's listed servers; aggregate with the maximum);
  - web, with billing on: `webhook_rejects` (deliveries refused since the last line);
  - fleet, with hosting on: `hosted_live`, `machines_lost` and `machines_held` (the lost
    machines the mass-lost brake holds from their end; now), `hosted_failed` and
    `launch_failures` (since the last line), `limits_held` (1 while the owners' limits
    hold is on); and the leader alone (a standby's would be a stale pass): the fleet's
    health (`hosted/fleet_health.h`) as `attention_danger` and `attention_warn` (its
    findings of each level), `machines_stuck` (the machines a stuck or flapping finding
    names), `machine_disk_free_pct` (the least free share of a ready or draining
    machine's trees' disk; left out with none reporting), and `fleet_pass_age_s` (the
    seconds since its supervisor's last pass that failed in nothing; from its start
    before one);
  - worker: `billing_sync_age_s` (the oldest unsynced webhook event's age; 0 with none,
    -1 with billing off);
  - gate: `probes_unanswered` (since the last line, for want of a lobby: what the
    deployment alarms on), `lobbies_live` (aggregate with the maximum);
  - every program: `db_lock_timeouts` (`55P03`) and `db_deadlocks` (`40P01`), the
    statements that failed so since the last line.
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
- **Periodic jobs** (`jobs/scheduler.*`): each program that runs jobs schedules its own
  periodic kinds alone (`parts.h`: the worker's `worker_schedule`, the data program's
  `data_schedule`), in every one of its processes with no election. Each tick (every
  second) enqueues each periodic kind once per time bucket, keyed `<kind>@<bucket>` (unique
  for good, so however many processes tick a bucket gets one job), and none beside one still
  queued or running; the billing reconcile also runs at the worker's start, keyed by its
  release (`<kind>@start:<family>:<revision>`, its ECS task definition's, so a release's
  tasks run it once; by its kind alone off ECS), and the billing sync's bucket is enqueued
  only while an event waits.
- **The prune**: finished jobs are deleted 14 days after they end (`jobs.prune`, hourly;
  an admin's prune names a shorter keep in its payload, `keep_s`, a day at the least).

| Kind | What | Enqueued by | Run by | Cap |
| --- | --- | --- | --- | --- |
| `machine.launch` | the row's launch (`hosted/fleet_jobs.*`) | the supervisor, with the machine's row | worker | 8 |
| `machine.describe` | the cloud's word on a live machine, onto its row | the supervisor's boot and lost checks, deduped per machine | worker | 4 |
| `machine.terminate` | a cloud machine ended | the supervisor's ends | worker | 4 |
| `fleet.sweep` | the driver's machines against the rows | every 5 min (the worker's schedule), and at the supervisor's reconciliation | worker | 1 |
| `billing.reconcile` | the full reconcile with Stripe | at a release's start, every `ONNET_BILLING_RECONCILE_MINUTES`, an admin | worker | 1 |
| `billing.sync` | the customers with an event to sync | each fresh webhook event; every minute while one waits | worker | 1 |
| `mail.send` | one mail, at most once | the welcome and reset routes | worker | 4 |
| `hosts.prune`, `host_keys.prune`, `web_sessions.prune`, `active_user_sessions.prune`, `login_addresses.prune`, `password_resets.prune`, `billing_events.prune` | the prunes (`active_user_sessions.prune`: the retail logins' rows unused for 2 h) | every `HOST_SWEEP_INTERVAL_MS` | worker | 1 |
| `stats.prune` | the machines' minutes rolled up into their hours; the hosted servers' and the machines' samples past 48 h, their hours past 400 days, the machines' egress months past 13, the programs' task minutes rolled up by the hour (minutes past 48 h, hours past 400 days), the fleet's machines ended or failed 14 days ago | hourly | worker | 1 |
| `jobs.prune` | finished jobs past 14 days (or its payload's `keep_s`) | hourly, an admin | worker | 1 |
| `unknown_messages.prune` | the unknown-message rows no process has seen for 30 days | hourly | worker | 1 |
| `tree.finalize` | an uploaded data set tree checked, sized and made ready (`hosted/data_set_jobs.h`) | the admin's finalize | data | 2 |
| `release.validate` | an uploaded update package read, mapped to its data sets, made a draft or invalid | the admin's upload completion | data | 1 |
| `release.apply` | one section of a published release applied to its data set's baseline, a new tree | the admin's publish, one per section | data | 1 |
| `tree.prune` | the objects of the prunable trees removed | hourly (the data program's schedule), after each job above, the delete routes | data | 1 |

**The admin's view** (`jobs/job_admin.*`, `jobs/job_routes.*`; the site's admin page
**Background jobs**, `/admin/jobs`) reads the jobs table and the fleet leader's row alone,
so every replica answers alike. A worker process shows by the claims its threads left on
the rows; a running job whose lease ran out is one whose process has gone away. The
failures come grouped by kind and **failure key** (the error with its ids, times and
numbers taken out, its first 300 characters), so launches that failed alike are one row
with a count. Each action is one compare-and-set on the job's state, so none changes a job
a worker holds, and each is logged with its actor and the job.

| Route | Auth | Reply |
| --- | --- | --- |
| `GET /api/admin/jobs/overview` | admin | each kind's counts, latency and mean run, the newest 20 failure groups, the worker processes, the schedules, the newest scheduled job, the fleet leader; never a job's secret |
| `GET /api/admin/jobs` | admin | the filtered, paged list (`?state=`, `?kind=`, `?from=`, `?to=`, `?q=`, `?failure=`, `?before=`, `?limit=` 1..200); 400 `invalid_field` |
| `GET /api/admin/jobs/<id>` | admin | `{"job"}` with its `payload`; 404 |
| `POST /api/admin/jobs/<id>/retry` | admin | a failed job queued again with one more attempt: 200; 404; 409 `not_failed`, `secret_dropped`, `already_pending` |
| `POST /api/admin/jobs/retry-group` | admin | `{"kind", "key"}`, each job of that failure group retried: 200 with each outcome's count; 400 |
| `POST /api/admin/jobs/<id>/cancel` | admin | a queued job failed for good, its secret dropped: 200; 404; 409 `not_queued` |
| `POST /api/admin/jobs/run` | admin | `{"kind"}`, a periodic kind enqueued now (`<kind>@manual-<ms>`): 202; 400; 404; 409 `already_pending` |
| `POST /api/admin/jobs/prune` | admin | `{"keep_days": 1..14}`, a `jobs.prune` now: 202; 400; 409 `already_pending` |

The replies' times are ISO 8601 UTC, with `Cache-Control: no-store`.

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_WORKER_THREADS` | 4 | The job pool's threads (1..64) in the worker and in the data program, and at most `DATABASE_POOL_MAX` less two (the pool's keeper's connection and the one the process holds): asked for more, that many start, with a boot warning. |

## The fleet program

`apps/novaworld_fleet/election.*` (migrations `0008_controller_state.sql` and
`0014_fleet_leader.sql`, which renamed its row's table): of the
fleet processes (`apps/novaworld_fleet/main.cpp`), the one that runs the machines' internal
listener, the hosted servers' supervisor (with its admin links) and the console drain,
elected among them. It runs no job pool and no scheduler: the periodic jobs are each
program's own ("Background jobs" above). Service policy, not a port.

- **The election.** A session advisory lock (`db::SessionLock`, its own key) on a
  connection of the fleet's own, outside the pool, tried again while another
  process holds it (each try waits the lock timeout): a standby waits there. Once it
  holds the lock, the fleet program moves `fleet_leader.generation` on and names
  itself (`holder`, a random `fleet-<hex>`), then starts its parts: the internal
  listener (bound only now, so the deployment's internal name reaches the process that
  renders the start files; a bind that fails ends the process), the supervisor (which
  reconciles), the console drain.
- **The fence.** Every check (each second) it reads the row on its lock's connection.
  The connection gone (and the lock with it) or a generation not its own is the
  leadership lost: the server logs `[fleet] FATAL ... lost the leadership` and
  exits 71, and its container starts it again as a standby. Every supervisor pass first
  asks that it still leads (its flag, and the row's generation its
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
  takes the configured `ONNET_GLSVSS_*`, none by default). DFX2's row carries the values
  NovaLogic's live gate answered for its tag, in this gate's shape (the witness:
  `docs/titles/dfx2-vs-jo-re.md`). A tag no row (nor the alias) answers to gets no reply,
  and the unknown tracker records it. The gate reads the table once in `start()`, so its
  receive thread never touches the database for a probe; a change to the table takes a
  restart.
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

- **Why one address** (ADR 0063 decision 8, with its witness): a stock host reconnects
  only to the lobby address the gate first gave it, after 240 s of silence, so with one
  address a host whose lobby went away is listed again on a live one within about 271 s,
  with no restart; retail self-hosts recover the same way.
- **Its row** (`lobby_instances`: id, endpoint, `active` or `draining`, `started_at`,
  `last_seen_at`). The NW UDP listener's start registers a new row, reaps every instance
  unseen for 5 min with its rows (`kInstanceStaleSeconds`, well past a database failover)
  and clears the rows no instance holds, all before it serves. Its receive thread moves
  `last_seen_at` every 5 s; a row found reaped all the same is registered again under a new
  id (draining still when it was) and the lobby writes its session rows and listings anew
  from memory. A clean stop drains the row, then deletes it with its rows. Every row the
  lobby writes names its instance (`lobby_id`).
- **The gate hands every client the one lobby address** while at least one instance is
  active and seen within 15 s (`lobbydb::live_lobbies`, read every 5 s on the gate's own
  connection); after a failed read, its fallback for six refreshes; with none live, its
  fallback `ONNET_LOBBY_ENDPOINT`; with no fallback, no reply (logged every 10 s at most).
  The reply is the gate's one send, to the probe's source, so behind a UDP load balancer it
  rides the probe's flow back. `POSTIPADDRESS` stays this gate's (`apply_status_blob` finds
  the listing by its key whichever lobby holds it).
- **The lobby behind the balancer**: its target group (UDP 64206, client addresses and
  ports preserved, so a lobby lists a host at its own address) checks each task's `/livez`.
  A deployment starts the new task first, then the balancer drains the old one (10 s)
  before ECS signals it; a lobby always answers on the flow a datagram came in on.
- **Draining** (`POST /api/admin/lobbies/<id>/drain`, or its own lobby as it stops): the
  instance no longer counts for the gates; its sessions stay until they leave. An admin's
  drain does not take its task out of the balancer.
- **RIDs come from one sequence** (`lobby_rid_seq`: 0x0A000000 and 1..0xFFFFFF, cycling, a
  held suffix passed over), so two instances never mint one RID and none is reused while
  held. A host that relists on another instance gets a new RID.
- **Pushes need no instance named**: a command takes its session row's `lobby_id` as it is
  inserted, each instance takes only its own, and a `forget_owner` goes to every instance.

| Route | Access | Replies |
| --- | --- | --- |
| `GET /api/admin/lobbies` | admin | the instances by id, each with its state, times and session count |
| `POST /api/admin/lobbies/<id>/drain` | admin | 200; 404 |

**The lobby's tables** (`backend/migrations/0006_lobby_commands.sql`,
`engine/net/novaworld/lobby_repository.h`; service policy):

- **`lobby_sessions`**, one row per lobby session, which the listener alone writes:
  inserted with the `ClientHello` that admits the address, replaced by a `ClientAuth` that
  starts a new logical connection, updated as the session's statements change it (its
  retail login's tag, RID, hosting, listing), deleted with the drop; `last_seen_at` lags
  by up to 10 s. The receive thread writes the row before it answers the statement that
  changed it.
- **`lobby_commands`**, what the routes and the supervisor address to the lobby holding a
  session: `server_command` and `stop_hosting` (the pushes below), `end_session` (the
  admin's end) and `forget_owner` (an account lost its keys). An insert is made only when
  the session rows say the lobby would take it. The receive thread takes them between
  receive batches (at most 256 datagrams each, so a flood still lets the commands and the
  heartbeat run), every 250 ms at most, oldest first, checks each again against its memory
  and runs it; one whose session is gone or changed, or past its 30 s expiry, is dropped.
  A command is best effort, as the pushes always were.

**The admin's view** (`connection_routes.*`, the site's admin page `/admin/sessions`):
every session the instances hold, from their `lobby_sessions` rows, with the server it
lists and an end queued for its instance. A session's account is its listing's owner
while it hosts, else the account whose live retail login its latest `Cookie` list named
(its `LOGINSESSIONTAG`, which the row keeps and no route sends).

| Route | Access | Replies |
| --- | --- | --- |
| `GET /api/admin/connections` | admin | every session, oldest first: its row's `session_id`, its instance, state, game tag, address, times, `account` (`via` `host_key` or `login`) and `host` while it hosts |
| `POST /api/admin/connections/<session id>/end` | admin | 202, an `end_session` command for the instance holding it: the client gets its `ServerGoodBye` burst and the connection drops (`Logout`), as the NAPI server role ends a connection it asked to disconnect; 404 for a session that ended; 409 `hosting` (with its `rid`: stop the listing instead, since ending its session only parks it for the re-host) |

## Pushing statements to a listed server

The service can send a hosting connection retail's two administrative statements over
its own NovaWorld session: `ServerCommand` (one `Cmd` verb line; the host runs it, so
the service can punt a player, rename the server, change its message, cycle the map
and so on) and `ServerStopHosting` (`MsgCode` / `MsgParam1` / `MsgParam2`).
`lobbydb::queue_server_command` / `queue_stop_hosting` resolve the RID to the hosting
session's row and insert the statement as a command for its lobby ("The lobby's tables"
above); the lobby's receive thread takes it within 250 ms and frames it exactly as a reply
(one reliable record, retained until the client's ACK covers it). A connection replaced,
dropped or no longer hosting by then loses the push. The thread also runs the NAPI server
role's ACTIVE send-interval leg (CS field 5, 1000 ms): a connection with records still
unACKed gets one header-only packet per interval, whose fresh sequence shows the client a
gap if the push was lost. The server role's own 60 s keepalive is not run (D-NET-393: the
service already answers every packet the client sends).

`ServerStopHosting` is the sysop punt: a stock host ends its running NovaWorld match for
everyone, goes to the main menu and resets its NovaWorld session (the RE record's "The
exit"). The service takes the server out of the browser once the statement is framed,
since the host answers it with no `ClientStopHosting`.

The routes live in `host_routes.*`. Both families compose a command through one helper,
`compose_server_command`: the body `{"verb", "target", "args"}` (`target` one of `None`,
`ByIndex`, `ByIpAndPort`, `ByName`, `ByPCID`, or absent) goes through
`server_command_text`, so a line the host's reader would drop is a 400 with the
composer's reason, and so is an arg holding a control byte (C0, 0x7F or C1).

The body is UTF-8 (JSON); a stock host reads its `Cmd` in cp1252 (D-NET-382), so the
composer converts each arg to cp1252 (`base/io/cp1252.h`; an escaped surrogate pair
joined first) and refuses with a 400 an arg holding a character cp1252 lacks (naming its
codepoints) or bytes that are not UTF-8, never substituting. `SetServerName` and
`SetServerMsg` texts are counted in the host's bytes, 31 and 127 at most
(`SERVER_COMMAND_NAME_CAP` / `SERVER_COMMAND_MSG_CAP`), so a longer one is a 400 rather
than clipped there. A host reports its text back in cp1252, so every JSON list of listed
servers sends it as UTF-8 (`retail_text_to_utf8`); the GSB blobs and every other retail
wire output keep the host's bytes.

**The admin's pushes** (the Bearer `ADMIN_API_TOKEN`, or an admin-role website session
with the CSRF header; see "Website sessions and roles"), each logged with its actor and
the composed line (control bytes escaped):

| Route | Body | Replies |
| --- | --- | --- |
| `POST /api/admin/hosts/<rid>/command` | `{"verb": "SetServerName", "target": "None", "args": ["New Name"]}`, any of the eighteen verbs | 202 queued (with the composed `cmd`); 400 with the composer's reason; 404 unknown RID; 409 not hosting |
| `POST /api/admin/hosts/<rid>/stop` | none | 202 queued (MsgCode 7, `NWUSERVERMSGCODE_NOVAWORLDSYSOPPUNT`); 404; 409 |

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
  (`hostdb::listing_owned_by`, the live claim, read at the request), and again at push
  time: the queue resolves the RID only to a session whose row names that account as its
  listing's owner, and the receive thread drops the statement if the owner changed or the
  claim went before it went out ("Revocation" under "Listed servers and owners" below). A
  RID no listing holds, another account's listing and a miss at the queue get the same
  404 (`unknown_rid`), so the routes never tell one from the other.
- **The rate limit.** Each owner push takes one token from the account's bucket (in
  Redis): 8 at once, then one every 2 s; past it 429 `rate_limited` with `Retry-After`.
  The token is taken before the body is read, so a refused body or another account's RID
  spends it too.
- **The verbs.** Every `ServerCommand` verb acts only on the server it is sent to, so
  an owner may send all of them but `SetMPReset`, which stays an admin's: a nonzero
  `mpreset` saved in `game.cfg` ends the host process at its next session create
  (D-NET-385). The stop is the owner's way to end the hosting.
- **What reaches everyone.** The stop, `Cycle`, `EndMission` and `GameOver` act on every
  player on the server; the page confirms each first. `PuntPlayer` aimed at a listen host's
  own slot (slot 0) ends its hosting too.
- **The log.** Every owner push past the session check is logged once with the account,
  the RID, the composed line and the outcome (`queued`, `not_hosting`, `unknown_rid`,
  `no_listing_of_theirs`, `invalid_command`, `verb_not_allowed`, `rate_limited`).

## Listed servers and owners

Every listed server can name the account that hosted it (`active_hosts.host_user_id`),
through the HOSTKEY a stock client fetches from `/NWHost.dll` and copies into its `Host`
var list's `HostKey`. Service policy throughout: how the retail service checked a
HostKey is unwitnessed.

- **The key.** `/NWHost.dll`'s first call mints the 48-letter A..P key (24 CSPRNG bytes)
  and, when the request's own retail login names an active account, records it for that
  account in `host_keys`, as its SHA-256 only. The account is the one the request's
  `LOGINSESSIONTAG` names while that retail login lives, else the one its
  `PERSISTENTEXPRESSLOGINDATA` pin names, each only at the credentials generation that
  login read ("The credentials generation" below); never the `NWHANDLE` cookie, which any client can
  set. Either way the pages and their cookies are the bytes a stock client parses. An
  account keeps its newest 20 keys (and any a listed server holds).
- **The claim.** A connection presenting a `HostKey` claims it for its RID
  (`hostdb::claim_host_key`), and the row then carries the account. A stock host's fresh
  `ClientHostRequest` carries no key, so it is listed with no owner until the first
  `ClientHostUpdate` carrying it (D-NET-465). A key backs one listed server at a time. A
  dropped connection parks its state and RID for 240 s, and its re-host after the NWU
  reconnect re-sends its key, so it keeps the same RID and owner. A boot releases every
  claim with the previous run's rows.
- **Expiry.** A key lasts 24 h from its last use, its claim or the periodic
  `host_keys.prune` job while a listed server holds it (counted at most once an hour);
  the job deletes the rest.
- **Revocation.** A row stores its owner only while one of the owner's keys stands
  claimed for its RID; every write checks it. An admin password reset or a status other
  than `active` deletes the account's keys (with its retail login), a deleted account
  takes them (`ON DELETE CASCADE`, its listings' owner set NULL): the listing stays listed
  with no owner from its next write, and a banned account's key claims nothing. The
  account's own routes and owner pushes read the live claim (`hostdb::listing_owned_by`),
  so they drop the listing at once, and an account change made through the admin's user
  routes (or a reset by link) also queues a `forget_owner` for the lobby.
- **Strict mode.** `ONNET_REQUIRE_HOST_KEY=1` lists no server whose `HostKey` claims no
  account (empty, unknown, expired, revoked, or held by another listing), with code 200,
  the retail table's `NWUSERVERMSGCODE_NEEDTOLOGIN`: a re-host carrying such a key is
  refused (`ServerHostResult` `Success=0`, which a stock client shows as NWEC58); a fresh
  request carrying no key is answered but held out of the browser until an update presents
  a key, and one whose key then claims no account ends with `ServerStopHosting`
  `MsgCode=200`. Off (the default), such a host is listed with no owner, so
  `opennova-serve` without `--credentials` keeps listing.

| Route | Access | Replies |
| --- | --- | --- |
| `GET /api/me/hosts` | a website session (`require_user`) | the caller's listed servers, oldest first, each with its browser columns, addresses, message and `roster` (whose `slot` a player verb's `ByIndex` names); 401; `Cache-Control: no-store` |
| `GET /api/admin/hosts` | admin | every listed server, the same fields plus `owner` |
| `GET /api/hosts` | admin | every listed server as the lobby stores it, with the joiners `/NWJoin.dll` recorded |

The public `/api/lobbies` and the GSB name no owner, and no joiner's account or
address; `GET /api/hosts`, which names both, is an admin route. The owner pushes to
its own servers through `POST /api/me/hosts/<rid>/command` and `/stop` ("Pushing
statements to a listed server" above).

## Billing (monthly support tiers)

A support tier is a monthly Stripe subscription that supports the project and sets the
account's hosting entitlement ([`hosted/README.md`](hosted/README.md)): tier 1, 2 or 3 lets it run 1, 2 or
3 hosted servers (`billing/tiers.h`), and the own-machine add-on, one subscription of
quantity 1 per own machine (bought through Checkout, canceled in the Portal, which
changes no quantity), moves that many of them onto machines of their own while a tier
also grants, and the static-address add-on, one subscription per address, gives an own
machine a fixed public address and a name ([`hosted/README.md`](hosted/README.md) "Static
addresses"). The service makes Stripe Checkout and Customer Portal
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
  Stripe last answered (`billing_subscriptions`: its item, `tier`, `own_machine`,
  `static_address` or `unknown` for a price no setting names, read from its price; its status and since when;
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
- **The sync** (`billing/billing_sync.*`, `billing/billing_jobs.*`): two jobs any worker
  runs ("Background jobs" above), their state in `billing_sync_state` and
  `billing_sync_backoff`. `billing.sync` reads each customer with an unrecorded event back
  from Stripe with all its subscriptions, then writes it in one transaction (no Stripe call
  ever runs inside one); a customer Stripe could not be asked about waits 60 s, doubling to
  15 min. `billing.reconcile`, at a release's start (the worker's), every
  `ONNET_BILLING_RECONCILE_MINUTES` and on an admin's ask, walks every subscription: this
  instance's written, another instance's (or one naming no account here) listed as an
  orphan, a granting row Stripe no longer knows canceled, two granting tiers on one account
  listed as duplicates. It also checks the configured prices (monthly, recurring, the key's
  mode, the sold one active, one currency); until they check out, checkout answers 503
  `billing_misconfigured`.
- **The capacity gate** (`hosted/sold_capacity.*`): a new subscription is never sold past
  what the fleet holds. Every account holds what its entitlement grants, used or not (its
  servers less those on own machines are shared places, its own machines are own
  machines), an admin's comp grant included; the fleet sells `ONNET_FLEET_MAX_SHARED` x
  `ONNET_FLEET_SHARED_SLOTS` shared places, `ONNET_FLEET_MAX_OWN` own machines and
  `ONNET_FLEET_MAX_ADDRESSES` static addresses (each one granted, used or not). A
  checkout is judged, and its item reserved (`billing_checkout_reservations`, an hour and
  a quarter), in one transaction under the hosting-start lock, before Stripe makes its
  session, so two checkouts at once never both take the last place; the reservation
  counts as the subscription until the sync writes the one its session made (its
  `opennova_reservation` metadata), the session's `checkout.session.expired` event, the
  account's next checkout or its end drops it. One past the fleet is 409 `sold_out`, and
  `GET /api/billing/tiers` says which items are `available`. A start's `at_capacity`
  stays the hard gate; the Portal's tier changes are not judged.

| Setting | Default | Meaning |
| --- | --- | --- |
| `STRIPE_SECRET_KEY` | none | `sk_` or `rk_` (a restricted key is the recommended one), `test` or `live`: the mode everything else must match. |
| `STRIPE_WEBHOOK_SECRET` | none | The webhook endpoint's signing secret, `whsec_...`. |
| `STRIPE_PRICE_TIER1`..`3`, `STRIPE_PRICE_OWN_MACHINE` | none | Comma lists of `price_...` ids: the first one sold, the rest still recognized (grandfathered); each price in one list alone. The amounts are Stripe's, never the service's. |
| `STRIPE_PRICE_STATIC_ADDRESS` | none | The static-address add-on's prices, the same way; required only while `ONNET_FLEET_MAX_ADDRESSES` is above 0 (else it may stay unset or `REPLACE_ME`). |
| `ONNET_SITE_URL` | none | The site's origin (no path): Checkout returns to `/me/support?checkout=success&session_id=...` or `?checkout=cancel` there, the portal to `/me/support`. `https://` with a live key. The dev compose stack sets `http://localhost:8000`. |
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
| `GET /api/billing/tiers` | public | `enabled` (billing on, its prices checked out), the currency, each tier's servers and amount, the own-machine add-on's amount, the static-address add-on's while the fleet sells addresses, and each one's `available` (the fleet takes one more for an account that has nothing yet; false: hosting is full) |
| `GET /api/me/billing` | a website session | the account's entitlement (`max_servers`, `comp_servers`, `comp_own_machines`, `own_machines`, `static_addresses`, `used`, `stopping`, `can_start`), the support tier and add-on subscriptions shown, each static address's subscription (`address_subscriptions`, with its address once named), `can_manage`, `can_subscribe`; `Cache-Control: no-store` |
| `POST /api/billing/checkout` | a website session, the CSRF header | `{"item": "tier1" \| "tier2" \| "tier3" \| "own_machine" \| "static_address"}`: 200 `{"url"}`; 400; 403 `not_eligible`, `payment_past_due` (an address while a payment is past due); 409 `checkout_in_progress`, `already_subscribed` (the portal changes a tier), `own_machine_limit`, `address_limit` (an address for every own machine already), `sold_out` (the fleet has sold all it holds of what the item adds); 429 (6, then one every 20 s, shared with the portal); 503 `billing_misconfigured`, `stripe_unavailable`. The account's previous session is settled first (a completed one synced), then the item judged and reserved under the hosting-start lock, so nothing is sold twice or past the fleet; the session asks for the terms of service and stamps the instance, the account and the reservation on the subscription |
| `POST /api/billing/portal` | a website session, the CSRF header | 200 `{"url"}`; with `{"cancel_subscription": "sub_..."}` (a static address's) the Portal's cancel flow for it, returning to `/me/addresses`; 400; 404 `no_billing_account`, `not_found`; 429; 503 |
| `POST /api/me/billing/sync` | a website session, the CSRF header | the account's customer read back from Stripe now (the success page's, before the webhook): as `GET /api/me/billing`; 404; 429 (3, then one every 10 s); 503 |
| `POST /api/billing/webhook` | Stripe's signature | above |
| `GET /api/admin/billing` | admin | the mode, the price check, the last reconcile, the pending events, every subscription with its account, the duplicates, `unknown_prices`, the orphans, the newest 50 events and the owners' limits hold (`limits_hold*`); with billing off `{"enabled": false}` and the hold alone |
| `POST /api/admin/billing/reconcile` | admin | a full reconcile now: 202; `{"force_limits": true}` also has the next supervisor pass apply the stops the hold keeps back (which applies with billing off too); 400 |
| `POST /api/admin/billing/link` | admin | `{"user_id", "stripe_customer_id"}`: an orphan's customer made the account's, stamped and synced: 200; 400; 404; 503 |

`DELETE /api/admin/users/<id>` answers 409 `active_subscription` while a subscription of
the account still grants (Stripe would bill an account that is gone). The admin's user
list and detail carry each account's entitlement, live servers and subscriptions; the
detail and `PUT /api/admin/users/<id>/comp` carry the service's sold `capacity` against
what the fleet sells (`oversold` when an admin's grant, or a lowered maximum, took it
past).

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
  when it connects to NovaWorld, so every web request carries them as cookies.
  `GET /NWStart.dll` answers a DFX2 client whose `NWCDKIID` is no wire key with the start
  page's `ACT` page (`dfx2_0_actlogin.htm`, "Please login to activate this product")
  instead of `OUT`. Its completed login (`GET /NWLogin.dll`) then carries
  `Set-Cookie: NWACT=<value>` for the posted `NWPSSK`: the response's `SET-COOKIE`
  lines land in the browser's one cookie jar, and the NovaWorld screen's idle update reads
  `NWACT` from it, writes it to the registry `CDKIID`, validates it, and clears it (the
  witness: the RE record's section 9). The registry write needs an administrator. An activated client, a JO client and a
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

## Infrastructure (the admin's view)

The site's admin page **Infrastructure** (`/admin/infrastructure`) shows the deployment as
it runs, read on web (`apps/novaworld_web/infra/`) in one route,
`GET /api/admin/infrastructure` (admin; `Cache-Control: no-store`). From the database,
which every replica reads alike: the lobby instances with their sessions and listings, the
fleet process that leads and the fleet's machines. From AWS, with web's task role
(`infra/infra_status.*`): each program's ECS service (its counts, deployments, task
definition against the recorded release, newest events), its running and recently stopped
tasks, the one-off tasks, the load balancer's target groups and their targets' health, the
database's and the cache's status, each machine's EC2 state and image, and, with hosting
on, the fleet cluster: hostd's daemon service and each machine as its container instance
(its EC2 instance, its `machine-id` and `kind` attributes, its image, whether its ECS agent
is connected, its tasks). A fleet roll's preview reads each machine's image from the same
read (`GET /api/admin/fleet/roll`, `hosted/README.md`).

The page is the service's hub. Each program has a card (its ECS service, and from
`GET /api/admin/programs` what its tasks use now and its tasks gone in 24 hours) opening the
program's page (`/admin/infrastructure/programs/<name>`: its tasks, the usage charts of all of
them or of one, and its log); hostd's card holds the fleet cluster's machines; the database,
the cache and the load balancer each open their CloudWatch charts
(`/admin/infrastructure/resources/rds|cache|nlb`, which say they are not available on the
local stack). The machines are one line here: their page is **Fleet** (`/admin/fleet`), with
what needs an admin (`GET /api/admin/attention`, also counted on the admin menu), a card per
machine opening its own page (`/admin/fleet/machines/<id>`: its facts against the release,
slots, trees, timeline, usage and hostd's log) and the roll's preview (`?tab=roll`). A hosted
server's admin page adds a Usage tab (its slot's CPU, memory, network and tick health) and a
Logs tab (each start's output); its owner sees neither, only where its start stands and why
its last run ended.

The AWS calls are curl's, signed by curl itself (`--aws-sigv4`), run the way billing's
Stripe calls are (`infra/aws_api.*`, XML replies read by `infra/aws_xml.*`), not the AWS
CLI, whose Python start would cost about a second of web's half vCPU per call. The
credentials are the task role's, read for each refresh and kept no longer, in each call's
owner-only curl config file. One read runs its calls at once and is kept 20 s, however
many admins look. A call that fails leaves its section's error (AWS's own code and
message) and the others stand. Without AWS (the local stack: no `ONNET_INFRA_CLUSTER`, or
no task role) nothing is called and the page says why; it never shows made-up data. Web's
task role holds these reads alone, scoped to the deployment's resources
(`infra/aws/programs.tf`, `infra_read`; the fleet cluster's, `infra/aws/fleet_agent.tf`).

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_INFRA_CLUSTER` | none | Web: the deployment's ECS cluster, whose services (the programs) and tasks the view reads. Unset (the local stack), AWS is not read. |
| `ONNET_INFRA_REGION` | none | Web: the region of every AWS call the view makes. |
| `ONNET_INFRA_TARGET_GROUPS` | none | Web: the load balancer's target groups, a JSON object of listener to target group ARN (`{"gate": "arn:...", "lobby": ..., "http": ..., "https": ...}`). |
| `ONNET_INFRA_DATABASE` | none | Web: the RDS instance's identifier. |
| `ONNET_INFRA_CACHE` | none | Web: the ElastiCache replication group's id. |
| `ONNET_INFRA_RELEASE_PARAMETER` | none | Web: the SSM parameter holding the release (`/opennova/<deployment>/release`), whose revisions say whether each program runs the release's. |
| `ONNET_INFRA_FLEET_CLUSTER` | none | Web: the fleet cluster (`opennova-<deployment>-fleet`), whose hostd service and container instances the view reads; unset or empty with hosting off, and nothing of it is read. |
| `ONNET_INFRA_LOAD_BALANCER` | none | Web: the network load balancer's ARN suffix (`net/<name>/<id>`), the dimension of its CloudWatch metrics (the usage view's `nlb`). |

### Usage

`GET /api/admin/usage?subject=server|machine|program|infra&id=<id>&range=48h|400d` (admin;
`infra/usage_routes.*`) answers one shape for every subject: `{"subject", "id", "range",
"step_seconds", "limit": {"cpu", "memory_bytes"}, "series": [{"at", "cpu_percent",
"memory_bytes", "net_in_bps", "net_out_bps"}], "now"}`, 1-minute points for 48 h or hourly
ones for 400 days, `cpu_percent` in percent of `limit.cpu`'s CPUs and `now` the newest
minute while it is at most 3 minutes old. A hosted server's (`hosted_server_samples`,
`_hourly`; its slot's limits) carry `tick_p99_ms` and `late_ticks` too, a machine's
(`fleet_machine_samples`, `_hourly`; its memory's limit, `disk_total_bytes`)
`disk_free_bytes`, and a program's (`program_samples`, `program_hourly`; `&task=` for one
task) add its tasks up minute by minute, the CPU in percent of their CPUs together. The
samples keep cumulative byte counters: a rate is the growth between two minutes over the
time between them, none across a gap past 5 minutes or where a counter went back; an hour's
is its bytes over the hour. `subject=infra&id=rds|cache|nlb` answers an AWS resource's
CloudWatch metrics instead (`infra/metrics_store.*`, `cloudwatch:GetMetricData`, kept 60 s):
`{"available", "unavailable", "error", "period_seconds", "statistics": {"<metric>":
"Average"|...}, "metrics": {"<metric>": [{"at", "value"}]}}`, the database's CPU, freeable
memory, connections, network and IOPS, the cache's primary node's CPU, engine CPU, memory,
network and connections, the balancer's processed bytes, active flows and each target
group's healthy hosts (`HealthyHostCount:<listener>`); the local stack's says why none.
`GET /api/admin/programs` lists every program the service runs: its tasks running and
desired and the revision its ECS service runs (none locally: running is the tasks
reporting), what its reporting tasks use now, `restarts_24h` (its tasks that reported in
the last 24 h and no longer do: a crash's, a deploy's replaced ones, a scale-in's) and each
task, the reporting ones and the ones ECS runs.

### Logs

`GET /api/admin/logs` (admin; `Cache-Control: no-store`; `infra/log_routes.*`) answers a
page of one log, newest last, from the store `ONNET_LOG_STORE` names (`infra/log_store.*`):
`?subject=server&id=<id>[&generation=<g>]` (a hosted server's start, its newest without a
number), `?subject=machine&id=<id>` (a machine's hostd) or
`?subject=program&id=<program>[&task=<task id>]` (a program's task, or every task of it),
with `&before=<cursor>` (the page before an earlier one), `&limit=` (1 to 500, 200) and
`&q=` (the lines holding that text). The reply: `{"subject", "id", "generation", "task",
"source"` (`cloudwatch` or `file`), `"group", "stream"` (where it read), `"lines"` (each
`{"at", "text"}`, `at` ISO 8601 with milliseconds or null), `"before"` (the older page's
cursor, null for none)`}`; 404 for no such server, start, machine, program or task, 502
`logs_unavailable` with AWS's words, 503 `no_log_store`. What it reads comes from rows:
the stream a start's row names (`hosted_server_starts.log_stream`, in `/servers`), the
machine's (`fleet_machines.agent_log_stream`, in `/machines`), a program's task that
sampled itself (`program_samples`; `<program>/<program>/<task id>` in `/service`); no
request names a group, a stream or a path. A known stream's newest lines are CloudWatch
Logs' `GetLogEvents`, a text filter or a program's every task `FilterLogEvents` over
windows walking back from the newest (15 minutes, then four times as long, to 31 days); a
read makes at most 8 calls in 10 s, a read like one in the last 3 s is answered from
memory, and an admin has two reads in flight at most (60 at once, then two a second). The
local stack's directory store reads the same subjects' files (`servers/<id>/g<g>.log`,
`machines/<id>/hostd.log`, `programs/<program>/<task>.log`) backwards from their end.

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
`ONNET_SITE_URL` ("Account email and password resets" below). The job pools (the
worker's, the data program's) take `ONNET_WORKER_THREADS` ("Background jobs" above).
Seeding: `SEED_DEV_USERS=1` applies the dev-only `0002_dev_users.sql` (the
`test`/`foo` accounts); leave it unset in production. Build + run via Docker:
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
    fleet's election hold keys of their own, so neither waits on a start;
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
  ([`backend/README.md`](../../backend/README.md)). A development database is recreated
  with `scripts/dev_postgres.sh reset`; a deployment's, with DEPLOY.md's "Resetting the
  service database".

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
- **Session.** 32 CSPRNG bytes, the cookie `opennova_session=<64 hex>; Path=/;
  Max-Age=2592000; HttpOnly; SameSite=Lax`, or with `ONNET_COOKIE_SECURE`
  `__Host-opennova_session=...; Secure` (so a sibling subdomain cannot plant one).
  `web_sessions` stores only the token's SHA-256 with the account, the address and the
  user agent; an account keeps its newest 20. A session lasts 30 days from its last use,
  renewed (row and cookie) at most once a minute; `web_sessions.prune` deletes the expired.
- **Revocation.** A deleted player takes its sessions and known login addresses. An admin
  password reset or a status other than `active` moves the account's credentials
  generation and, in one transaction, deletes its website sessions, its retail login's
  `active_user_sessions` row and its hosting keys ("Listed servers and owners"); the HTTP
  layer drops its retail login pins from Redis for every replica. So a reset locks out
  whoever held the old password and a lifted ban revives nothing. The known login
  addresses stay, so the owner never meets the per-username cap while someone else
  guesses. A session whose account went inactive any other way is refused and deleted when
  next used; `active` alone is active (an empty status is refused by the route and a
  `CHECK`).
- **The credentials generation.** A login reads the account's generation with its
  password hash, before bcrypt runs, and everything it then grants is written only
  while the account is still at it, tested by the insert itself: the website session
  (also only while the account is active), the retail login's `active_user_sessions`
  row (likewise) and the known address. So a reset or a ban that commits while a
  password is being checked lets that login grant nothing: the site answers the
  generic 401, the game login a bad password's page. The retail login's session row
  and its pin carry the generation they were made under and count only while the
  account still has it, so a reset ends them even where nothing deletes them.
- **Usernames and handles.** A username: 1..64 bytes, no control byte; a handle
  (`nwhandle`, the player's name in the game and the `NWHANDLE` / `CHAR` cookies): 1..30
  bytes, no control byte, the longest a stock host reads back whole (`auth.h`,
  `kHandleMaxBytes`), checked on registration, the admin create and every admin update
  (400 `invalid_field`). Every client-supplied string in any listener's log line goes
  through `loggable()` (`loggable.h`): control bytes as `\xNN`, cut at 64 bytes.
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
  `template_engine.*`) goes out with `& < > " '` as entities, so a request's values, a
  server's name and a maintenance message add no markup; a stock client's page parser
  turns them back in a page's text (inside a tag it decodes none, and the values there are
  bare page names).
- **Login answers.** An unknown username and a wrong password get the same 401
  `invalid_credentials`, and an unknown name still costs one bcrypt run, so neither the
  body nor the timing enumerates accounts. Only the right password learns that an
  account is banned (403 `account_banned`) or otherwise inactive (403
  `account_restricted`), the game login's NWEC11 / NWEC12. A body that is not a JSON
  object, a missing or mistyped field and a username over 64 bytes are 400s before any
  brake. A new login ends the session the browser held. Maintenance mode does not
  close the site login (an admin turns it off from there).
- **CSRF.** Every state-changing request a session cookie authenticates needs
  `X-OpenNova-Request: 1`, else 403 `csrf_header_required`, and so do `POST /api/login`,
  `/api/logout` and `/api/register`. A cross-site form cannot set the header, and a
  cross-origin script would need a CORS preflight the server never grants. Beyond
  `SameSite=Lax`, it covers same-site origins (`staging.<domain>`) and the routes no
  cookie authenticates (a forged login, logout or registration). It is checked before
  any rate-limit bucket; the web client sends it on every request (`web/src/api/client.ts`).
- **The login brake** (token buckets in Redis, `rate_limiter.*`: one script per draw, so
  every replica draws on the same bucket exactly; a key expires when its bucket would be
  full again), run by `POST /api/login` and the retail `POST /NWLogin.dll`. The two keep
  their address-keyed buckets apart: the site's keyed on the address its trusted proxy
  names, the retail login's on its TCP peer or the retail port's own proxy's
  (`ONNET_RETAIL_PROXY`), so nothing the site sees can brake a retail login. An IPv6
  address counts as its /64 (its /56 in the (username, address) buckets).
  - every well-formed attempt, per route and client address: 20 at once, then one
    every 3 s;
  - each password check, per route, username and client address: 10, then one every
    30 s, so a guesser's address is shut out while the owner elsewhere never meets it;
  - each password check per username, from any address and either route: 30, then one
    every 2 s, except from the account's known addresses (a right password from them
    in the last 30 days, `login_addresses`, at most 32 an account).
  A check takes its tokens before bcrypt runs and gets them back when the password is
  right, so the caps are exact however many checks run at once and a right password
  costs nothing; a full per-username cap locks out only logins from addresses the
  account has not used. `/api/login` refuses with 429
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

- **The address.** `POST /api/register` requires `email` (400 `missing_field`).
  `normalize_email` (`auth.h`) takes at most 254 bytes with exactly one `@`, a local part
  and a domain of two or more labels, no space or control byte (400 `invalid_field`), and
  stores it with its ASCII letters lowercased, so one address names one account (400
  `email_exists`; a `CHECK` refuses an upper-case or empty one). The admin's create takes
  an optional `email` under the same rules. Older accounts, service accounts and admin-made
  ones without one have none (`NULL`). Every admin user route answers `email`, read-only.
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
  ("Background jobs" above) on their own connection, the subject and bodies the job's
  secret, so a mail outlives a restart of the process that asked for it. A worker sends
  it at most once and logs one line (`[mail] sent <template> to '<address>' ...` or
  `[mail] WARN ... not sent: <why>`), never the body; a mail the queue cannot take is
  logged and dropped, never the request's failure. The `SesMailer` runs the AWS CLI's
  `sesv2 send-email` through the command runner on the worker's task role, the request in
  an owner-only JSON file deleted once the CLI ends, so no address or link is on a command
  line. Infrastructure: the domain's SES identity, its DKIM and MAIL FROM records and the
  worker's permission (`infra/aws/mail.tf`; DEPLOY.md, "Outbound mail (SES)"). Web sends
  none, so its task role holds none.

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_MAIL_FROM` | none | The sender's address, in lower case (`support@<domain>`; infra.env names it). Unset, mail is off: each mail is logged as not sent. |
| `ONNET_MAIL_REGION` | none | Amazon SES's region (infra.env's). |
| `ONNET_SITE_URL` | none | The site's origin the mails link to (billing reads it too). Mail is on only with all three; a sender without a site URL or a region boots with mail off and a warning naming why. |

## Build layout and tests

This directory builds the static library `opennova_novaworld_common`, what more than one
program uses; each program's directory builds its own code as
`opennova_novaworld_<program>_core` and its `main.cpp` as `opennova-novaworld-<program>`.
All of it, and its tests, exist only with `OPENNOVA_NOVAWORLD_SERVICE=ON` (the root CMake
option, off by default; the Linux CI jobs and the images turn it on). The HTTP half (the
website and retail routes' support, the jobs, `hosted/`, `billing/`) builds only under
`BUILD_NOVAWORLD_HTTP`, which defines `OPENNOVA_HTTP_ENABLED` PUBLIC: web, the fleet program,
the worker and the data program need it, the gate and the lobby build with or without it.

The tests live in `tests/novaworld/` (`service_tests.cmake`); each says what it pins.
Every test that opens a database (ctest label `pg`, `opennova_add_pg_test`) makes its own
databases, one per case, on the PostgreSQL server `-DOPENNOVA_TEST_PG=<uri>` names
(`tests/common/test_database.h`); the HTTP layer's (label `redis`) also need the Redis
server `-DOPENNOVA_TEST_REDIS=<url>` names, each in a key namespace of its own
(`tests/common/test_redis.h`). Without the server a test prints `SKIP: needs a PostgreSQL
server` (or `a Redis server`) and exits 77, which `--suite core` counts as a failure.
`scripts/dev_postgres.sh up` and `scripts/dev_redis.sh up` print the values for local
servers, and the Linux CI jobs run both beside the build. The route harness,
`opennova_novaworld_web_http_routes` (`tests/novaworld/http_routes_test.cpp`), drives the
Crow listener in process on port 0; the six binaries also run whole as child processes
(`opennova_novaworld_programs`, `opennova_novaworld_signal_shutdown`, `nw_programs_e2e`),
and the local stack's smoke test runs the deployed shape (`scripts/dev_stack.sh smoke`,
CI's `local-stack.yml`).
