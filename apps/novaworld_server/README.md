# NovaWorld server

The standalone NovaWorld server: one process, three listeners, one SQLite
database. Implements the matchmaking/backend half of NovaWorld (the gate, the
session handshake, the browser/host/play container services, and the legacy
`NW*.dll` HTTP flow). Protocol record: [`docs/net/novaworld-net-re.md`](../../docs/net/novaworld-net-re.md).

## Listeners

| Listener | Port | Protocol |
| --- | --- | --- |
| `GateListener` | UDP 7597 | `novaworld_gate` probe → gate response (NWU `GATEAPI`); the same port is the advertised `POSTIPPORT`, the sink for a retail host's plaintext status heartbeat (`Lobby_UpdateServerInfo`), folded into `active_hosts` by `HostKey` |
| `NwUdpListener` | UDP 64206 | NWU framing -> session handshake (0x41/0x42/0x43/0x46) -> PN dispatch: NOVAWORLDUDP containers or JointOperations game runtime |
| `HttpListener` (Crow, `BUILD_NOVAWORLD_HTTP`) | TCP 8080 | `NW*.dll` routes + `/api/*` backend + web portal fallback |

## Thread model

- **The main thread owns the boot and the tick loop.** It migrates and seeds the
  database, then runs `manager.tick(now)` every `tick_interval_ms` and on the same
  tick flushes the `UnknownTracker` and runs the throttled host-staleness sweep.
- **Each listener runs on its own internal thread(s).** They mutate shared state
  through the `ConnectionManager` verbs (add / refresh / drop) and the
  `NwUdpListener`'s `lobby_states_` map (guarded by `lobby_states_mu_`); the
  higher-level invariants (the per-peer session-state map, the unknown
  accumulator) carry their own mutexes.
- **Every thread has its own SQLite connection.** A `db::Database` is used by one
  thread at a time: its last insert id, its error text and any open transaction
  belong to the connection. `main()` owns one `db::ConnectionPool` on
  `DATABASE_PATH`; the main thread and the gate and NW UDP receive threads each
  hold a connection for their lifetime (the listeners lease theirs in `start()`,
  so a database that cannot be opened stops the boot), each HTTP request leases
  one for the handler, and `erase_lobby_state` (run from the NW UDP thread and
  from the main thread's `on_lost`) leases one per call. The database is WAL, so
  connections read while one writes, and the 5 s busy timeout makes a second
  writer wait. A write that spans statements (`clear_all`, `replace_roster`,
  `apply_status_blob`, the lobby's host row with its roster, a registration's
  checks and insert) is one `db::Transaction`, and a read that spans them
  (`/api/hosts` and the `.gsb` feeds: the rows, then each roster) is one
  `db::ReadSnapshot`, so a host never goes out beside another write's roster.
- **The `UnknownTracker` records from listener threads** into an in-memory,
  mutex-guarded accumulator and is flushed to the DB only on the main tick — never
  per packet.
- **Each listener's `start()` holds its port before the boot goes on.** The gate and
  NW UDP listeners bind their socket in `start()` and move it into the receive
  thread (with the thread's lease), so the port they report is the one they serve,
  also with port 0 (the OS's pick). The HTTP listener's `start()` returns once Crow
  serves (Crow's first tick tells it, with the port Crow bound). A bind that fails
  stops the boot.
- **The listeners start HTTP first, then NW UDP, then the gate,** so each one starts
  knowing the ports its siblings serve and advertises those: the NW UDP SessionInit's
  web domain names the HTTP port, the gate's `UDPNOVAWORLD` and `STARTUPURL` the NW UDP
  and HTTP ports, and the HTTP menus' `HOST_URL` / `GSB_SERVER` the HTTP listener's own.
  With nonzero ports these are the configured ports, byte for byte; with port 0, the
  OS's picks. The gate, a client's first contact, answers only once what it names
  serves.
- **`main()` owns SIGINT and SIGTERM** (Ctrl+C; `docker stop` sends SIGTERM). The
  handler only raises the shutdown flag and puts the signal's default action back,
  so the same signal again ends a shutdown that is stuck. Not when the server is a
  container's PID 1: the kernel drops a default-action SIGINT or SIGTERM sent to a
  PID namespace's init (pid_namespaces(7)), so the compose files run it under
  Docker's init (`init: true`), which is PID 1 and forwards both signals; a bare
  `docker run` of the image needs `--init` for the same. The tick loop sees the flag
  within one tick and stops in order: the HTTP listener (Crow's threads joined, a
  held-open request included), then the gate and NW UDP receive threads (each
  returns its lease), then the connections (`Shutdown`). As `main()` returns, its
  locals go in reverse declaration order, so the main lease and then the pool
  (declared first) close last, and the last close checkpoints the WAL. The HTTP
  listener clears Crow's own signal set (`signal_clear()`), which would otherwise
  take both signals and stop only Crow.

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
which removes the in-memory per-peer session state and the matching `active_hosts` /
`host_players` rows (the host row by RID, the player row by peer address). So a host
that quits, times out, or is replaced disappears from the browser the same way.

A **backstop sweep** runs on the main tick (`host_sweep_interval_ms`, default 30 s):
it prunes `active_hosts` rows whose `updated_at` (refreshed by every
`ClientHostUpdate` heartbeat) is older than `host_stale_window_ms` (default 5 min).
This only catches rows the normal teardown somehow missed; `host_players` rows
cascade. At boot the table is wiped entirely (previous-run cleanup).

## Pushing statements to a listed server

The service can send a hosting connection retail's two administrative statements over
its own NovaWorld session: `ServerCommand` (one `Cmd` verb line; the host runs it, so
the service can punt a player, rename the server, change its message, cycle the map
and so on) and `ServerStopHosting` (`MsgCode` / `MsgParam1` / `MsgParam2`).
`NwUdpListener::push_server_command` / `push_stop_hosting` resolve the RID to the
hosting connection under `lobby_states_mu_` and queue the statement; the receive
thread, which owns the socket, frames it on its next pass exactly as a reply (one
reliable record on the connection's sequencing, retained until the client's ACK
covers it) and answers a 0x44 for it from those records. Between receive batches the
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
service also takes the server out of the browser as the statement goes out, since the
host answers it with no `ClientStopHosting`. The admin routes (Bearer
`ADMIN_API_TOKEN`):

| Route | Body | Replies |
| --- | --- | --- |
| `POST /api/admin/hosts/<rid>/command` | `{"verb": "SetServerName", "target": "None", "args": ["New Name"]}` (`target` one of `None`, `ByIndex`, `ByIpAndPort`, `ByName`, `ByPCID`) | 202 queued (with the composed `cmd`); 400 when `server_command_text` refuses the line or an arg holds a control byte (below 0x20, or 0x7F, which a host would save into its `game.cfg` or relay in chat), with the reason; 404 unknown RID; 409 not hosting |
| `POST /api/admin/hosts/<rid>/stop` | none | 202 queued (MsgCode 7, `NWUSERVERMSGCODE_NOVAWORLDSYSOPPUNT`): ends a stock host's running NovaWorld match for everyone and drops the host from NovaWorld (the browser row at once, its session reset with the match's exit); 404; 409 |

## Configuration

All via environment (`server_config.cpp`): `ONNET_PUBLIC_HOST`,
`ONNET_{GATE_UDP,NW_UDP,HTTP}_PORT`, `DATABASE_PATH`, `MIGRATIONS_DIR`, `SEED_DIR`,
`TEMPLATES_DIR`, `STATIC_DIR`, `WEB_DIST_DIR`,
`TICK_INTERVAL_MS`, `HOST_SWEEP_INTERVAL_MS`, `HOST_STALE_WINDOW_MS`,
`ADMIN_API_TOKEN`. Gate extras (all optional): the MET endpoint
`ONNET_MET_IP`, `ONNET_MET_PORT`, `ONNET_MET_LABEL`, `ONNET_MET_PING`,
`ONNET_MET_EXT` (emitted as the METIPADDRESS family when `ONNET_MET_IP` is set)
and the GLSVSS leg `ONNET_GLSVSS_REQUEST`, `ONNET_GLSVSS_RIMS`,
`ONNET_GLSVSS_AGRMS` (the gate VARs) plus `ONNET_GLSVSS_RESULTS` (what
`ClientGLSVSSRequest` is answered with). The gate response carries only the
nineteen keys the retail parser recognises. The host/join reflection override
for a docker bridge or NAT (leave unset in production): `ONNET_CLIENT_REFLECT_IP`,
`ONNET_CLIENT_REFLECT_{GATE,NOVAWORLD}_PORT`.
Seeding: `SEED_DEV_USERS=1` applies the dev-only `0002_dev_users.sql` (the
`test`/`foo` accounts) — leave unset in production. Build + run via Docker:
[`Dockerfile`](Dockerfile) and the compose files under [`deploy/`](../../deploy/).

## Build layout and tests

Every source but `main.cpp` builds as the static library
`opennova_novaworld_server_core` (mirroring `opennova_serve_core`); the exe is
`main.cpp` over it. The HTTP sources (`http_listener`, `template_engine`,
`session_store`, `auth`, `catalog_repository`) join the core only under
`BUILD_NOVAWORLD_HTTP`, which also defines `OPENNOVA_HTTP_ENABLED` PUBLIC so
`main.cpp` starts the listener. The route harness, ctest
`opennova_novaworld_server_http_routes`
([`tests/novaworld/http_routes_test.cpp`](../../tests/novaworld/http_routes_test.cpp)),
drives the Crow listener in-process on port 0 (the port it reads back from
`bound_port()`, and the port a rendered page's `HOST_URL` / `GSB_SERVER` carry)
over a `db::ConnectionPool`
on a SQLite file under `backend/migrations`, with `apps/common`'s `http_exchange`. It exists only
with `BUILD_NOVAWORLD_HTTP=ON` and runs in `net-linux.yml` and `ci.yml`'s
test-linux. The ctest `opennova_novaworld_server_signal_shutdown`
([`tests/novaworld/server_signal_shutdown_test.cpp`](../../tests/novaworld/server_signal_shutdown_test.cpp),
POSIX builds) runs the real binary on a temp database with every port 0, sends
SIGINT and then SIGTERM to a fresh run, and expects exit 0, the shutdown legs
logged in order and no WAL file left; with the HTTP layer it reads the HTTP port
from the `[http] listening on :<port>` line and holds a half-sent request open
across the signal. On Linux a third run checks that the first signal puts the
default action back and the same signal again ends a shutdown still pending. A
last run probes the gate and opens a NovaWorld session: `UDPNOVAWORLD` names the
NW UDP listener's logged port and, with the HTTP layer, `STARTUPURL` and the
SessionInit's web domain the HTTP listener's. The ctest
`gate_listener` ([`tests/novaworld/gate_listener_test.cpp`](../../tests/novaworld/gate_listener_test.cpp))
starts the real `GateListener` on port 0 and checks that its reported port answers
a probe and comes back as POSTIPPORT, and that `UDPNOVAWORLD` and `STARTUPURL`
carry the sibling ports its config names.
