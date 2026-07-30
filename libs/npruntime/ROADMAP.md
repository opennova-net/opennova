# `libs/npruntime` — the in-match client + server build record

The IDA-faithful, Godot-free, headless-testable redesign of the NovaWorld (Joint Operations)
in-match networking client and server. Mirrors the original engine's function flow / type names /
global names, reuses the verified codec libs, and proves parity by driving the new code with the
golden pcaps. **Server-first.**

> **Status: the P0–P8.2 build-out below is COMPLETE (closed 2026-06-27).** This file is now the
> record of *how the runtime was built* and why each phase decided what it did — read it for the
> module map, the frame order, what P8 retired, and the test-harness design. It is **not** live
> status: in-match work since P8.2 (world-stream paging, the 0x0A/0x18 residual repairs, the
> retail-join fidelity rounds) is tracked per-divergence in
> [`docs/divergence-ledger.md`](../../docs/divergence-ledger.md) (the `PAR-NET` slice) and witnessed
> in [`docs/net/novaworld-net-re.md`](../../docs/net/novaworld-net-re.md) §8. Start there for what
> is open today.

The witness record is `docs/net/novaworld-net-re.md`; governing decisions are ADRs 0009–0013 and
0019. The original approved design lived in a session plan that was never tracked in this repo;
everything load-bearing from it was folded into the phase sections below.

## Why

The old in-match glue grew empirically against captures and is overworked:
`libs/novaworld/game_session.cpp` (1332-line phase machine with hardcoded retail fixtures +
ack-loop hacks), the `replication_min` `build_tag_*` builders (quarantined/superseded), the
`netsim` deferred stubs, and the SP/host/joiner entanglement in
`godot/engine/simulation/nova_simulation.cpp`. The codecs themselves (`novacrypto`, `napi`, the
`novaworld` encode/decode/framing/capture layer) are byte-witnessed and solid. This effort
rebuilds the *runtime* on top of those codecs as one faithful, maintainable core.

## Locked decisions

1. **Home:** new portable lib `libs/npruntime` (`opennova::np`) + a new headless app
   `apps/nw_server` (in-match host, separate from the matchmaking `apps/novaworld_server`).
2. **Fidelity:** mirror function names 1:1 and field names/order with `// [orig: Name @ 0xADDR]`
   / `// [orig +0xNN]` comments, idiomatic C++ types. **Wire bytes stay byte-exact**; in-memory
   layouts do NOT reproduce padding.
3. **Tests:** byte-exact comparison, **fully local** — pcap tests env-gate on the local goldens
   and **skip cleanly when absent** (no committed derived oracle), mirroring
   `tests/novaworld/nw_pool_groundtruth_test.cpp`.

## Reuse vs rewrite

- **Keep / build on:** `libs/novacrypto`, `libs/napi`; `libs/novaworld` codecs (`ingame_decode`,
  `ingame_encode`, `protocol_message`, `nw_session_framing`, `session_hello`, `session_keys`,
  `wire_capture`, `replay_timeline`, `serverlog_decode`); the `libs/netsim` seam
  (`ISessionTransport`, `LoopbackChannel`, `Connection`/`NetSystem`, `NetClientView`/`ClientState`,
  `EntityWireBridge`, `PlayerIntent`) — finish the stubs, don't redesign; `apps/common/pcap_reader`;
  `ClientSession` / `LobbySession`.
- **Promote (keep proven logic, re-target):** `HostSessionAccept` → `NapiNPProtocol` server
  handshake legs; `JoinerSession` → `client_runtime` in-match leg. **Port the F3 dcb-timing fix
  and D.0 name-match verbatim — do not "simplify".**
- **Retired (P8, DONE):** `libs/novaworld/{game_session,game_server_runtime,host_session_accept,
  replication_min}.{h,cpp}` + `session_protocol`'s `dispatch_in_match_session_messages` + the
  `replication_min` `build_tag_*` builders. The `game_session.cpp` tag-burst order was the *spec* for
  `Server_SendInitialGameStateToPlayer` (read, not kept — the empirical `tick()`/`queue_*` machine had no
  original-engine counterpart and was dropped); the reactive §5.1 replies moved to
  `npruntime/server_message_dispatch.cpp`, the per-frame `0x0A` adapter to `netsim/net_system.cpp`. Kept
  the POD structs `PlayerReplicationState` / `GameEntitySnapshot`.

## Phases

Each phase is green on ctest (scope: `ctest --test-dir build -C Release -R "npruntime|novaworld|netsim"`)
plus the applicable golden pcap.

### ✅ P0 — Scaffold (DONE)
`libs/npruntime` (CMake target, registered in root CMake after `libs/netsim`). Data types +
enums defined: `NapiNPServerCtx` / `NapiNPProtocol` / `NapiGameSettings` / `NapiNPConnection`
(`napi_np_server_ctx.h`, `napi_np_connection.h`); `ConnectionMode` / `NetworkType` / `SocketMode`
/ `ConnectionPhase`. Bar met: builds; `npruntime_server_session` test green.

### ✅ P1 — Host bring-up (DONE)
`server_session.{h,cpp}`: `set_connection_mode` / `set_transport_mode` / `create_session` /
`start_server` (§5.0). Volatile host-start values (`host_key`, `host_start_tick`,
`session_seed_id`) are passed in via `SessionStartup` for determinism / golden seed-injection.
Bar met: `host_running==1`, the §5.0 mode booleans, one type-2 loopback connection registered;
dedicated-host variant has no local client. All in `npruntime_server_session_test`.

> Refinement vs plan: the witness splits `+0x50 transport_mode` (NovaWorld/LAN network type) from
> `+0x54 socket_state` (the socketless-vs-socket selector `SetTransportMode` writes). Both modeled
> faithfully; `netsim::TransportMode` stays the per-connection mode on `NapiNPConnection.link`.

### ✅ P2 — Per-connection handshake (DONE)
Promoted `HostSessionAccept`'s legs onto `opennova::np` free functions over `NapiNPServerCtx` /
`NapiNPProtocol.connection_list` (`napi_np_protocol.{h,cpp}`): `handle_server_datagram` dispatches
`handle_client_hello`→0x81 / `handle_client_join`→0x82 / `handle_client_session`→0x83 /
`handle_client_goodbye`, plus `tick_connections` / `frame_in_match_s2c` / `bind_connection_player`.
`JoinerSession` promoted to `np::JoinerConnection` (`joiner_connection.{h,cpp}`) over a type-2
`NapiNPConnection`. The per-connection SCRK/seq/ack + handshake latches fold onto `NapiNPConnection`
(`PeerState.self_id` unifies onto `connection_id`/`+0x18`). The **F3 dcb-timing fix** (dual-path
streaming-entered latch) and the **D.0 name-match** are ported verbatim. The 0x43 spawn-gate machine
is still driven through a reimpl-owned `ctx.game_runtime` (`GameServerRuntime`, retired P8 / replaced
by the World-driven spawn at P3). Tests: `npruntime_handshake_server` (all 5 sub-runs incl. F3
ordering, tag51, tag46, non-JO), `npruntime_joiner_connection` (real legs ↔ real `JoinerConnection`
↔ real `NetSystem` apply), `npruntime_golden_lan_join_session` — all green; full `novaworld`/`netsim`
suite unaffected.

> Refinement vs plan (lifecycle composition): P0/P1/P2 compose into one authoritative listen-host
> bring-up — `set_connection_mode` → `set_transport_mode` → `create_session(..., local_client)`
> (P1: `is_in_session`/`host_running`, registers the type-2 loopback) → `configure_session_runtime`
> (P2: builds the runtime, clears only the **type-1 remote-joiner** nodes so the host's own loopback
> survives). The live legs are gated faithfully: `handle_client_hello`/`_join` reject until
> `is_authority && host_running` (`[orig: CNapiNetwork_ValidateJoinRequest @0x4c61b0]`), and
> `handle_client_join` re-validates the JO identity + HK echo (`classify(pn)==JointOperations`,
> `host_key==0 || auth.hk==host_key`; `[orig: NapiNPProtocol_HandleClientJoin @0x62b750]`) so a
> non-JO or wrong-HK 0x42 is dropped. The tests now walk the real lifecycle (`bring_up_host`) and
> add `run_listen_host_lifecycle` (loopback preserved through reconfigure),
> `run_handshake_rejected_when_host_down`, and non-JO/wrong-HK 0x42 rejection coverage.

> Refinement vs plan (golden parity): the golden `retail-lan-host-join-session.pcapng` is a LAN
> host/join — it starts mid-handshake (0x81 retransmits) so the **0x41 ClientHello is not captured**,
> and full-datagram byte-parity of 0x81/0x82 is **not** achievable in P2 because the remaining
> divergence lives entirely in the **`libs/novaworld` session builders** (`build_server_hello` /
> `build_server_auth`), not in the promoted legs. The golden test therefore asserts the field-level
> parity the legs *control* (0x81 HK/PN/PG; 0x82 CI/CK/CR/SK/SCRK/NA, seed-injecting host_key/SK/SCRK)
> and logs the deferred builder gap rather than inventing values (faithful-port rule). Measured vs a
> LAN host: 0x81 — retail `ServerHello.CI` = host node index (2, not the client echo), the
> game-server field block (`is_game_server`: SF/P1/P2/NP/MP) is present, identity strings are the
> retail build's; 0x82 — `MI` is host-specific (retail 3 vs our 0x113f default), the CS control-field
> *values* are the retail set (ours are onnet's `[UNVERIFIED]` values), and a LAN host emits **no CU**
> block (we append novaworld name/url/nwuid). **Follow-up:** a `libs/novaworld` session-builder grill
> (witness CI/MI/CS/game-server-block/LAN-CU-suppression in IDA) to reach full 0x81/0x82 byte-parity.
>
> **GRILLED 2026-06-27 (D-NET Wave 3):** 0x81 `NapiNPProtocol_SendServerInfoPacket @0x6204b0` —
> **FIXED** (`server_hello_to_bytes` now the flat builder: SF unconditional, P1/P2/NP/MP nonzero-gated,
> no PL fiction; D-NET-16/18). 0x82 `CNapiNPConnection_SendSessionInit @0x620ef0` — **witnessed**
> (full field order + the CS x30 `{dir,idx,u32 timeout_ms}` shape + CU-from-type-3-msg-queue with
> LAN-suppression; IDB comment @0x620ef0). 0x82 **CU-gate FIXED** — `build_server_auth` gained an
> `include_novaworld_cu` param (default true; npruntime `make_server_auth_datagram` passes
> `transport_mode == NovaWorld`), so a LAN/SP host emits no CU (the witnessed LAN behavior). The CS
> values were already the witnessed engine template (D-NET-1; `engine_cs_fields` matches it exactly —
> the "[UNVERIFIED] onnet" note was stale). **0x82 is now faithful**; the residual golden byte-diff is
> only host-specific seed values (CI host-node-index / MI host dcb / identity strings / live UT), not
> builder logic. **Wave 3 (0x81 + 0x82 session builders) DONE.**

> P2 follow-up fixes (grill 2026-06-26, docs/net §5.0a — D-NET-104/105/106): a code review of the
> promotion found the join leg had only part of the witnessed `0x42` behavior. Now witnessed against
> `Jointops.exe` and ported (all green):
> 1. **Retransmit re-sends, never re-mints (D-NET-104).** `HandleClientJoin @0x62b750` FindConnection-s
>    first; an already-joined node with matching CI+CK re-sends the cached `0x82` via
>    `NapiNPConnection_SendSessionInit @0x620ef0` — the promoted legs were re-minting the SCRK/SK on
>    every `0x42`, stalling joins on a normal lossy-UDP retransmit.
> 2. **MI = the host-assigned ConnectionId/dcb (D-NET-105).** The `0x82` MI TLV carries `conn+0x18`
>    (the join-order dcb `++protocol[947]`), the client adopts it and echoes it in its `0x48`, and the
>    host stamps it into the joiner's `0x0C` `ownerConnectionId`. The host now assigns + ships MI +
>    latches `self_id_seen` on a LAN listen host, and the bundled `JoinerConnection`/`JoinerSession`
>    now read MI + emit the `0x48` — so the F3 streaming-entered gate finally trips for an opennova
>    client (it was dead: the client never sent a `0x48`). This also closes the "MI is host-specific
>    (retail 3 vs our 0x113f default)" parity gap noted above — MI is now the assigned dcb, not the
>    placeholder.
> 3. **Capacity gate (D-NET-106, npruntime only).** `handle_client_join` rejects a join when
>    host-loopback + already-joined joiners `>= max_players` (`CNapiNetwork_ValidateJoinRequest
>    @0x4c61b0`). The reject is modeled as a silent drop (the overlay-reject packet is not modeled
>    yet). `libs/novaworld/host_session_accept.cpp` does NOT model the capacity layer and is left as a
>    tracked divergence (retires at P8); fixes 1 & 2 ARE mirrored in both copies.
>
> Tests: `npruntime_handshake_server` gains `run_retransmit_0x42_keeps_keys` +
> `run_capacity_rejects_when_full`; `npruntime_joiner_connection` now asserts the host surfaces F3 for
> a real `JoinerConnection` (no test-crafted `0x48`). `npruntime|novaworld|netsim` all green.

### ✅ P3 — World bring-up + initial S2C burst (DONE — structural)
`Server_InitNewRoundState → ProcessPendingPlayerSpawns → Server_BuildPlayerInfoAndAdd`
(`server_spawn.{h,cpp}`) spawns the pool-0 player in `World` over `world::spawn_player` /
`spawn_remote_player` (ADR 0012), writing `entity+0x78` ownerConnectionId — the host's own type-2
loopback at the wire-proven host dcb (`kHostPlayerDcb=2`, joiners assigned from 3), surfaced through
`entity_wire_bridge`'s 0x0C `entity_flags`. `Server_SendInitialGameStateToPlayer`
(`server_initial_state.{h,cpp}`) is the §5.2a two-track machine over a per-connection `InitialStateBurst`
cursor, every emittable body from real `World`+`bms::File` (ADR 0003, no fixtures):
- player-sync track: `0x2C → 0x08 → 0x2A×6 → 0x1C → 0x0B → 0x66 → 0x76 → 0x11`
- world-stream track: `0x10 → 0x0D → 0x0C → 0x20 → 0x45 → 0x7E → 0x1A` → game-state 9

The world-stream bodies reuse the proven `entity_wire_bridge` pool extractors (0x0C/0x20) + the empty
0x10 marker; 0x0B = `bms::encode_header_blob`; 0x1C/0x11 empty. The spawn-gate latches (F3
`PeerEnteredWorldStreaming` / `PeerSpawned`) were **re-sourced verbatim** off `conn.burst`; `ctx.world`
is the path selector (the World-driven burst when wired, else the P2 `game_runtime` GameSessionState
mirrored onto `conn.burst` — so the P2 unit tests stay value/tick-identical). `game_runtime` is kept
only for the §5.1 joiner handshake-reply bodies (retired P8). Tests: `npruntime_server_spawn`,
`npruntime_initial_state_burst`, `npruntime_golden_lan_join` (all green); the existing
`npruntime_handshake_server` (F3 ordering) / `npruntime_joiner_connection` / `..._golden_lan_join_session`
unaffected; full ctest 223/223.

> Refinement vs plan (scope): **structural P3** — the ~8 §5.2a serializers with no witnessed byte
> format (`0x2C/0x08/0x2A/0x66/0x76/0x45/0x7E/0x1A`) are **emitted as nothing + logged once**, never
> faked (faithful-port rule; mirrors P2's deferred-builder-gap). Full burst byte-parity is the
> follow-up grill wave (witness each serializer @ the addresses cited in `server_initial_state.cpp` →
> `ingame_encode`, land via `re-doc`). The fixture burst in `game_session.cpp`
> (`queue_mission_bootstrap` / `queue_state4_loading_gate`) is no longer on the npruntime spawn/burst
> path; its full deletion is P8, once `game_runtime`'s §5.1 reply role is also grilled.

> Refinement vs plan (golden): the `retail-lan-host-join.pcapng` host→joiner S2C stream decodes to the
> §5.2a order **byte-for-byte** — `2C 08 2A×6 1C 0B 66 76 11 | 10×N 0D×7 0C 20×N 45 45 7E 1A | 5A 5A
> 42 0A 0F …` — and `npruntime_golden_lan_join` asserts that ordering (player-sync before world-stream;
> `0x10→0x0C→0x20→0x1A`) against the capture, validating the model on ground truth. Body byte-parity is
> print-only/deferred (our world-stream bodies are built from our own `World`, not the capture's
> mission). **Observation for the grill wave:** the retail host streams pool-1 `0x0D` and many full
> `0x10` records, whereas our host follows D-NET-97/98 (empty `0x10`, pool-1 `0x0D` omitted) to avoid
> the joiner's C2S-`0x0F` flood — reconcile whether the joiner suppresses its local `.bms` pool-1/2
> spawn when *joining* (vs hosting), which would let the host stream them without conflict.
> *(Since answered by D-NET-194: the retail joiner never opens the `.bms` at all — its world is
> built entirely from the wire `0x0B` header + spawn stream, so the host can stream freely.)*

### ✅ P4 — Per-frame host loop (SP) (DONE)
`Server_TickUpdate` (`server_tick.{h,cpp}`): **(1)** drain C2S (`NapiNPProtocol_Pump` →
`PumpRecvQueues` → `DispatchOpcode` → `DispatchMessage`; in-match `0x0C` →
`dispatch_entity_packet_callback` → `NetPacket_SerializePlayerState` SNAP) **(2)** `World::run_logic_tick`
**(3)** per-connection S2C `0x0A` fan **(4)** flush.

**Single-owner decision:** `NapiNPProtocol.connection_list` is the one connection table (each node's
embedded `netsim::Connection link`). `Server_TickUpdate` walks it directly and drains/emits over each
`conn.link`; it constructs/registers NO `NetSystem` and registers NO net ISystem (so the C2S queue
never double-drains — P7 guardrail noted in `server_tick.h`). `NetSystem` stays byte-for-byte unchanged
as the *legacy* `nova_simulation` table until P7 folds it onto `connection_list`; `ctx.net` stays
declared-but-unused (removed at P7). To keep ONE drain/emit implementation, the two `net_system.cpp`
file-statics were promoted to public free functions `netsim::drain_connection_c2s` /
`emit_connection_s2c`; `NetSystem::tick`/`emit_s2c` became thin loops over them (behavior-preserving —
all `netsim_*` tests stay green).

**Deliberately NOT changed:** `handle_client_session` (it still only *surfaces* `PeerC2SInMatch`; the
owner reframes it onto `conn.link.transport->push_inbound`, the proven path — npruntime has no
production `PeerC2SInMatch` consumer yet, so an inline apply would be dead code + a P7 double-apply
trap). The per-frame `0x0A` codec is unchanged: `build_tag_0a_world_reference` already returns
`encode_frame_update` (the witnessed §5.9 bytes `decode_frame_update`/`NetClientView` round-trip) — the
P8 cleanup just lifts the snapshot→`FrameUpdate` adapter into netsim. `SerializingSink::send_command`
stays the structural no-op: the `0x23` entity-command body is unwitnessed and the sink has zero callers
(faithful-port — never invent bytes; grill `NapiNPServer_SendFiltered` 0x23 then port once a caller exists).

Bar met: `npruntime_golden_gameplay` — the gameplay golden's real retail C2S `0x0C` (2351 extended
uplinks) drains → SNAPs the owned entity → the emitted S2C `0x0A` anchor IS that entity's post-SNAP
position (opennova↔opennova self-consistency); full-body byte-parity vs the capture is DEFERRED
(our World ≠ the capture's ASH_G3D entity set), printed not asserted, mirroring `npruntime_golden_lan_join`.
Env-gated `NW_GOLDEN_GAMEPLAY` + skip-clean. `npruntime|netsim` ctest 13/13 green.

### ✅ P5 — Headless client runtime (DONE)
`client_runtime.{h,cpp}` (`np::ClientRuntime`): the faithful per-frame client role `[orig:
Client_ProcessNetworkFrame @0x42c180]` (witnessed + landed net-re §5.44) composed with the connect-leg
state machine and the S2C→`ClientState` fold. Frame order is the witnessed **recv-pump → raw-input pack
→ C2S `0x0C` (gated `!is_authority`) → send-pump**. **Role-aware** (the original runs the same
not-authority-gated frame on every machine): **Joiner** drives the in-match connect legs (reusing the
P2-promoted `np::JoinerConnection` — Idle→Hello→Auth→Driving burst→InMatch — NOT the lobby
GATE→VERIFY→READY→PLAY flow, which is the separate ADR-0010 `ClientSession` matchmaking path owned by the
binding) then per frame folds S2C and emits the `0x0C` uplink; **HostClient** is the SP host's own
loopback view (handshake-less, `is_authority`, recv-fold only, `0x0C` suppressed).

> Refinement vs plan (connect legs): the original blurb's `GATE → HELLO → AUTH/VERIFY → READY → PLAY`
> conflated the matchmaking lobby flow with the in-match leg. The IDA witness confirms only HELLO→AUTH→
> in-match-burst→spawn is the in-match runtime; the lobby legs stay in `ClientSession` (§7.1).

**Seam additions:** `netsim::NetClientView::apply(tag,body)` (the remote-wire fold path; `pump` is the
loopback path — one fold path per role), `netsim::ISessionTransport::deliver_c2s` (uniform C2S
inbound-inject), `np::apply_in_match_c2s` (the production `PeerC2SInMatch` consumer, D-NET-126), and the
single `np::is_in_match(conn)` predicate shared by `Server_TickUpdate`'s drain + emit (resolving
D-NET-121/122 — the host loopback's own per-frame `0x0A`, anchored to its player).

Bar met: `npruntime_client_runtime` (always-on) — the full in-process client↔server round-trip
(handshake → spawn → per-frame `0x0C` → `apply_in_match_c2s` → `Server_TickUpdate` drain/SNAP + `0x0A`
fan → `NetClientView` fold) + the host-as-client D-NET-121/122 anchor path; `npruntime_golden_client`
(env-gated `NW_GOLDEN_GAMEPLAY`, skip-clean) — the real emission path reproduces the captured retail
`0x0C` **inner message byte-for-byte** and re-frames the captured 9-message bundle into a **byte-identical
datagram** (emitted C2S == client-origin), with the S2C `0x0A` fold cross-checked against the
world-stream spawn positions (0.87 world units, non-circular). `npruntime|netsim|novaworld` ctest green
(55/55). **Deferred-and-logged (P6):** the per-frame housekeeping (`0x34` keepalive / `0x4C` anti-cheat /
`0x2C` RTT ping) and the `send_holdoff_countdown` gate.

### ✅ P6 — Real UDP transport (MP) (DONE)
A real-UDP **owner loop** drives the runtime over sockets — "real transport" is NOT a new socket/
protocol layer (the runtime already emits/parses fully-framed NWU/CRC/SCRK datagrams via
`handle_server_datagram` / `frame_in_match_s2c` / `tick_connections`); it is `apps/nw_server` driving
`apps/common/net_sockets` against those entry points at a fixed 62 Hz cadence. `UdpSessionTransport`
stays an identity byte-conduit; the framing/crypto stays in `libs/novaworld` (the owner reframes a
popped inner `[tag][body]` via `frame_in_match_s2c` and routes inbound raw `0x43` through
`handle_server_datagram`) — the `.agents/network.md` guardrail, satisfied without changing the transport.

- **`libs/npruntime/{include/npruntime/host_session.h,src/host_session.cpp}`** (shared by `main.cpp`,
  the NovaWorld listener, and the socket tests — one wire owner loop) +
  **`apps/nw_server/main.cpp`** (a headless **dedicated host**: `HostOnly`, with no
  synthetic loopback player; loads a mission via `NW_MISSION`, drift-free `sleep_until` 62 Hz loop, SIGINT
  shutdown). The per-tick body is the §5.44 recv-before-send order: recv-drain → `tick_connections`
  (pre-spawn §5.2a bursts) → `Server_TickUpdate` (single C2S drain + logic tick + 0x0A fan) → S2C flush
  (`pop_outbound` → `frame_in_match_s2c` → `sendto`). The joiner spawn is AUTOMATIC (`tick_connections`
  → `Server_ProcessPendingPlayerSpawns` binds `owned_entity`); the owner only attaches the peer's
  `UdpSessionTransport` and streams the joiner's NAMED dcb-bearing `0x0C` (mirrors
  `NovaSimulation::announce_joiner_organic_spawn`). **CMake godot-cpp guard is structural** — godot-cpp
  is a separate SCons build never in this CMake graph; linking only `opennova_*` static libs makes the
  target incapable of pulling it (there is no toggle).
- **§5.44 housekeeping ported** into `ClientRuntime` (the P5 deferred-and-logged set): `0x34` keepalive
  (29760-tick, both roles), `0x4C` net-quality (310-tick, Joiner in-match), `0x2C` RTT ping (every
  deployed frame), and the `send_holdoff_countdown` send-block gate — all `[orig @0x42c1a9..0x42c4bc]`,
  via a new public `JoinerConnection::frame_inner(tag,body)` so they ride the same `0x43`/SCRK envelope
  as the `0x0C`. **Witness correction (re-doc §5.44):** `g_tag2CSendCooldown` (`@0xA860D8`) is set-to-62
  + self-decremented but is **NOT read as a send gate** (the only three xrefs are this fn's read/dec/set)
  — so the `0x2C` fires every deployed frame, NOT throttled 62-tick as §5.44 first phrased. `seed_session`
  sets a `replay_mode_` that suppresses the housekeeping so a seeded golden replay reproduces only the
  captured `0x0C` (`npruntime_golden_client` byte-parity preserved). HostClient emits no housekeeping
  (recv-only loopback) — deferred-and-logged.
- **`np::drop_connection(ctx, peer)`** added — owner-initiated eviction for the recv-timeout / dead-peer
  path (no `0x46`); mirrors the goodbye teardown without surfacing an event.

Bar met: **`npruntime_two_endpoint_socket`** (always-on) — a `ClientRuntime` joiner and the
`apps/nw_server` owner loop, each on its own bound loopback UDP socket, run a full join → spawn → play
loop over real `sendto`/`recvfrom` (single-thread poll-pump): the joiner reaches InMatch+deployed, the
peer SNAPs to its C2S `0x0C` over the wire, no synthetic host player exists, and the S2C `0x0A` folds
into `ClientState`. It also asserts the host's emitted §5.2a S2C tag order and (env-gated
`NW_GOLDEN_LAN_JOIN`, skip-clean) cross-checks it against `retail-lan-host-join.pcapng` — **passes against
the local golden**. `apps/nw_server` live-hosts a real 1333-entity JO mission at 62 Hz. `npruntime|netsim|
novaworld` ctest green (the 16 affected + the 41 net scope). The `0x10`/`0x0D`/`0x1A` and the unwitnessed
§5.2a serializers stayed deferred at P6 (structural P3) — **now closed by D-NET Wave 1 (P8.2 below)**.
The retail `0x57` RTT pong and field-3 send holdoff are **also now LANDED**: the server bounces C2S
`0x2C` echoFlag!=0 as S2C `0x57` and batches that reactive reply with the same tick's `0x0A` under
the 1300-byte send ceiling, while `ClientRuntime` queues reliable replies/housekeeping and gates the
complete send pump for the requested frame count (`NapiNPConnection+0x648`; §5.34/§5.44).

### ✅ P7 — Godot adapter rewrite (DONE — in-match core + lobby sweep)
`nova_simulation.cpp` is a thin adapter over `npruntime` — every in-match path funnels into one
runtime, the binding owns sockets/signals only.

- **In-match core — DONE** (`346da20c`): SP listen + LAN host construct `NapiNPServerCtx` + `World`
  + the host owner loop (`host_pump` = recv→`handle_server_datagram` → `tick_connections` →
  `Server_TickUpdate` → per-type-1 S2C reframe; `dispatch_event`/`admit_peer` named-0x0C announce),
  the host's own dcb-2 loopback folds via a HostClient `ClientRuntime`; the joiner is a Joiner-role
  `ClientRuntime` (`joiner_pump`). Faithful host-player auto-spawn (`Server_ProcessPendingPlayerSpawns`:
  host `0xFFF0`, first joiner `0xFFEF` downward). Dropped the NetSystem-ISystem + the separate
  `run_logic_tick` (D-NET-123/125); `ctx.net` stays null. ~470 lines of dead legacy net code removed
  from the binding (legacy libs still compile for their other callers — P8 deletes them). Three new
  owner helpers: `np::mark_host_client_in_match`, `np::admit_synthetic_peer` (the no-handshake test
  admit), `ClientRuntime::spawn_pose()`.
- **Present unified on the listen server — DONE** (`942bddc5`): the no-net AI-pool present is retired;
  `MissionRuntime` always stands up the listen server (editor preview included). The editor in-place
  Simulate + the debug-overlay live entity-list are degraded/un-tested (accepted — editor sim de-scoped;
  the ~24 sim-coupled editor/debug tests were deleted/updated). Scalar `get_entity_*` getters stay.
- **B1 — `libs/novaworld/http_flow` DONE** (`e4036223`): `LobbyHttpFlow` owns the EPASK login / GSB /
  NWJoin machines (URL builders + cookie jar + LoginStep/JoinStep), reusing the existing byte helpers;
  literals byte-preserved; ctest `http_flow` green. **Nothing consumes it yet.**
- **B2/B3 — DONE** (`42c49b62`): the lobby bindings are now pure pumps over `LobbyHttpFlow`.
  **B2** = new `libs/novaworld/lobby_vars.{h,cpp}` (`HostRegistration` + `make_host_var_list`/
  `make_host_request`/`make_host_update`→`NapiMessage`; `az_fingerprint` + `LobbyIdentityParams`/
  `make_lobby_identity_vars` = the NW-S5 10-var identity set; `parse_host_port`) — the host var-builders
  + the client identity assembly + the `UDPNOVAWORLD` split moved out of BOTH bindings; ctest
  `lobby_vars` proves byte-equality vs the old hand-assembly. The host keeps
  `verify_cookie_vars={{NWUID,""}}` (the 10-var set is client-only). **B3** = `nova_world_client` holds
  one `opennova::LobbyHttpFlow flow_`; `sync_flow_context()` snapshots the gate/session outputs into
  `LobbyHttpContext` at each leg-init (NEVER inside a leg callback — keeps `http_base()` stable
  mid-login); `ship_spec()` maps `HttpRequestSpec`→`HTTPRequest`; the 3 `request_completed` callbacks
  feed `on_login`/`on_gsb`/`on_join_response` and re-ship `NeedRequest` on the same per-leg node (mind
  `on_gsb_response`'s arg order: body 3rd, out 4th, no headers). Deleted ~355 lines (login/GSB/join
  machines + `cookie_jar_`/`epask_`/Login+JoinStep). D-1 (flagged): async join `Failed` → uniform
  `STATE_CONNECTED` fallback. **Verified:** `lobby_vars`+`http_flow` ctest green; 23/23 net ctest green;
  GDExtension compiles; `nova_world_client`/`host` GUT 4/4 each; and a **live headless our-stack lobby
  smoke** against the local Docker NovaWorld server (our `NovaWorldHost`+`NovaWorldClient`,
  `SEED_DEV_USERS` `test`/`test`) ran host-register → connect → login(TestPlayer) → GSB browse →
  NWJoin → `joined_game 127.0.0.1:32768`, the shared cookie jar carrying NWHANDLE/PCID into the join
  legs. Docker GOTCHA (Windows): NW UDP 64206 is in a reserved port range — remap (`ONNET_NW_UDP_PORT`)
  + run loopback with `ONNET_PUBLIC_HOST=127.0.0.1 ONNET_CLIENT_REFLECT_IP=127.0.0.1`. Pre-existing
  faithful note (not a regression): the server's STARTUPURL leaves `[GT]/[VER1]/[VER2]/[CC]`
  unsubstituted (`resolve_startup_url` only fills when `[domainname]` is present; the server tolerates it).

Bar (met): `/gut` full suite passing / 0 failing; net ctests green; SP launches and renders via
`ClientState`; lobby smoke (login → browse → join) green against a local NovaWorld server.

### ✅ P8 — Retire legacy glue (DONE)
The legacy in-match net glue is deleted; `npruntime` is the single in-match runtime. Deleted:
`libs/novaworld/{game_session,game_server_runtime,host_session_accept}.{h,cpp}` + `replication_min.cpp` +
`session_protocol`'s `dispatch_in_match_session_messages` (kept `classify_session_protocol`) + the retired
`build_tag_*` builders. Kept the POD structs `PlayerReplicationState` / `GameEntitySnapshot` — the
surviving `replication_min.h` header that holds them is now `replication_model.h` (the "min" implied a
minimal build; we are reimplementing the full server). (The `SpawnPointEntity` / `EntityBatchBuildResult` /
`kSpawnPointTypeIds` burst-input
structs went with the builders — no surviving caller). Bar met: full ctest **223/223** + the net scope
**15/15**; GUT green; no code references to the retired symbols.

The §5.1 gate (the prerequisite): a "grill-the-gate" IDA pass (net-re §5.45 / **D-NET-127**) confirmed the
reactive reply *structure* matches the witnessed `NapiNPServerMsg_0x0NN` handlers (`0x002 @0x512FD0`,
`0x022 @0x514C90`, `0x029 @0x514F10`) and that the one-shot join burst is `Server_OnPlayerJoin @0x51a680`
(§5.43 / D-NET-114) — NOT the empirical `tick()`/`queue_*`/state4 spawn-gate machine `game_session.cpp`
grew (a reimpl invention with no original-engine counterpart). So the faithful port (user-chosen "most in
line with the original engine") **dropped that burst machine** — `Server_SendInitialGameStateToPlayer`
over `conn.burst` is the sole spawn driver — and moved ONLY the reactive gameplay-message handlers into
the new `libs/npruntime/server_message_dispatch.{h,cpp}` (`dispatch_session_replies` over
`ctx.session_config` + the per-connection `NapiNPConnection.reply` state). Reply BODIES are carried
verbatim (captured-from-observation fixtures, D-NET-127); the faithful per-body serializer port + the
deferred §5.2a serializers + the `Server_OnPlayerJoin` join-burst tail (`0x42/0x0F/0x4D/seed/0x3E`,
to fold into `Server_SendInitialGameStateToPlayer`) stay the tracked grill-wave follow-up.

Migrations the deletion forced: `napi_np_protocol.cpp` (drop `ctx.game_runtime` + the mirror; reactive
replies via `dispatch_session_replies`); the per-frame `0x0A` adapter (`build_tag_0a_world_reference`)
lifted into `libs/netsim/connection_fan.cpp` (`build_0a_frame`); `apps/novaworld_server`'s JO routing
re-pointed off `HostSessionAccept` onto a World-less `np::NapiNPServerCtx` session responder;
`nova_simulation` `host_session_config_` retyped `GameServerRuntimeConfig` → `np::SessionReplyConfig`.
Tests: the P2 `handshake_server_test` migrated to assert the reactive §5.1 replies (the World-driven
spawn/F3 flow is covered by `npruntime_client_runtime` / `npruntime_two_endpoint_socket`); the World-less
game_runtime-mirror tests (`game_session`/`game_server_runtime`/`replication_min`/`host_session_accept`/
`joiner_session`/`joiner_connection`) removed; `session_protocol_test` trimmed to the classifier.

### ✅ P8.1 — consolidate the dead remnants of the retired path (DONE, 2026-06-27)

A structural cleanup pass after P8, removing the dead halves the retirement left behind (no behavior
change; 223/223 ctest + GDExtension green):

- **`netsim::NetSystem` class deleted.** Production (`Server_TickUpdate`) already drove the lifted free
  functions `drain_connection_c2s`/`emit_connection_s2c`/`build_0a_frame`; the class itself had no
  production consumer (only the 3 `netsim` tests). The file `net_system.{h,cpp}` was **renamed
  `connection_fan.{h,cpp}`** (no `NetSystem` left in it = the old name was a misnomer); it now holds
  only the per-connection drain/fan primitives (message tags now come from
  `npwire/ingame_message_id.h`). The 3 tests moved to a small
  explicit harness `tests/netsim/conn_fan_test_util.h` (a plain `std::vector<Connection>` + the free
  functions + `spawn_remote_player`), keeping every assertion.
- **`NapiNPServerCtx.net` field removed** (always `nullptr`; the "removed at P7" promise honored here)
  + its forward decl + the `nova_simulation` assignment + the `server_tick.h` guardrail note.
- **`libs/novaworld/joiner_session.{h,cpp}` deleted** (the carried-forward orphan, superseded by
  `np::JoinerConnection`) + its `CMakeLists.txt` entry.
- **Doc-integrity fix:** the D-NET-116/117/119 ID collisions (each assigned once as a §5.43 behavior
  entry and once as a §8 IDB-hygiene entry) are resolved — the IDB-hygiene trio renumbered to
  D-NET-128/129/130 (the code-cited behavior entries keep their numbers).

### ✅ P8.2 — port the §5.2a initial-state serializers (DONE, 2026-06-27, D-NET Wave 1)

The player-sync serializers that P3-P6 left as "emit nothing / deferred" are ported from the witnessed
originals (grilled vs `Server_SendInitialGameStateToPlayer @0x51bba0` and cross-checked byte-for-byte
against the retail-lan-host-join golden). See net-re §5.2a serializer-grill table for the full witness
map + verdict (MATCHING). Net effect: the host's §5.2a player-sync burst is now `2C 08 2A×6 1C 0B 66 76
11 | 10 0C 20 1A` (was `1C 0B 11 | 10 0C 20`).

- New `ServerRules` (the 0x08 block) + `weapon_restrictions` on `NapiNPServerCtx`; the serializers live
  in `server_initial_state.cpp` (0x2C serverName+mapFile, 0x08 server-config + `build_server_config_flags`
  @0x4c4dc0, 0x2A const table×6, 0x66 weapon-restrictions, 0x76 server-tick16, 0x1A timestamp).
- 0x45 terrain-delta + 0x7E briefing are emitted by the original ONLY when present; both `return 0` and
  the orig skips otherwise, so they are faithfully ABSENT on the headless host (not deferrals).
- IDB hygiene: `WriteTypeNameAndBaseName → NetPacket_WriteServerNameAndMapFile`; `byte_24D1FC4 →
  g_server_name_str`; `baseName → g_map_file_name` (the wire proved serverName+mapFile, not type/base).
- Tests: `npruntime_initial_state_burst` (full order + per-body byte assertions), `npruntime_golden_lan_join`
  (retail 0x2A byte-parity + 0x2C/0x08/0x66/0x76 structure-parity). 22/22 net ctest + GDExtension green.

## Test harness (built up across phases)

- **GoldenSession loader** (`tests/npruntime/`): load a golden via
  `apps/common/pcap_reader::stream_pcap_udp_file` → `libs/npwire/wire_capture`
  (`CaptureDecoder::push`) → ordered `(direction, tag, decoded-fields, raw-payload, ts, port)`
  events, partitioned host vs joiner by port. Env-gate on the golden path with a `DEFAULT_*_PCAP`
  fallback; skip if absent. New env vars: `NW_GOLDEN_LAN_JOIN`, `NW_GOLDEN_LAN_JOIN_SESSION`,
  `NW_GOLDEN_GAMEPLAY` (wire in `tests/CMakeLists.txt`).
- **Level 1 (unit, per tag):** decode golden payload → re-encode → assert byte-identical + fields.
- **Level 2 (server e2e):** replay client-origin datagrams in → assert emitted S2C == server-origin.
- **Level 3 (client e2e):** replay server-origin datagrams in → assert emitted C2S == client-origin
  and decoded `ClientState` matches.
- **Determinism:** seed-inject session id / SCRK / `host_start_tick`; normalization mask only for
  genuinely volatile witnessed fields (seq, timestamps). Byte-parity is the bar on the wire.

Goldens (local, gitignored, captured 2026-06-26): `.scratch/golden/retail-lan-host-join.pcapng`
(handshake + world-load), `retail-lan-host-join-session.pcapng` (session-only handshake),
`retail-gameplay-session.pcapng` (~8 min in-match `0x0a`/`0x0c` loop). Decode best with
`apps/nw_pp --items <ITEMS.DEF>`; each has a `.pcapng.txt` sidecar.

## Key references

- Spec / witnesses: `docs/net/novaworld-net-re.md` §3 (session flow), §4 (NAPI dispatch), §5.0
  (bring-up), §5.1/§5.2/§5.2a (load + spawn gates + host spawn flow), §5.9 (replication loop),
  §6.3–6.5 (structs). ADRs 0009–0012.
- Runbooks: `.agents/README.md` (the standing working rules and task routing),
  `.agents/interop.md` (capture, packet-diff, live-repro triage), `.agents/ida.md`,
  `.agents/debug.md`. (`.agents/network.md` is a redirect that points back here — this file
  is the architecture-guardrails / module-ownership / frame-order owner; do not chase the
  redirect in a circle.)
- Promote-from (both since deleted — `host_session_accept` retired at P8, `joiner_session`
  deleted at P8.1, recorded above): `libs/novaworld/include/novaworld/{host_session_accept.h,joiner_session.h}`.
- Seam: `libs/netsim/include/netsim/{connection_fan.h,connection.h,session_transport.h,serializing_sink.h,udp_session_transport.h}`.
- Test pattern: `tests/novaworld/nw_pool_groundtruth_test.cpp`, `tests/netsim/*`,
  `tests/novaworld/nw_pcap_stream_test.cpp`.
