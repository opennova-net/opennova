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

The website's sessions (next section) take three more:

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_COOKIE_SECURE` | off | `1` makes the session cookie `__Host-opennova_session` with `Secure`. Set it wherever browsers reach the site over https: prod sits behind Cloudflare's TLS while nginx and this server speak plain http, so no request header can tell (`deploy/env/app.prod.env` sets it). Leave it off for the http://localhost dev site, where a Secure cookie is never sent back; the cookie is then `opennova_session`. |
| `ONNET_TRUSTED_PROXIES` | none | Comma-separated exact addresses of the reverse proxies whose `X-Real-IP` (else the last `X-Forwarded-For` hop) names the client, for the rate limits and the session's recorded address. Prod's nginx (`web/nginx.conf`, host networking) reaches the server from `127.0.0.1`; behind Cloudflare it takes the visitor's address from `CF-Connecting-IP`, for Cloudflare's own ranges only (`web/cloudflare-realip.conf`, regenerated by `web/cloudflare-realip.sh`). Unset, the TCP peer is the client and those headers are ignored. |
| `ONNET_BOOTSTRAP_ADMIN` | none | An existing account's username, given the `admin` role at boot while no account is an admin; with an admin present it does nothing (logged either way, a name no account has is a warning). How the first site admin is made; later ones through `PUT /api/admin/users/<id>` `{"role":"admin"}`. Pass it through the vault-injected secrets file, never a committed one, and remove it once it ran (DEPLOY.md). |

## Website sessions and roles

The NovaWorld site logs players in with their NovaWorld account (`web_access.*`,
`web_session.*`, `rate_limiter.*`, `http_json.h`; migration
`0008_web_sessions_and_roles.sql`). The retail `NW*.dll` login keeps its own flow and
shares only the login brake below.

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
  session writes at most once a minute. The main tick's sweep prunes expired rows.
- **Revocation.** A deleted player takes its sessions. An admin password reset or a
  status other than `active` (`PUT /api/admin/users/<id>`) deletes the account's
  sessions in the same transaction, so a reset locks out whoever held the old one and
  a lifted ban revives nothing; a session whose account went inactive any other way
  is refused and deleted when next used. An empty status is refused; one stored empty
  reads as `active` everywhere.
- **Login answers.** An unknown username and a wrong password get the same 401
  `invalid_credentials`, and an unknown name still costs one bcrypt run, so neither the
  body nor the timing enumerates accounts. Only the right password learns that an
  account is banned (403 `account_banned`) or otherwise inactive (403
  `account_restricted`), the game login's NWEC11 / NWEC12. A username over 64 bytes is
  400 before any brake. A new login ends the session the browser held. Maintenance mode
  does not close the site login (an admin turns it off from there).
- **CSRF.** Every state-changing request (any method but GET, HEAD, OPTIONS) that a
  session cookie authenticates needs `X-OpenNova-Request: 1`, else 403
  `csrf_header_required`, and so do `POST /api/login`, `/api/logout` and
  `/api/register`. A cross-site form cannot set the header, and a cross-origin script
  that sets one needs a CORS preflight the server never grants (it sends no
  `Access-Control-*` header). `SameSite=Lax` already keeps the cookie off cross-site
  POSTs; the header also covers same-site origins such as `game.<domain>` and the
  routes no cookie authenticates: a forged login would sign the visitor into the
  forger's account, a forged logout sign them out, and a forged registration (a
  `text/plain` form can carry a JSON body) create accounts from visitors' addresses.
  The header is checked before any rate-limit bucket, so forged requests drain none.
  The web client sends it on every request (`web/src/api/client.ts`).
- **The login brake** (in-memory token buckets, mutex-guarded, at most 10,000 keys per
  limiter; refilled buckets are dropped and a full table evicts the least recently
  used), shared by `POST /api/login` and the retail `POST /NWLogin.dll`:
  - every attempt, per client address: 20 at once, then one every 3 s (counted before
    the body is read);
  - failed attempts only, per (username, client address): 10, then one every 30 s, so
    a guesser's address is shut out while the owner elsewhere never meets it;
  - failed attempts only, per username from any address: 30, then one every 2 s, which
    one address alone can neither empty nor hold empty.
  A right password drains neither failure bucket. `/api/login` refuses with 429
  `rate_limited` and `Retry-After` in seconds. `/NWLogin.dll` refuses with its own
  failure reply, the msgbase page (`jop_2_msg.htm`) a stock client shows for a bad
  password, carrying "Too many failed logins. Please try again later." (no retail
  throttling reply is witnessed). `POST /api/register` takes 10 then one every 3 min
  per address.
- **Roles and admin routes.** `players.role` is `player` (every account) or `admin`.
  Every `/api/admin/*` route accepts the Bearer `ADMIN_API_TOKEN` (machine callers and
  the deploy toolbox; no CSRF header) or an admin-role session (with the CSRF header on
  a state change); a player's session gets 403 `forbidden`, no credential 401
  `{"error":"unauthorized"}` with the Bearer challenge. The role is read on every
  request, so a demotion takes effect on the live session. Each granted admin state
  change is logged with its actor (`token`, or the session's username). The last admin
  may demote themselves: the Bearer token can always promote again, and with no admin
  left `ONNET_BOOTSTRAP_ADMIN` works once more.

## Build layout and tests

Every source but `main.cpp` builds as the static library
`opennova_novaworld_server_core` (mirroring `opennova_serve_core`); the exe is
`main.cpp` over it. The HTTP sources (`http_listener`, `web_access`, `web_session`,
`rate_limiter`, `template_engine`, `session_store`, `auth`, `catalog_repository`) join
the core only under
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
carry the sibling ports its config names. The website sessions' store and the rate
limiter also have Crow-free ctests that compile those sources straight in and run on
every build: `web_session`
([`tests/novaworld/web_session_test.cpp`](../../tests/novaworld/web_session_test.cpp),
the store and the roles on a migrated in-memory database) and `rate_limiter`
([`tests/novaworld/rate_limiter_test.cpp`](../../tests/novaworld/rate_limiter_test.cpp),
the buckets on a hand-stepped clock and the trusted-proxy client address); the
route harness drives the cookie, CSRF, rate-limit and admin-credential paths over
the wire.
