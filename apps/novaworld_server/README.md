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
host answers it with no `ClientStopHosting`.

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
  checked again at push time: the push carries the caller's account to
  `NwUdpListener`, which resolves the RID only to a connection whose
  `LobbyState::host_user_id` is that account, under its lobby lock, and the receive
  thread drops the statement if the connection's owner changed before it went out (a
  re-host whose key names another account or none). That miss is the same 404. An
  admin password reset, an inactive status or a delete also takes the account off its
  listings' in-memory owner (`NwUdpListener::forget_owner`, beside the retail login
  pins), so a push the account queued just before is dropped there too, not sent.
- **The rate limit.** Each owner push, command or stop, takes one token from the
  account's bucket (`HostRoutes::kOwnerPushPerUser`, in memory): 8 at once, then one
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
  outcome: `queued`, `not_hosting`, `unknown_rid` (the listener's push-time miss),
  `no_listing_of_theirs` (the route's own look-up), `invalid_command`,
  `verb_not_allowed` or `rate_limited`.

## Listed servers and owners

Every listed server can name the account that hosted it (`active_hosts.host_user_id`),
through the HOSTKEY a stock client fetches from `/NWHost.dll` and copies into its `Host`
var list's `HostKey`. Service policy throughout: how the retail service checked a
HostKey is unwitnessed.

- **The key.** `/NWHost.dll`'s first call mints the 48-letter A..P key (24 OS CSPRNG
  bytes, a nibble each) and, when the request's own retail login names an account,
  records the key for it in `host_keys` (migration `0009_host_keys.sql`), as its
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
  its mission's start; opennova-nw-lister and `opennova-serve` send theirs right after
  the host result, which a fresh stock host does not, D-NET-465). A key backs one
  listed server at a time: another connection
  presenting it while that listing lives links to no account. A dropped connection
  parks its lobby state and RID (`kParkedLobbyTtlMs`, 240 s), and its re-host after the
  NWU reconnect (`CurrentlyHosting=1`) re-sends its kept `Host` list, key included, so
  it claims the same key under the same RID and keeps the owner; a key whose listing
  is gone can be claimed again. A boot releases every claim with the previous run's
  rows.
- **Expiry.** A key lasts 24 h from its last use, its claim or the main tick's sweep
  while a listed server holds it (counted at most once an hour); the sweep deletes the
  rest.
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
    live claim (`hostdb::listing_owned_by`), not the row. A reset, a status change or a
    delete made through `PUT` / `DELETE /api/admin/users/<id>` also takes the account
    off its listings' in-memory owner (`NwUdpListener::forget_owner`), so an owner push
    it queued just before is dropped by the receive thread rather than sent.
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
  `--credentials` and `opennova-nw-lister` without an account keep listing.

| Route | Access | Replies |
| --- | --- | --- |
| `GET /api/me/hosts` | a website session (`require_user`) | `{"hosts": [...]}`, the caller's listed servers, oldest first: `rid`, `gsid`, `game`, `server_name`, `mission_name`, `game_type`, `players`, `max_players`, `host_ip`, `host_port` (the endpoint joiners are sent to), `peer_ip` (the address its session comes from), `created_at`, `updated_at` (UTC), `msg` (the server's message) and `roster`, the host's PlayerList as of its last update (`slot`, `player_name`, `team`, `type`; the slot is what a player verb's `ByIndex` target names); 401 with no live session; `Cache-Control: no-store` |
| `GET /api/admin/hosts` | admin (the Bearer token or an admin session) | `{"count", "hosts": [...]}`, every listed server with the same fields plus `owner`, `{id, username}` or `null` |

The public `/api/lobbies`, `/api/hosts` and the GSB name no owner. The owner pushes to
its own servers through `POST /api/me/hosts/<rid>/command` and `/stop` ("Pushing
statements to a listed server" above).

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
`ONNET_CLIENT_REFLECT_{GATE,NOVAWORLD}_PORT`. The listing policy:
`ONNET_REQUIRE_HOST_KEY=1` lists no host whose HostKey names no account (default off:
listed with no owner; "Listed servers and owners" above).
Seeding: `SEED_DEV_USERS=1` applies the dev-only `0002_dev_users.sql` (the
`test`/`foo` accounts) — leave unset in production. Build + run via Docker:
[`Dockerfile`](Dockerfile) and the compose files under [`deploy/`](../../deploy/).

The website's sessions (next section) take four more:

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_COOKIE_SECURE` | off | `1` makes the session cookie `__Host-opennova_session` with `Secure`. Set it wherever browsers reach the site over https: prod sits behind Cloudflare's TLS while nginx and this server speak plain http, so no request header can tell (`deploy/env/app.prod.env` sets it). Leave it off for the http://localhost dev site, where a Secure cookie is never sent back; the cookie is then `opennova_session`. |
| `ONNET_TRUSTED_PROXIES` | none | Comma-separated exact addresses of the reverse proxies whose `X-Real-IP` (else the last `X-Forwarded-For` hop) names the client, for the website's rate limits and the session's recorded address. Prod's nginx (`web/nginx.conf`, host networking) reaches the server from `127.0.0.1`; behind Cloudflare it takes the visitor's address from `CF-Connecting-IP` for Cloudflare's own ranges only (`web/cloudflare-realip.conf`, regenerated by `web/cloudflare-realip.sh`). That header is not proof: a Cloudflare Worker on any zone can fetch the origin with a value of its choosing until edge authentication is on (DEPLOY.md), so these addresses brake only the website's routes, never the retail login. Unset, the TCP peer is the client and those headers are ignored. |
| `ONNET_API_PROXY_ONLY` | off | `1` answers every `/api` request (`/api` and everything under it, a route or not) with 403 `{"error":"proxy_required"}` unless its TCP peer is one of `ONNET_TRUSTED_PROXIES`, so the website's API is reached only through the proxy that applies the edge checks, never straight on the HTTP port. The retail routes a stock client reaches on that port directly (the `NW*.dll` pages, the `.gsb` feeds, the templates and static files) stay open. Prod sets it (`deploy/env/app.prod.env`): its nginx reaches the server from `127.0.0.1`, the one trusted proxy. With it on and no trusted proxy, every `/api` request is refused (logged at boot). Leave it off for the dev site. |
| `ONNET_BOOTSTRAP_ADMIN` | none | An existing account's username, given the `admin` role at boot while no account is an admin; with an admin present it does nothing (logged either way, a name no account has is a warning). How the first site admin is made; later ones through `PUT /api/admin/users/<id>` `{"role":"admin"}`. Pass it through the vault-injected secrets file, never a committed one, and remove it once it ran (DEPLOY.md). |

Migrations: each boot applies the `backend/migrations` files (`MIGRATIONS_DIR`) that
`_schema_migrations` does not name yet. It goes by file name only, so a migration
edited in place never runs again on an existing database. The service database is
disposable: a PR that edits one says "the service DB must be reset" in its notes, and
its deploy is `./deploy/run.sh app reset-db` ([DEPLOY.md](../../DEPLOY.md),
"Resetting the service database").

## Website sessions and roles

The NovaWorld site logs players in with their NovaWorld account (`web_access.*`,
`web_session.*`, `rate_limiter.*`, `http_json.h`; migrations
`0008_web_sessions_and_roles.sql` and `0010_credentials_generation.sql`). The retail
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
  session writes at most once a minute. The main tick's sweep prunes expired rows.
- **Revocation.** A deleted player takes its sessions and its known login addresses.
  An admin password reset or a status other than `active` (`PUT
  /api/admin/users/<id>`) moves the account's credentials generation
  (`players.credentials_gen`, migration 0010) and deletes its website sessions, its
  retail login's `active_user_sessions` row and its hosting keys in the same
  transaction (a server it listed loses its owner at its next refresh; "Listed servers
  and owners"), and the HTTP layer drops the retail login's
  `PERSISTENTEXPRESSLOGINDATA` pins to the account (which would resume a retail login
  with no password), so a reset locks out whoever held the old one and a lifted ban
  revives nothing. The known login addresses stay: forgetting them would bare the
  owner to the per-username cap just when someone else is guessing, and the
  per-(username, address) bucket still brakes a guesser at one of them. A session
  whose account went inactive any other way is refused and deleted when next used. An
  empty status is refused, by the update (400 `invalid_field`) and by the schema (a
  `CHECK` in migration 0003), so `active` alone is active everywhere.
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
  POSTs; the header also covers same-site origins such as `game.<domain>` and the
  routes no cookie authenticates: a forged login would sign the visitor into the
  forger's account, a forged logout sign them out, and a forged registration (a
  `text/plain` form can carry a JSON body) create accounts from visitors' addresses.
  The header is checked before any rate-limit bucket, so forged requests drain none.
  The web client sends it on every request (`web/src/api/client.ts`).
- **The login brake** (in-memory token buckets, mutex-guarded, at most 10,000 keys per
  limiter; refilled buckets are dropped and a full table evicts the least recently
  used), run by `POST /api/login` and the retail `POST /NWLogin.dll`. The two keep
  their address-keyed buckets apart: the site's key on the client address its trusted
  proxy names, which a Cloudflare Worker can forge until edge authentication is on;
  the retail login's on its TCP peer (a stock client reaches the HTTP port directly:
  the gate's `startupurl` names `ONNET_PUBLIC_HOST` and the port, through no proxy),
  so nothing the site sees can brake a retail login. An IPv6 address counts as its
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
  Every `/api/admin/*` route accepts the Bearer `ADMIN_API_TOKEN` (machine callers and
  the deploy toolbox; no CSRF header) or an admin-role session (with the CSRF header on
  a state change); a player's session gets 403 `forbidden`, no credential 401
  `{"error":"unauthorized"}` with the Bearer challenge. The role is read on every
  request, so a demotion takes effect on the live session. Each admin state change it
  lets through is logged once its handler has answered, with the actor (`token`, or the
  session's username) and the reply's status. The last admin
  may demote themselves: the Bearer token can always promote again, and with no admin
  left `ONNET_BOOTSTRAP_ADMIN` works once more.

## Build layout and tests

Every source but `main.cpp` builds as the static library
`opennova_novaworld_server_core` (mirroring `opennova_serve_core`); the exe is
`main.cpp` over it. The HTTP sources (`http_listener`, `host_routes`, `web_access`,
`web_session`, `rate_limiter`, `template_engine`, `session_store`, `auth`,
`catalog_repository`) join the core only under
`BUILD_NOVAWORLD_HTTP`, which also defines `OPENNOVA_HTTP_ENABLED` PUBLIC so
`main.cpp` starts the listener; the header-only `loggable.h` serves every listener
either way. The route harness, ctest
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
the wire. The listing's owner: `host_key`
([`tests/novaworld/host_key_test.cpp`](../../tests/novaworld/host_key_test.cpp), every
build) drives the key store and the lobby session's links and strict refusal on a
migrated in-memory database; the route harness pins `/NWHost.dll`'s recording (and its
unchanged bytes), `GET /api/me/hosts` and `GET /api/admin/hosts`; and
`nw_host_ownership_e2e`
([`tests/novaworld/nw_host_ownership_e2e_test.cpp`](../../tests/novaworld/nw_host_ownership_e2e_test.cpp),
`BUILD_NOVAWORLD_HTTP` only) runs two in-process services, one strict, against
opennova-nw-lister's `Lister` with and without an account and hand-driven host
sessions (the NWU reconnect, a second connection presenting a listed key). Both e2e
tests build their service from
[`tests/novaworld/nw_service_harness.h`](../../tests/novaworld/nw_service_harness.h).
The owner's pushes: the route harness's `owner_host_push` case (its NW UDP listener
writes the pool, so a host's key links its row) drives the 401 / 403 / 404 / 400 /
429 answers, the same 404 for another account's RID and a free one, the push-time
re-check, the drain's re-check after a ban (the drain held with
`NwUdpListener::pause_push_drain`), the 409, "café" arriving as cp1252, the roster and
the stop against hand-driven hosts (the admin case adds the encoding refusals, an escaped
surrogate pair named, "€ ™" arriving as 0x80 and 0x99, and the byte caps at and past them);
`host_text_utf8`, a host-reported cp1252 name coming back as UTF-8 in `/api/lobbies` and
`/api/hosts` and as the host's byte in the GSB; and `nw_owner_admin_e2e`
([`tests/novaworld/nw_owner_admin_e2e_test.cpp`](../../tests/novaworld/nw_owner_admin_e2e_test.cpp),
`BUILD_NOVAWORLD_HTTP` only) lists an in-process `opennova-serve` with an account and
administers it from the website's routes: its match takes the owner's
`SetServerName` (and saves `game.cfg`) and runs its `TextChatServer`, another
account's push is a 404, and the owner's stop ends the listing.
