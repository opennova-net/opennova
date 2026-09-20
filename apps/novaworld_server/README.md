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

- **The main thread owns the database and the tick loop.** It runs
  `manager.tick(now)` every `tick_interval_ms`, then on the same tick flushes the
  `UnknownTracker` and runs the throttled host-staleness sweep. All routine DB
  mutation funnels through here.
- **Each listener runs on its own internal thread(s).** They mutate shared state
  through the `ConnectionManager` verbs (add / refresh / drop) and the
  `NwUdpListener`'s `lobby_states_` map (guarded by `lobby_states_mu_`). SQLite is
  built `THREADSAFE=1`, so writes from a listener thread are serialized by the
  engine; the higher-level invariants (the per-peer session-state map, the unknown
  accumulator) carry their own mutexes.
- **The `UnknownTracker` records from listener threads** into an in-memory,
  mutex-guarded accumulator and is flushed to the DB only on the main tick — never
  per packet.

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
nineteen keys the retail parser recognises. Expansion-publish pipeline: `EXPANSION_GITHUB_TOKEN` (PAT the
server tags the expansion repos with) and `EXPANSION_PUBLISH_TOKEN` (bearer the
repos' CI presents to `/admin/internal/.../publish`); both empty by default.
Seeding: `SEED_DEV_USERS=1` applies the dev-only `0002_dev_users.sql` (the
`test`/`foo` accounts) — leave unset in production. Build + run via Docker:
[`Dockerfile`](Dockerfile) and the compose files under [`deploy/`](../../deploy/).
