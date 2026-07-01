# Track A: NovaWorld server reland, protocol completeness, IDA grill, engine client

## Reland mechanics (PR 3)

Source: `net/pr37-rescue` (merge commit `1bb779e1`, "Implement NovaWorld web and UDP
stack (#37)"). That lineage shares no usable merge base with current master, so the
reland is per-path extraction, not a merge:

```
git checkout 1bb779e1 -- libs/novacrypto libs/napi libs/novaworld libs/bms \
    apps/common apps/novaworld_server backend \
    third_party/sqlite third_party/bcrypt \
    tests/novacrypto tests/napi tests/novaworld tests/bms
```

Manual wiring ported from `git show 1bb779e1:CMakeLists.txt`:

- Root CMake: third_party sqlite/bcrypt, the `BUILD_NOVAWORLD_HTTP` option (FetchContent
  asio 1-30-2 + Crow 1.2.0), lib and app subdirectories. Net libs stay OUT of
  `OPENNOVA_CORE_TARGETS`/`opennova_shared`: no Python FFI consumer, and it keeps
  sqlite and socket-adjacent code out of the Blender FFI library.
- `tests/CMakeLists.txt`: the net registration block, names preserved so the scoped
  ctest regex (`gate|lobby|novaworld|napi|crypto|gsb|protocol_message|session|bms`)
  keeps working.
- RE doc §7 flips from "planned (unlanded)" to "landed on the integration branch".

Known overlaps, accepted as-is: `libs/bms` is a minimal net-side spawn-point parser that
overlaps `libs/mission`'s full BMS support; it relands experimental-marked with a
follow-up to converge. `lobby_*` identifiers in `libs/novaworld` name the browser and
session service and stay verbatim.

CI: the existing win+mac `test` matrix builds the net libs and runs the net ctests with
HTTP OFF. A new `novaworld-server` job (ubuntu-latest, `BUILD_NOVAWORLD_HTTP=ON`) builds
the full server and runs the scoped ctests; it is the repo's first Linux leg and the
deploy image (Track B) reuses its flags.

## What the relanded code already does

- All ten documented NOVAWORLDUDP session containers dispatched in
  `libs/novaworld/src/lobby_session.cpp`, with an `unknown:<name>` fall-through seam.
- `ConnectionManager` with a 120 s heartbeat window and
  `DropReason{Logout,Timeout,Replaced,Shutdown}`, boot-time stale-host purge, GOODBYE
  and ClientStopHosting teardown.
- Full legacy HTTP surface (`NWPrepare/NWStart/NWLogin/NWHost/NWJoin/NWLogout`,
  `*.gsb`) plus the `/api/*` backend (games, lobbies, stats, register, admin users and
  expansions), EPASK and PUBcrypto, retail templates, SQLite, bcrypt.

## Completeness work

**Unknown-message tracking (PR 9).** `libs/novaworld/unknown_tracker.{h,cpp}`:
thread-safe `record()` from listener threads, write-behind `flush(db)` on the main
tick. Signature channels: `gate:<KEY>`, `nwu:0x<op>`, `container:<Name>`,
`ptype:0x<full_msg_type>`, `pn:<PN>` (JOINTOPERATIONS traffic lands here),
`http:<METHOD> <path>`. Persistence in `backend/migrations/0004_unknown_messages.sql`
with `UNIQUE(channel, signature)`, count upsert, first-sighting sample capped at 512
bytes. Exposed via `GET /api/unknowns`. Samples can contain player handles; the cap and
a README privacy note cover the public-deploy case.

**Session and host lifecycle (PR 10).** Host-row and play-state teardown on every
`DropReason`, not just polite exits; a periodic staleness sweep on the main tick (~30 s
cadence, 5 min window, both in `server_config`, annotated `// policy:` because the
original server is unobservable from the client binary); thread-model notes in the
server README.

**`GET /api/server-info` (PR 9)** returns `novaworld_ip` and `redirect_hostnames` for
the launcher.

## IDA grill (PRs 7-8)

After the reland is green, before the lifecycle designs freeze. Instances: retail
Jointops.exe (JO:CA V1.7.5.7) and jodemo. Every session updates code, IDB, and
`docs/net/novaworld-net-re.md` together; verdicts (MATCHING / DIVERGENT, fixed /
POLICY) land in a new §8.

Wave 1: gate VAR set vs `CNapiGateManager` (NW-G1: the POSTIPADDRESS/POSTIPPORT
discrepancy; produce a per-binary required/optional VAR table), session envelope and
ServerHello/ServerAuth defaults, all ten containers field-for-field against what retail
actually reads. Plus launcher items NW-L1 (DFX2 gate hostname) and NW-L2 (non-gate URL
sweep).

Wave 2: EPASK/PUBcrypto, NWLogin cookie semantics, `.joi` and NK/BK token recipes, the
GSB-vs-GLB chunk-tag pin (NW-G2), teardown and heartbeat semantics (NW-G3).

Witness-vs-policy rule: client-observable behavior requires an IDA witness and an
`[orig: Name @ 0xADDR]` citation; server-internal choices (timeout durations, sweep
cadence) are policy and annotated as such.

## Cross-validation (PR 6)

`fixtures/novaworld/` (three `.nwmsg` capture runs plus jop templates) extracted from
the `worktree-net-final` branch. New `nwmsg_replay_test.cpp` streams every record
through NWU decode, envelope, and container parse; re-encodes and byte-compares where
deterministic. Direction rule: our `nwu_encrypt` corresponds to onnet's `nwu_decrypt`.

## Engine client (PRs 5, 19)

`NovaWorldClient` (GDExtension, `godot/engine/network/`) relands first as-is with a GUT
smoke. PR 19 then refactors it into a thin pump over a new Godot-free
`libs/novaworld/client_session.{h,cpp}` state machine mirroring `CNapiGateManager`:
IDLE, GATE_PROBE, HELLO, AUTH, VERIFY, READY, then BROWSE / HOST / PLAY, finally
GOODBYE. Bytes in, bytes out, event queue; sockets stay in the binding
(`PacketPeerUDP`, `HTTPRequest`).

Menu wiring uses the existing Command-by-name seam in `godot/game/menu_shell.gd`: a new
`novaworld_control_names` export plus `godot/game/novaworld_panel.gd` filling the .mnu
LIST widget. Copy stays artist-facing.

**Milestone:** from the game shell menu, our server verifies the session, the
novaworld_browser renders live rows, and Host a Game registers a row a second client
sees. Login uses the dev seed user until the HTTP login leg is grilled.

## Client completion — ADR 0010 (post-merge, on the trunk)

Phases 0-2 (endpoint picker, gate→hello→auth→verify session, GSB browse) landed with the
merge. Status 2026-06-12:

- **OpenNova target works end to end.** Our client reaches `Verified`/`CONNECTED` against
  `apps/novaworld_server` (loopback ctest + a live local run). This is the product path.
- **Real-NovaWorld target: NW-S5.** Against genuine NovaLogic NovaWorld (`gs.novaworld.net`
  = 207.178.209.201; web/asset host .204 — NovaLogic's boxes, the parity target, *not* ours)
  the client completes gate/hello/AUTH but the verify leg stalls (no `ServerStartVerify` after
  `ClientConnected`). Grill NW-S5 (RE doc Wave 4) found the cause is the missing HTTP login:
  our `ClientConnected` body is already correct (retail's is bare too), but retail (a) emits it
  only after `ServerSessionInit` and (b) carries a login-cookie `Cookie` var-list in the verify
  request. So NW-S5 resolves once Phase 3 lands; the reverted NW-S4 "empty-root-wrapper" guess is
  superseded.
- **Phase 3 — EPASK login.** Wire contract witnessed (NW-S5/B): the login is a POST of
  `application/x-www-form-urlencoded` to `NWLogin.dll` with EPASK-encrypted NAME/PASSWORD; the
  EPASK bundle is a Set-Cookie; the engine attaches all subnet-keyed cookies to *every* request,
  GSB fetch included (resolves the ADR Phase-2 open question). The Godot-free core landed:
  `libs/novaworld/http_login.{h,cpp}` (`build_credentials_post_body`, `parse_set_cookie_values`,
  `CookieJar`) with `tests/novaworld/http_login_test` proving the body round-trips through the
  server's decode path. **Remaining (host-side, needs the user's live real-NW test):** the
  binding's HTTP login chain (prepare GET → login POST → relay GET, cookie capture, feeding the
  verify `Cookie` var-list + join CU) and the panel's credential fields. Open product decisions:
  credential storage and whether login is offered/required per target.

## In-match seam (PR 20, design only)

ADR: PN=JOINTOPERATIONS gameplay traffic enters the single World tick as a
`NetSystem : ISystem` behind the existing `INetCommandSink` boundary in
`libs/world/world.h`, preserving determinism and the 62-frame cadence. The §4 msginfo
tables are the wire enumeration. `game_session`, `game_server_runtime`, the replication
model (the POD structs since consolidated into `replication_model.h`), and `libs/bms` stay
experimental pending that ADR.

## GameWorld rename (PR 2)

`godot/engine/world/nova_world.{gd,tscn}` becomes `game_world.{gd,tscn}` with
`class_name GameWorld`; usage sites in `main_game`, `mission_play_controller`,
`mission_workspace`, tests, and comment references update; docs gain a glossary entry
distinguishing GameWorld (the runtime world-sim host scene) from NovaWorld (the
service). Residue gate: a case-insensitive grep over `godot/` and `docs/` must hit only
net-domain files.
