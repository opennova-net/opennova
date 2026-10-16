# NovaWorld server

The standalone NovaWorld server: one process, three listeners, one PostgreSQL
database ("Database" below). Linux only: it ships as its container image. Implements the matchmaking/backend half of NovaWorld (the gate, the
session handshake, the browser/host/play container services, and the legacy
`NW*.dll` HTTP flow). Protocol record: [`docs/net/novaworld-net-re.md`](../../docs/net/novaworld-net-re.md).

## Listeners

| Listener | Port | Protocol |
| --- | --- | --- |
| `GateListener` | UDP 7597 | `novaworld_gate` probe → gate response (NWU `GATEAPI`) from the tag's lobby ("Lobbies: one per title" below); the same port is the advertised `POSTIPPORT`, the sink for a retail host's plaintext status heartbeat (`Lobby_UpdateServerInfo`), folded into `active_hosts` by `HostKey` |
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
- **Every thread has its own database connection.** A `db::Database` is used by
  one thread at a time: its row counts, its error text and any open transaction
  belong to the connection. `main()` owns one `db::ConnectionPool` on
  `DATABASE_URL` (at most `DATABASE_POOL_MAX` connections). A thread holds one
  lease at a time, and code holding one hands its `db::Database&` to what it calls
  (a second lease on one thread can wait on itself once the pool is out; a debug
  build asserts it). The gate and NW UDP receive threads each lease a connection
  for their lifetime (each thread leases its own, and `start()` waits for it, so a
  database that cannot be reached stops the boot); the main thread holds one for
  the boot's work and one for its tick loop, and none while it starts or stops the
  listeners and workers; each HTTP request leases one for the handler; and
  `erase_lobby_state` (the `on_lost` sink, run on the NW UDP thread and on the main
  thread) writes on the calling thread's own. A `db::Error` carries the server's SQLSTATE
  (`unique_violation()`, `lock_timeout()`, `connection_lost()`) and its text
  names the failing statement, never a bound value, so it goes to the server
  log alone: a reply to a database failure carries only its error code
  (`db_error`). A write that spans statements (`clear_all`, `replace_roster`,
  `apply_status_blob`, the lobby's host row with its roster, a registration's
  checks and insert) is one `db::Transaction`, which holds the writer lock
  ("Database" below), and a read that spans them (`/api/hosts` and the `.gsb`
  feeds: the rows, then each roster) is one `db::ReadSnapshot`, so a host never
  goes out beside another write's roster.
- **The hosted servers' supervisor runs on a thread of its own** (with
  `ONNET_HOSTED_PROVIDER` set; "Hosted servers" below), started once the gate has its
  port and stopped before the listeners, so the local provider's servers deregister
  while the service still serves. Each pass leases a pooled connection. Each running
  hosted server's admin link is a thread of its own too (at most `ONNET_HOSTED_CAP`),
  ended after the supervisor and before the provider, so its session ends with `QUIT`
  while its server still runs; a link leases a pooled connection to write its console.
- **The `UnknownTracker` records from listener threads** into an in-memory,
  mutex-guarded accumulator and is flushed to the DB only on the main tick — never
  per packet. `GET /api/unknowns` (admin) serves this run's: each signature's first
  sample as hex and its sender's address.
- **Each listener's `start()` holds its port before the boot goes on.** The gate and
  NW UDP listeners bind their socket in `start()` and move it into the receive
  thread (which leases its connection), so the port they report is the one they serve,
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
  within one tick and stops in order: the main loop's lease back to the pool, the
  HTTP listener (Crow's threads joined, a held-open request included), then the
  gate and NW UDP receive threads (each returns its lease), then the connections
  (`Shutdown`). As `main()` returns, its locals go in reverse declaration order, so
  the pool (declared first) closes last. The HTTP
  listener clears Crow's own signal set (`signal_clear()`), which would otherwise
  take both signals and stop only Crow.

## Health: the watchdog, `/healthz` and the heartbeat

Service policy (`health.*`), what the container runtime and the uptime checks read:

- **The stall watchdog.** The gate's receive loop and the NW UDP listener's (each wakes
  at least every 250 ms with nothing to read) and the hosted servers' supervisor (a pass
  every second, a turn per server it steps) count their turns, and the main thread's
  tick samples the counters. A loop whose count has not moved for 30 s is stalled: the
  server logs `[watchdog] FATAL the <loop> made no progress for 30 s; exiting 70` and
  exits 70 at once (nothing in the process can unstick a thread; an orderly shutdown
  could hang on it), and the runtime starts it again (`restart: unless-stopped`).
- **`GET /healthz`** answers `ok`, or `503 stalled` while a watched loop has stood still
  for 30 s or the main thread's last sample is that old, with no other detail
  (`Cache-Control: no-store`). It sits outside `/api`, so `ONNET_API_PROXY_ONLY` never
  refuses it, and the image's `HEALTHCHECK` and the uptime checks use it.
  `GET /api/health` stays, a static `{"status":"ok"}` behind the proxy rule.
- **The heartbeat**, one line on stdout every 60 s, for the log pipeline's metric
  filters:
  `{"heartbeat":{"nw_sessions":N,"listed_hosts":N,"hosted_live":N,"hosted_failed":N,"machines_lost":N,"launch_failures":N,"billing_sync_age_s":N,"webhook_rejects":N,"limits_held":N}}`.
  `nw_sessions` counts the NovaWorld UDP sessions, `listed_hosts` the listed servers,
  `hosted_live` the live hosted servers; `hosted_failed` and `launch_failures` count the
  hosted servers that ended failed and the launches that failed (refused, or a machine
  that never booted) since the last line;
  `machines_lost` counts the fleet's machines lost now (their agents silent); `billing_sync_age_s` is the age
  in seconds of the oldest webhook event the billing worker has not synced (0 with none,
  -1 with billing off), `webhook_rejects` the webhook deliveries refused since the
  last line, and `limits_held` is 1 while the hosted servers' limits hold is on (0
  otherwise).
- **The image runs as UID 10002** (`novaworld`, no home, no shell): the server keeps no
  state in its container and writes only temporary files under `/tmp`.

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
tables). Each server runs one data set its owner picks. Everything here is service policy.

- **The hosting entitlement** (`billing/entitlement.*`): how many hosted servers an account
  may have live, the larger of its support tier's servers (a monthly Stripe subscription:
  tier 1, 2 or 3 grants 1, 2 or 3 servers; `billing/tiers.h`) and an admin's comp grant
  (`players.comp_servers`, 0..16, `PUT /api/admin/users/<id>/comp-servers`). Only an active
  player's account has one. It is read from the billing tables (`backend/migrations/
  0002_billing.sql`, the cache of Stripe the billing worker keeps), whether or not billing
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

- **Data sets** (`hosted/data_sets.*`). A data set is a game folder a machine mounts, the
  title `opennova-serve` speaks over it (`/game <code>`, a `gameprofile` row) and the
  lobby it lists in (the `games` row whose slug names it). The service knows three, in
  the order the site offers them: `jop_2_consumer` (Joint Operations, `/game jo`, base
  game or Escalation, `jox01`), `dfx2_consumer` (Delta Force Xtreme 2, `/game dfx2`, base
  game only) and `opennova_0_consumer` (OpenNova's own data, `/game jo`, base game only).
  Each has its own host-screen limits (Joint Operations': 2..64 players, a time limit of
  1..300, kill and score limits of 1..500, respawn and delay of 0..300; Delta Force Xtreme
  2's the same but 2..100 players, its host screen's cap; OpenNova's Joint Operations').
  A deployment offers the ones its machine driver has the data of
  (`ONNET_HOSTED_DATA_SETS`, below: the local driver's with a `resource_dir`, the ec2
  driver's every one it names, since each cloud machine mounts them all); with none,
  hosting is off. A start names one (`hosted_servers.game`, the games slug), and a server
  whose data set is no longer offered fails at its next placement (no restart mends that).

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
  keeping where it last ran.
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
  its state, its data set (`game`), the owner's settings (range-checked columns, never a
  file), its place in the fleet (above), its container's name and the machine's private
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
  (`hosted::request_server`), which holds the writer lock, so no two starts both pass a
  count. When a server is about to be placed (`provisioning`, before its credentials are
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
  `limits_hold_owners`, `limits_hold_servers`), billing on or off.
- **The supervisor** (`hosted/supervisor.*`) is a thread of its own, started once the gate
  has its port and the internal listener serves; each pass (every second) leases a
  pooled connection, steps every live machine (below), then walks every live server:
  `requested` → `provisioning` (placed; on its slot the service account gets a new
  password, a new admin password is minted, and the files are rendered for the slot's
  ports) → `booting` (its container wanted on its slot until its machine's agent reports
  it running) → `listing` (until a listed RID holds one of the service account's
  HostKeys) → `running` → `stopping` → `stopped`, or `failed`. Every move past a request
  or a stop is the supervisor's, a compare-and-set on the row's state, so a stop that
  lands while a pass places a server wins (it lets the slot go).
- **A stop** is the supervisor's own act: the row goes to `stopping` (asked by the owner
  or an admin, or by the supervisor when the owner's account is deleted or no longer
  active, read every pass and again right before a start mints credentials, or for the
  owners' limits above), the supervisor pushes `ServerStopHosting` to the listing (the
  sysop punt's MsgCode 7, scoped to the service account), and stops wanting the
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
- **The machines' lifecycle.** A booting machine whose agent never reports within the
  driver's boot timeout (15 min on EC2, 1 min locally) is terminated and failed. A ready
  (or draining) machine whose agent has been silent 60 s is `lost`: the driver is asked;
  gone, it ends now; still running, it is terminated 3 min later, and an agent that
  reports again first makes it ready (or draining) again. While its machine is lost a
  running server's health checks wait. A shared machine with no server on it ends after
  `ONNET_FLEET_IDLE_MINUTES` (30); an own machine ends once its server has ended. A
  draining machine moves its servers off one at a time, the next once the last moved one
  runs again elsewhere (or ended), and ends once it holds none; nothing new lands on it,
  and it counts toward neither maximum, so a roll launches the machines it moves servers
  to. A machine row moved to `ending` (an admin's end) is terminated at the next pass. A
  launch the cloud refuses, or a machine that never booted, fails the row, counts toward
  the heartbeat's `launch_failures` and holds every launch of that kind for 60 s.
- **The desired state** (`hosted/fleet_desired_state.h`, the supervisor's answer to each
  agent's sync): the containers of the live servers placed on the machine that are
  booting, listing, running or stopping (a stopping one only once its agent has reported
  it, so a server stopped before its start never starts), each with its slot, its user,
  its pinned image, its data set, `opennova-serve`'s arguments, the slots' limits
  (`ONNET_FLEET_SLOT_CPUS`, `ONNET_FLEET_SLOT_MEMORY_MB`, 128 processes) and its working
  directory's files until the agent has reported it once. The files are kept in the
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
  secret (stored hashed), the serve image, its slots and the data sets' mount paths; then
  every 5 s (1 s
  with work) it posts `POST /hosted/machines/<id>/sync` with every container's state, CPU,
  memory and its slot's egress, and runs what the reply wants. Both routes are on the
  **internal listener**, a second HTTP server bound to `ONNET_INTERNAL_HTTP_BIND` (the
  service's private address, `10.44.0.10:8070`, which the machines' security group alone
  reaches; `127.0.0.1:8070` by default), never on the public port: they answer only a TCP
  peer inside `ONNET_HOSTED_BOOTSTRAP_CIDRS` (and loopback on a loopback bind), and only
  from the machine's own recorded private address. Every refusal is one bare 404; a
  refusal inside the networks is logged without the token or the secret at most once per
  peer per minute, and Crow's per-request lines for these paths are left out of the log.
  The service starts it before the supervisor and stops it after the supervisor's thread.
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
  Samples are kept 48 hours, hours 90 days; the routes read the samples for 1h and 24h
  and the hours for 7d and 30d.
- **The admin link** (`hosted/admin_link.*`; ADR 0053 d5). A running server opens
  retail's remote admin on its `game.cfg`'s `remote_admin_port`, its slot's admin port,
  its `admin.cfg` granting the user `svc` (a 31-character random password, new at every
  start, rights `7F`) to the service's address alone (`ip_restrict`; on a cloud machine
  its security group too). The service is that console's only client: owners never get
  the port or the login, and reach the console only through the site's routes. The
  supervisor keeps one `AdminLink` per server in `running`, at its machine's private
  address (every pass syncs them: a link comes up once the server runs, is replaced when
  a restart rendered a new login, and goes down as the server leaves `running`, at
  `stopping` or a crash). Each link is a thread of its own (at most `ONNET_HOSTED_CAP` of
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
  admin link, then restarts the server.
- **The console** is the table `hosted_server_console`,
  a capped ring of each server's newest 500 lines: the commands the routes ask a link to
  record (with their sender, `owner`, `admin` or `service`), each reply packet, and the
  link's own status lines (the session up, lost or refused, closed, and the server's scene
  when it changes), in UTF-8 with UTC times. The reads the site polls (the player list,
  the rotation, the chat window) are not recorded.
- **Health.** While `running`, every pass reads its agent's report and every 20 s checks
  the listing and the admin link (its session up: its last status poll or command
  answered); three failed checks in a row fail the server. Always on: nothing reaps an
  idle server or ends one for its age.
- **Reconciliation** at the supervisor's start: a machine row still `launching` (the
  service went mid-launch) fails; every cloud machine the driver lists (`list_tagged`)
  that no live row names is terminated; a live row whose cloud machine is gone ends, its
  servers placed again with no restart counted (a `stopping` one is stopped); a server
  whose files went with the service (`booting`, its container never reported) is placed
  again; the rest resume where they were (a running one gets the listing timeout for its
  lister to re-host before the health checks count, since the boot cleared the previous
  run's listings), their containers answered for without files. A driver that cannot
  list ends nothing. Every 5 min the sweep half runs again.
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
  random password, rights `7F`, `ip_restrict` the service's address as the machine sees
  it) and the `--credentials` file (`NOVAWORLD_USER` / `NOVAWORLD_PASS`). All but
  `game.cfg` are secret (the host file carries the game password): sent to the agent
  only until it has reported the container, written owner-only and never logged. The
  admin login lives in the row's own columns, read by `hosted::admin_login_of` alone:
  `HostedServer` has no field for it, so no reply can carry it. `opennova-serve` runs with
  `/HOST server.host --credentials credentials.txt --master-host <host>
  --master-gate-port <port> /game <title>` (the data set's) and `/exp jox01` for
  Escalation. The settings' rules: a game the deployment offers, a name of 1..31 cp1252
  bytes with no control character or double quote, a password of up to 16 printable
  ASCII characters with no space or quote, the whole numbers within the data set's
  limits, an expansion the data set allows, and 1..32 `.bms` names of letters, digits,
  `_` and `-`.
- **Machine drivers** (`hosted/machine_driver.h`): the supervisor's one seam to the
  cloud. A driver launches the machine a row stands for (the row's id and its one-time
  token, nothing else), describes it, terminates it, lists the machines it runs for this
  deployment and plans a slot's ports. `ONNET_HOSTED_PROVIDER=local`
  (`hosted/local_machine_driver.*`, for development and the tests) runs each machine as
  a stand-in on this one: a thread that plays `opennova-hostd` over the real protocol
  against the internal listener on loopback and runs each container as an
  `opennova-serve` child (`ONNET_HOSTED_SERVE_BIN`, `--resource-dir` the data set's
  `resource_dir`, `--loose-root` with its `loose_root`) in
  `ONNET_HOSTED_WORK_DIR/machine-<id>/slot-<k>` (written from scratch with the reply's
  files, each owner-only), its output in `machine-<id>/<container>.log`, its ports free
  ones probed on loopback, its environment an allowlist (`PATH`, `HOME`, `LANG`,
  `LC_*`, `TZ`, `TMPDIR`; on Windows `PATH`, `SystemRoot`, `SystemDrive`, `windir`,
  `TEMP`, `TMP`), so none of the service's own variables reaches it. Its machines end
  with the service (they keep no state; the next boot's reconciliation places their
  servers again). `ONNET_HOSTED_PROVIDER=ec2` (`hosted/ec2_machine_driver.*`) launches EC2
  machines from the launch template of their kind (`ONNET_HOSTED_EC2_LAUNCH_TEMPLATES`,
  `{"shared": "lt-...", "own": "lt-..."}`; `infra/aws`: a stock Ubuntu image, the
  kind's type, every data set's private encrypted snapshot as a volume of its own, the
  machines' security group, their instance profile, IMDSv2 required), tagged
  `opennova:hosted`, `opennova:deployment`, `opennova:machine-kind`,
  `opennova:machine-id` and `Name`, with the machine script
  (`hosted/machine/machine_user_data.sh`, shipped in the image) as user-data: it mounts
  every data set read-only at `/opt/game/<slug>` by its volume's ext4 label (`od-jop2`,
  `od-dfx2`, `od-opennova0`), reserves the slots' ports out of the ephemeral range,
  fences the slots' users off the instance metadata and the service's internal port,
  and installs `opennova-hostd`, copied out of the serve image. The driver runs the AWS
  CLI v2 (installed in the image, pinned and checksummed) through a `CommandRunner`
  (`apps/common/command_runner.*`: an argv array, no shell, a 20 s timeout (under the stall watchdog), an
  environment of the system's and the `AWS_*` variables alone), its credentials the
  service instance's role; it describes from one cached `describe-instances` every 10 s
  (a failure reads unknown: nothing is decided on it), and a machine is terminated at
  its end, never stopped. The servers list through `ONNET_PUBLIC_HOST`
  (`--master-host`), so the listing records each machine's own public address.

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_HOSTED_PROVIDER` | `none` | The machine driver: `local` (stand-in machines on this one, above), `ec2` (EC2 machines); `none` runs no hosted server. Any other value, fleet settings or data sets that do not read, none the driver has the data of, `local` without `ONNET_HOSTED_SERVE_BIN`, or `ec2` missing or mistyping one of its settings (or with a loopback `ONNET_PUBLIC_HOST`) is `none` with a boot warning naming why. |
| `ONNET_HOSTED_CAP` | 24 | The most hosted servers live at once, across every account: a backstop beside the fleet's own room. |
| `ONNET_HOSTED_SERVE_BIN` | none | The local driver's `opennova-serve` binary. |
| `ONNET_HOSTED_WORK_DIR` | `backend/data/hosted` | The root of the local driver's machines' slot directories. |
| `ONNET_HOSTED_DATA_SETS` | none | A JSON object keyed by data set (`jop_2_consumer`, `dfx2_consumer`, `opennova_0_consumer`), each member of an entry optional: `missions`, a list of `.bms` names, the rotation a start on it takes when its request names none (the first the starting map) and the missions the site's rotation picker offers (with none, a start must name its missions); for `local`, `resource_dir`, the game folder its servers serve (their `--resource-dir`; never licensed data in a repository: an install, or a synthetic loose directory), which is what makes `local` offer the data set, and `loose_root`, `true` to add `--loose-root` (a folder with no game archives, mounted as loose files). The `ec2` driver offers every data set named (each cloud machine mounts them all). For example `{"jop_2_consumer":{"resource_dir":"C:/Games/Joint Operations Combined Arms","missions":["ASH_G11A.bms"]}}`. Any other key or member is a boot warning, and hosting is off. |
| `ONNET_INTERNAL_HTTP_BIND` | `127.0.0.1:8070` | The internal listener, `<IPv4>:<port>`: the service's private address in a deployment (`10.44.0.10:8070`); a private or loopback address, never a public or wildcard one. The ec2 machines' agents call it, and its address is what their `admin.cfg` admits. |
| `ONNET_HOSTED_BOOTSTRAP_CIDRS` | none | Comma-separated private IPv4 CIDRs, the networks the agent routes answer (the machines' subnet); each inside 10.0.0.0/8, 172.16.0.0/12 or 192.168.0.0/16. On a loopback bind, 127.0.0.0/8 is admitted besides. |
| `ONNET_FLEET_SHARED_SLOTS` | 12 | A new shared machine's slots (1..32). |
| `ONNET_FLEET_MAX_SHARED` | 2 | The most shared machines taking servers at once. |
| `ONNET_FLEET_MAX_OWN` | 2 | The most own machines taking servers at once. |
| `ONNET_FLEET_IDLE_MINUTES` | 30 | How long a shared machine with no server lives on before it ends. |
| `ONNET_FLEET_SLOT_MEMORY_MB` | 512 | Each container's memory (no swap). |
| `ONNET_FLEET_SLOT_CPUS` | 1 | Each container's CPUs (a decimal). |
| `ONNET_HOSTED_SERVE_IMAGE` | none | The serve image every start pins and the machines pull (`ghcr.io/<owner>/opennova-serve@sha256:...`, pinned at deploy); required for `ec2` (the local driver only names it). |
| `ONNET_HOSTED_HOSTD_IMAGE` | the serve image | `ec2`: the image `opennova-hostd` is copied out of (pinned at deploy). |
| `ONNET_HOSTED_EC2_LAUNCH_TEMPLATES` | none | `ec2`: the launch templates by machine kind, `{"shared": "lt-...", "own": "lt-..."}` (each `lt-<id>` at its default version, or `lt-<id>:<version>`). |
| `ONNET_HOSTED_EC2_REGION` | none | `ec2`: the region every AWS CLI call names. |
| `ONNET_HOSTED_EC2_SUBNET` | none | `ec2`: the subnet (`subnet-...`) a machine launches into; it must give its machines a public IPv4. |
| `ONNET_HOSTED_DEPLOYMENT` | none | `ec2`: the `opennova:deployment` tag every machine carries and every describe, list and terminate is scoped to, so two stacks in one account never touch each other's machines (letters, digits, `.`, `_`, `-`). |
| `ONNET_HOSTED_AWS_CLI` | `/usr/local/bin/aws` | `ec2`: the AWS CLI v2, an absolute path (run as given, no `PATH` search). |
| `ONNET_HOSTED_MACHINE_SCRIPT` | `/app/hosted/machine_user_data.sh` (where the image ships it) | `ec2`: the machine script, the user-data template (`hosted/machine/machine_user_data.sh`); a run outside the image names it. |

The deploy toolbox sets every `ec2` setting from the Terraform outputs (DEPLOY.md,
"Hosted game servers"); nobody copies an id by hand.

The routes (`hosted/hosted_routes.*`, the admin's fleet routes `hosted/fleet_routes.*`, and the
comp grant with the admin user routes), behind
`ONNET_API_PROXY_ONLY` like every `/api` route. Every refusal is an error code with the
service's own message, never database text.

| Route | Access | Replies |
| --- | --- | --- |
| `GET /api/me/servers` | a website session | `{"enabled", "full", "entitlement", "data_sets", "servers"}`: `enabled` is a machine driver with at least one data set; `full` that a start now would be `at_capacity` (`ONNET_HOSTED_CAP` servers live, or the fleet with no place for a shared server and none for one on an own machine the account still has free); `entitlement` the account's hosting entitlement (`max_servers`, `used`: its live servers, a stopping one included, `own_machines`, `own_used`: those of them on own machines, `can_start`, `source`: `none`, `tier` or `comp`); `data_sets` the ones it offers, in order (empty when hosting is off), each `game`, `display_name`, `expansions` (`code` and `name`, the base game's `""` first), `default_missions` (its `missions`: the rotation a start on it takes when it names none and the missions the site's rotation picker offers) and `limits` (`max_players`, `time_limit`, `kill_limit`, `max_score`, `respawn`, `start_delay`, each `min` and `max`); `servers` its live servers by id, oldest first, then its newest 10 ended ones, newest first, each `id`, `state`, `live`, `own_machine`, `settings` (`game`, `game_name`, `max_players`, `game_password`, `time_limit`, `kill_limit`, `max_score`, `respawn`, `start_delay`, `team_ff`, `expansion`, `missions`), `game_display_name` and `expansion_display_name` (the catalog's names, so an ended server's read the same once its data set is gone), `restarts`, `failure_reason`, `created_at`, `updated_at`, `running_at`, `ended_at` (UTC, null until set) and `listing` (`rid`, `server_name`, `mission_name`, `players`, `max_players`, `host_ip`, `host_port` while listed, else null); `Cache-Control: no-store` |
| `GET /api/me/servers/<id>` | a website session | `{"server"}`, one of the account's servers in any state, as the list's plus `events` (the newest 50 transitions, oldest first: `from`, `to`, `detail`, `at`); 404 `not_found` for an id that is not the account's (another account's server is a 404, never a 403); `Cache-Control: no-store` |
| `POST /api/me/servers` | a website session, the CSRF header | the settings as JSON (`game_name` required; `game` defaults to the first offered data set and `missions` to that data set's; every other field to its default) and `own_machine` (`true` to run it on an own machine; default `false`, a shared one). 503 `hosting_disabled` with no machine driver; 429 past the account's bucket (6, then one every 20 s, shared with the stop); 400 `invalid_json`, `invalid_field`, or `invalid_settings` with the `field` and why; 403 `not_entitled` with no hosting entitlement (or an account not active), 403 `payment_past_due` while a granting subscription's payment is past due; 409 `server_limit` with `max_servers` and `used` beside the code when every server the account may run is live, 409 `no_own_machine` when each own machine it pays for already runs one, 409 `at_capacity` (`ONNET_HOSTED_CAP` servers live, or the fleet's planner finds it no place: every slot taken, the servers already waiting counted, and the fleet at its maximum machines); 202 `{"server"}`, `requested` |
| `POST /api/me/servers/<id>/stop` | a website session, the CSRF header | 429 past the bucket; 404 `not_found` for an id that is not the account's; 409 `not_live` once it has ended; 202 `{"server"}`, `stopping` (also when it already was), or `stopped` when the supervisor finished it in between |
| `GET /api/me/servers/<id>/stats` | a website session | one of the account's servers' stats, in any state: `?range=1h` (a point a minute), `24h` (the default; five minutes), `7d` (an hour) or `30d` (four hours), another 400 `invalid_field`; 404 `not_found` for an id that is not the account's; 429 past the caller's read bucket (60, then two a second). `{"server_id", "range", "live", "summary", "series"}`: `live` the server now, null once it has ended (`state`, `uptime_seconds` while running, else null, `players`, `max_players`, `mission`, `game_type` and `time_left` in seconds from its listing, `scene` (`game`, `menus`, `unknown`) and `link_state` from its admin link, `cpu_percent` and `memory_bytes` from its newest sample; each null where nothing is known); `summary` over the range (`live_seconds`, `running_seconds`, `availability` (running over live, 0..1, null with none), `restarts` (crash restarts), `peak_players`, `avg_players` (players at a time while it ran, null with none), `player_hours`, `unique_players`, `missions`, `egress_bytes`, `egress_bytes_month` (this calendar month, UTC, whatever the range) and `approximate`, the names of the fields counted approximately); `series` (`step_seconds`, and `points`, one per step oldest first: `t` (the step's start, ISO 8601 UTC), `players`, `max_players`, `running`, `cpu_percent`, `egress_bytes`, null where no sample fell); `Cache-Control: no-store` |
| `GET /api/me/servers/stats` | a website session | the account's servers together, as above with no `server_id` and `live` null: the summary's times added up, its peak the most players at once across them, each point's players and bytes added up and `running` true while any ran |
| `PUT /api/admin/users/<id>/comp-servers` | admin | `{"comp_servers": 0..16}`, the admin's grant; 200 `{"user"}` (which carries `comp_servers` and `max_servers`, the entitlement); 400 `invalid_json`, `invalid_field` (no whole number in 0..16) or `missing_field`; 404 for an unknown id or a service account. An entitlement down to no server stops the account's hosted server, as a ban does |
| `GET /api/admin/hosted` | admin | the fleet: `{"enabled", "provider", "cap", "live", "machines", "servers"}`. `machines`: the live machines by id, then the newest 10 ended or failed, each `id`, `provider`, `kind` (`shared` or `own`), `state` (`launching`, `booting`, `ready`, `draining`, `ending`, `ended`, `failed`, `lost`), `cloud_id`, `instance_type`, `slots`, `slots_used` (the live servers on it), `private_address`, `public_address`, `bootstrapped`, `agent_version`, `template_version`, `serve_image`, `reserved_for` (an own machine's server, else null), `last_report_at`, `launched_at`, `ready_at`, `ended_at`, `end_reason` and `egress_bytes_month` (its slots' egress this calendar month, UTC, as the stats count it). `servers`: the newest 100 the query takes, live first (`?owner=<username>`: that account's alone, none for an unknown name; `?state=live`, `ended` or `all`, the default; another state 400 `invalid_field`), each as `GET /api/me/servers`' plus `owner` (`{id, username, tier}`, `tier` its granting support tier or null; null once the account is gone), `service_account` (`{id, username}` or null), `provider`, `container` (its container's name), `machine_address`, `admin_port`, `machine_ref` and `slot` (null with none; an ended server's where it last ran), `game_port` (null before a placement), `generation`, `serve_image` and `players` (its listing's, null when not listed). Never the admin password or an agent's credential; `Cache-Control: no-store` |
| `GET /api/admin/hosted/<id>` | admin | `{"server", "machine"}`: one server in any state as the fleet view's plus `events` (the newest 50 transitions, oldest first), and the machine it is on or last ran on (as above, or null); 404 `not_found`; `Cache-Control: no-store` |
| `POST /api/admin/fleet/machines/<id>/drain` | admin | the machine takes no new server and the supervisor moves its servers off it one at a time (no restart counted), then ends it; 202 `{"machine"}` (one draining already included); 404 `not_found`; 409 `not_live` once it is ending or ended |
| `POST /api/admin/fleet/machines/<id>/end` | admin | the machine to `ending`: the next pass terminates it and places its servers again (no restart counted); its agent is refused from then on; 202 `{"machine"}` (one ending already included); 404 `not_found`; 409 `not_live` once it has ended |
| `POST /api/admin/fleet/roll` | admin | a fleet roll: every machine that takes servers (launching, booting, ready) drains, its servers moving to machines launched for them, so a new serve image or launch template reaches the fleet; 202 `{"draining": <how many>, "machines": [<their ids>]}` |
| `GET /api/admin/hosted/<id>/stats` | admin | any server's stats, as the owner's; 404 `not_found` |
| `GET /api/admin/hosted/stats` | admin | the fleet's: every server together, as the account's |
| `POST /api/admin/hosted/<id>/stop` | admin | 404 `not_found`, 409 `not_live`, else 202 `{"server"}` |
| `POST /api/admin/hosted/stop-all` | admin | the kill switch: every live server to `stopping`; 200 `{"stopping": <how many>}` |

**The console routes** (`hosted/console_routes.*`) run the curated commands on a hosted
server's admin link. The owner's act on one of the caller's own servers by its id
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
  request in flight: a second answers 429 `console_busy` at once, with no wait and no
  token drawn;
- at most handler threads - 2 console requests are admitted service-wide (503
  `console_full` past that), so two threads always stay for everything else;
- a rotation edit (an add, a remove, a next) also holds its server's one edit, so its read
  of the rotation and its edit run as a pair: a second edit meanwhile answers 409
  `rotation_busy`, and two removes can never both pass the last-map check.

`GET .../console` reads the database alone and is not admitted. The site serializes its
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
| `POST /api/me/servers/<id>/rotation` | `MISSION LIST` (and `GET GAMESTATE` when it lists none), then `MISSION ADD <file> <0\|1>` | body `{"mission": "<file>.bms", "swap_sides": false}`, appended; `{"result": "added"}`; 400 `invalid_mission`, `missing_field`, `invalid_field`; 404 `mission_not_found`; 409 `rotation_full` (32 maps, the start settings' cap: policy, not a server limit), `not_in_game` (retail answers an add OK between matches and appends nothing) |
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
the server compares `(u8)atol` and 256 would be its own slot 0; an index 0..31, a rotation being at most 32 maps (policy); a mission
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
  redelivery is a no-op) with the customer it names, the worker is woken, and the reply
  is 200 `{"received": true}`. The payload is never trusted beyond its customer's id.
- **The sync worker** (`billing/billing_sync.*`, a thread of its own): each customer with
  an unrecorded event is read back from Stripe with all its subscriptions, then written in
  one transaction (no Stripe call ever runs inside one), the longest-waiting first; a
  customer Stripe could not be asked about waits 60 s, doubling to 15 min. A deleted
  customer's subscriptions are canceled here and its row goes. A full reconcile runs at
  the first pass, every `ONNET_BILLING_RECONCILE_MINUTES` and when an admin asks: every
  subscription, a page of 100 at a time with its customer, written when it is this
  instance's (a missing customer row made again from its metadata), listed as an orphan
  when it grants and is another instance's or names no account here, and a granting row
  Stripe no longer lists read alone and canceled when Stripe no longer knows it; accounts
  with two granting tiers are listed as duplicates. The configured prices are checked
  then (and at the first pass): each a monthly recurring price in the key's mode, the one
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
| `POST /api/billing/checkout` | a website session, the CSRF header | `{"item": "tier1" \| "tier2" \| "tier3" \| "own_machine"}`; 429 past the account's bucket (6, then one every 20 s, shared with the portal); 400 `invalid_json` or `invalid_item`; 503 `billing_misconfigured`; 409 `checkout_in_progress` while another checkout of the account is running (one at a time); 403 `not_eligible` (an account not active, or the add-on with no tier granting); 409 `already_subscribed` while a tier grants (the portal changes it); 409 `own_machine_limit` for the add-on once the account has an own machine for every server it may run (each add-on checkout sells one; several are fine below that); 502 `stripe_unavailable`; 200 `{"url"}`. The account's previous session is settled first: expired while open; once complete (paid, the webhook perhaps still on its way), the customer is synced from Stripe and the checkout judged again, so nothing is sold twice (502 when Stripe cannot say). Its customer is made at its first checkout (idempotency key `opennova-<instance>-customer-<id>`), and made again in place of a row of the other Stripe mode; the session asks for the terms of service, lives an hour from its creation (Stripe refuses under 30 min) and stamps the instance and the account on the subscription it makes |
| `POST /api/billing/portal` | a website session, the CSRF header | 429 (the checkout bucket); 404 `no_billing_account`; 502 `stripe_unavailable`; 200 `{"url"}` |
| `POST /api/me/billing/sync` | a website session, the CSRF header | the account's customer read back from Stripe now (the success page's, before the webhook comes); 429 past its bucket (3, then one every 10 s); 404 `no_billing_account`; 502 `stripe_unavailable`; else as `GET /api/me/billing` |
| `POST /api/billing/webhook` | Stripe's signature | above |
| `GET /api/admin/billing` | admin | `{"enabled", "mode" ("test" \| "live"), "prices_ok", "prices_error", "last_reconcile_at" (ISO or null), "last_reconcile_error", "pending_events", "subscriptions" (each row with an account: `user_id`, `username`, `stripe_customer_id`, `stripe_subscription_id`, `item`, `tier`, `quantity`, `status`, `entitled`, `current_period_end`, `cancel_at_period_end`), "duplicates" (account ids), "unknown_prices" (the prices of our subscriptions that would grant but that no `STRIPE_PRICE_*` list names, as the last reconcile found them), "orphans" (`stripe_customer_id`, `stripe_subscription_id`, `username_hint`, `status`), "recent_events" (the newest 50: `event_id`, `type`, `received_at`, `outcome`), "limits_hold", "limits_hold_reason", "limits_hold_owners" and "limits_hold_servers" (the hosted servers' limits hold and the stops it keeps back, "Hosted servers" above)}; with billing off `{"enabled": false}` and the limits hold alone; `Cache-Control: no-store` |
| `POST /api/admin/billing/reconcile` | admin | no body or `{}`: a full reconcile on the worker's next pass, at once: 202 `{"reconcile": "requested", "force_limits": false}`; `{"force_limits": true}` also has the supervisor's next pass apply the stops the limits hold keeps back (`"force_limits": true`); 400 `invalid_json`, `invalid_field`; with billing off 503, but an override still applies (202 `{"reconcile": "off", "force_limits": true}`) |
| `POST /api/admin/billing/link` | admin | `{"user_id", "stripe_customer_id"}`: the customer (an orphan's) made the account's, its metadata stamped with this instance and the account, then synced; 400 `invalid_json`, `missing_field`, `invalid_field`; 404 `not_found` (no such player, or a customer Stripe does not know); 502; 200 `{"user_id", "stripe_customer_id", "outcome"}` |

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

All via environment (`server_config.cpp`): `ONNET_PUBLIC_HOST`,
`ONNET_{GATE_UDP,NW_UDP,HTTP}_PORT`, `DATABASE_URL`, `DATABASE_PASSWORD`,
`DATABASE_POOL_MAX` ("Database" below), `MIGRATIONS_DIR`, `SEED_DIR`,
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
Billing takes the `STRIPE_*` set and `ONNET_SITE_URL`, `ONNET_STRIPE_CURL`,
`ONNET_BILLING_AUTOMATIC_TAX` and `ONNET_BILLING_RECONCILE_MINUTES` ("Billing (monthly
support tiers)" above).
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

## Database

PostgreSQL 17, reached through libpq
([`engine/net/novaworld/db/database.h`](../../engine/net/novaworld/db/database.h), the
`opennova_pq` target the service alone links). In prod it is a managed instance (RDS);
locally, the dev compose stack's `postgres` service or `scripts/dev_postgres.sh up`.

| Setting | Default | Meaning |
| --- | --- | --- |
| `DATABASE_URL` | `postgresql://novaworld@127.0.0.1:5432/novaworld` | A libpq connection URI: host, port, role, database and options. Against RDS it carries `sslmode=verify-full&sslrootcert=/app/rds-global-bundle.pem`, the CA bundle the image pins ([`Dockerfile`](Dockerfile)). Not a secret: the boot log prints it, a password in it masked. |
| `DATABASE_PASSWORD` | none | The role's password, kept apart from the URI because it is a secret (the vault's). Unset, libpq's own sources apply (a password in the URI, `~/.pgpass`). |
| `DATABASE_POOL_MAX` | `16` | The most connections the server opens. A thread that needs one past them waits up to 30 s for a lease to come back, then its request fails `db_error`. |

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
- **One writer at a time.** A `db::Transaction` takes a database-wide advisory lock
  (the writer lock) after its `BEGIN` and holds it to its end, so a check and the write
  it decides (the hosted servers' cap, a registration's duplicate checks, the status
  blob's read-modify-write, a CD key's assignment, the first admin) never interleave
  with another transaction's, across processes too. Waiting for it past the lock
  timeout fails the write. Nothing slow runs inside one (bcrypt, the AWS CLI): every
  other writer would wait on it. A `db::ReadSnapshot` is `REPEATABLE READ, READ ONLY`:
  its reads see one snapshot and take no lock.
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
  session writes at most once a minute. The main tick's sweep prunes expired rows.
- **Revocation.** A deleted player takes its sessions and its known login addresses.
  An admin password reset or a status other than `active` (`PUT
  /api/admin/users/<id>`) moves the account's credentials generation
  (`players.credentials_gen`) and deletes its website sessions, its
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

## Build layout and tests

Every source but `main.cpp` builds as the static library
`opennova_novaworld_server_core` (mirroring `opennova_serve_core`); the exe is
`main.cpp` over it. All of it, the service target and their tests exist only with
`OPENNOVA_NOVAWORLD_SERVICE=ON` (the root CMake option, off by default; the Linux CI
jobs and the server image turn it on). The HTTP sources (`http_listener`, `host_routes`, `cd_keys`,
`cd_key_routes`, `web_access`,
`web_session`, `rate_limiter`, `template_engine`, `session_store`, `auth`, and the
hosted servers' `hosted/`) join the core only under
`BUILD_NOVAWORLD_HTTP`, which also defines `OPENNOVA_HTTP_ENABLED` PUBLIC so
`main.cpp` starts the listener; the header-only `loggable.h` serves every listener
either way, and `catalog_repository` (the games catalogue) builds in every core,
since the gate reads it too. The route harness, ctest
`opennova_novaworld_server_http_routes`
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
run one beside the build. The ctest `db`
([`tests/novaworld/db_test.cpp`](../../tests/novaworld/db_test.cpp)) drives the wrapper
itself: the binds and the types they read back as, errors and their SQLSTATE, savepoints,
read snapshots, the pool's limit, the writer lock, the lock timeout, a reconnect, and the
migration runner (its records, a failed file rolled back, a changed file refused). The
ctest `opennova_novaworld_server_signal_shutdown`
([`tests/novaworld/server_signal_shutdown_test.cpp`](../../tests/novaworld/server_signal_shutdown_test.cpp),
POSIX builds) runs the real binary on a temp database with every port 0, sends
SIGINT and then SIGTERM to a fresh run, and expects exit 0 and the shutdown legs
logged in order; with the HTTP layer it reads the HTTP port
from the `[http] listening on :<port>` line and holds a half-sent request open
across the signal. On Linux a third run checks that the first signal puts the
default action back and the same signal again ends a shutdown still pending. A
last run probes the gate and opens a NovaWorld session: `UDPNOVAWORLD` names the
NW UDP listener's logged port and, with the HTTP layer, `STARTUPURL` and the
SessionInit's web domain the HTTP listener's. The ctest
`gate_listener` ([`tests/novaworld/gate_listener_test.cpp`](../../tests/novaworld/gate_listener_test.cpp))
starts the real `GateListener` on port 0 over the migrated and seeded games table and
checks that its reported port answers a probe and comes back as POSTIPPORT, that
`UDPNOVAWORLD` and `STARTUPURL` carry the sibling ports its config names, that each
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
limiter also have Crow-free ctests that compile those sources straight in and run on
every build: `web_session`
([`tests/novaworld/web_session_test.cpp`](../../tests/novaworld/web_session_test.cpp),
the store and the roles on a migrated database of their own) and `rate_limiter`
([`tests/novaworld/rate_limiter_test.cpp`](../../tests/novaworld/rate_limiter_test.cpp),
the buckets on a hand-stepped clock and the trusted-proxy client address); the
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
account's push is a 404, and the owner's stop ends the listing. The hosted servers:
`hosted_supervisor`
([`tests/novaworld/hosted_supervisor_test.cpp`](../../tests/novaworld/hosted_supervisor_test.cpp),
every build; it compiles `auth`, `web_session` and `hosted/` straight in) drives the
settings' rules and renders, the data sets, the service accounts, the request's checks
(the fleet's room among them) and every server transition over a fake machine driver
whose machines' agents the test plays through the supervisor's sync
([`tests/novaworld/hosted_fake_driver.h`](../../tests/novaworld/hosted_fake_driver.h),
[`hosted_fleet_rig.h`](../../tests/novaworld/hosted_fleet_rig.h)) on a hand-stepped clock:
two servers on one shared machine's slots, the own machine, the stops, any exit while
stopping, the crash restarts (an exit, the OOM kill, a failed start, a refused launch),
their limit and the restart window, the timeouts, the health checks, a lost machine
placing its server again with no restart counted, the owners' changes including a
withdrawn comp grant and a canceled support tier, the owners' limits (a tier lowered, past
due, the add-on cut) and their mass-stop hold, the reconciliation, the console ring's cap,
and the admin links following the running servers; `hosted_fleet`
([`tests/novaworld/hosted_fleet_test.cpp`](../../tests/novaworld/hosted_fleet_test.cpp),
every build) the fleet's side over the same fake: the desired state each sync gets (the
files until reported, a stopping server's unstarted container, the drain flag, a
container answered for without files after a restart), a boot that never reports, a lost
machine terminated or ready again, an idle shared machine ending, a drain moving its
servers one at a time, an admin's end, the reconciliation of the cloud's machines with
the rows and the periodic sweep; `hosted_stats`
([`tests/novaworld/hosted_stats_test.cpp`](../../tests/novaworld/hosted_stats_test.cpp))
the sampler on a hand-stepped wall clock (the counter's differences and its restart, the
CPU's share, the unique players never stored, the missions, the rollup, the machines'
month, the prune) and the reads over each range for one server, an account and the
fleet; `hosted_placement` the planner (and the request's
`place_after`); `hosted_fleet_repository` the fleet's rows; `hosted_agent_route` the agent
routes on the internal listener and the local driver end to end; `hosted_ec2_machine_driver`
the EC2 driver on a stub `CommandRunner`; `command_runner`
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
scripted remote-admin server on loopback (the session, CSRF and owner refusals, every
command and its reply, the arguments refused before a byte is sent, the rotation's
checks (a full rotation, between matches), the admission (one request in flight per account, one rotation edit per server), hostile replies kept valid JSON, the write bucket, the console ring, the admin's routes, the refusals before
running, with no link and while stopping, and no line off the curated list reaching the
server); `hosted_console_commands`
([`tests/novaworld/hosted_console_commands_test.cpp`](../../tests/novaworld/hosted_console_commands_test.cpp),
every build) the curated list itself, its compositions and its reply mapping;
and `hosted_local_e2e`
([`tests/novaworld/hosted_local_e2e_test.cpp`](../../tests/novaworld/hosted_local_e2e_test.cpp),
`BUILD_NOVAWORLD_HTTP` only) two real `opennova-serve` children on one local machine's
slots, the machine's stand-in agent bootstrapping and syncing against the internal
listener: started from the site, each listed as its service account at its slot's game
port and shown as its owner's, the first renamed by the owner's `SetServerName` (its
`game.cfg` saved in its slot), administered over the supervisor's admin link (`PLAYER
LIST`, a recorded `CHAT SEND` in the console ring, no password in a log or console line),
and both stopped from the site, the link going down with the first. `hosted_admin_link`
([`tests/novaworld/hosted_admin_link_test.cpp`](../../tests/novaworld/hosted_admin_link_test.cpp),
every build) runs the admin link against a real `opennova-serve` child on the LAN network
type with the hosted renders' `admin.cfg`: the login, `PLAYER LIST`, `CHAT SEND` read back
by `CHAT GET`, the reply framing (two packets, one, the quiet gap, each followed by a
command in step), the status poll, the reconnect after the child is killed and started
again on the same port, `QUIT` at the end, and the password in no log or console line;
`admin_protocol` and `admin_server` pin the reply-count table itself, against the
engine's own console.

The support tiers: `billing_stripe` (every build) the transport, the typed client, the
webhook signature and the settings; `billing_entitlement`
([`tests/novaworld/billing_entitlement_test.cpp`](../../tests/novaworld/billing_entitlement_test.cpp))
the entitlement's truth table (the statuses, the period's net, comp against tier, the
add-on beside a tier, past due, an inactive or service account) and the billing tables;
`billing_sync`
([`tests/novaworld/billing_sync_test.cpp`](../../tests/novaworld/billing_sync_test.cpp))
the worker over the in-memory Stripe (an event synced into the entitlement, a redelivery,
another instance's customer and the orphans, a wiped cache rebuilt by a reconcile, the
backoff, a deleted customer, the price check); and the route harness's `billing_routes`,
`billing_webhook`, `billing_admin` and `admin_user_detail` cases the routes over the
same fake, the webhook's signatures computed with `io::hmac_sha256`, and its `healthz`
case the probe and the watchdog on a hand-stepped clock.
