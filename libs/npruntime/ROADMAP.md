# `libs/npruntime` — clean-slate in-match client + server roadmap

The IDA-faithful, Godot-free, headless-testable redesign of the NovaWorld (Joint Operations)
in-match networking client and server. Mirrors the original engine's function flow / type names /
global names, reuses the verified codec libs, and proves parity by driving the new code with the
golden pcaps. **Server-first.**

This file is the working build plan + live status. The approved design lives in the session plan
`ok-so-we-have-gleaming-puzzle.md`; the witness record is `docs/net/novaworld-net-re.md`;
governing decisions are ADRs 0009–0012.

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
- **Retire (after callers migrate, P8):** `libs/novaworld/game_session.{h,cpp}` +
  `game_server_runtime.{h,cpp}` (the tag-burst order in `game_session.cpp` is the *spec* for
  `Server_SendInitialGameStateToPlayer` — read it, don't keep it); `replication_min`
  `build_tag_*` builders (keep the POD structs).

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

### P4 — Per-frame host loop (SP)
`Server_TickUpdate` (`server_tick.h`): **(1)** drain C2S (`NapiNPProtocol_Pump` →
`PumpRecvQueues` → `DispatchOpcode` → `DispatchMessage`; in-match `0x0C` →
`dispatch_entity_packet_callback` → `NetPacket_SerializePlayerState` SNAP) **(2)** `World::run_logic_tick`
**(3)** `NetSystem::emit_s2c` per-connection fan **(4)** flush. Finish the netsim stubs
(`SerializingSink::send_command`, per-connection emit fan, loopback). Reconcile the connection
table: `NapiNPProtocol.connection_list` vs `NetSystem`'s internal table (decide single owner).
Bar: gameplay golden `0x0C`-in → `0x0A`-out round-trip.

### P5 — Headless client runtime
`client_runtime.{h,cpp}`: connect legs (`GATE → HELLO → AUTH/VERIFY → READY → PLAY`, reusing
`ClientSession` + promoted `JoinerSession`) + `Game_ProcessMainFrame` client role
(`Client_ProcessNetworkFrame` packs C2S `0x0C`; `NetClientView::pump` folds S2C into `ClientState`).
Bar: headless client ↔ headless server in-process full round-trip; gameplay replay folds into
`ClientState`.

### P6 — Real UDP transport (MP)
`UdpSessionTransport` real NWU/CRC/SCRK framing swap at the owner boundary; `apps/nw_server`
`main.cpp` (`apps/common/net_sockets` + a fixed-cadence `Server_TickUpdate` loop); connection
modes 2/3/4. Guard the CMake so `apps/nw_server` never pulls godot-cpp. Bar: two-endpoint over
real sockets; full `retail-lan-host-join.pcapng` end-to-end.

### P7 — Godot adapter rewrite
`nova_simulation.cpp` becomes a thin adapter over `npruntime` (construct World + NetSystem + ctx;
call the frame driver each `_process`); the net bindings become pure socket pumps, no protocol
logic in Godot. Bar: `/gut` green; SP launches and renders via `ClientState`.

### P8 — Retire legacy glue
Delete `game_session`, `game_server_runtime`, and the `replication_min` `build_tag_*` builders
once unreferenced. Bar: full ctest + GUT green; no references to retired symbols.

## Test harness (built up across phases)

- **GoldenSession loader** (`tests/npruntime/`): load a golden via
  `apps/common/pcap_reader::stream_pcap_udp_file` → `libs/novaworld/wire_capture`
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
- Runbooks: `.agents/network.md` (architecture guardrails, module ownership, frame order),
  `.agents/interop.md`, `.agents/ida.md`, `.agents/debug.md`.
- Promote-from: `libs/novaworld/include/novaworld/{host_session_accept.h,joiner_session.h}`.
- Seam to finish: `libs/netsim/include/netsim/{net_system.h,connection.h,session_transport.h,serializing_sink.h,udp_session_transport.h}`.
- Test pattern: `tests/novaworld/nw_pool_groundtruth_test.cpp`, `tests/netsim/*`,
  `tests/novaworld/nw_pcap_stream_test.cpp`.
