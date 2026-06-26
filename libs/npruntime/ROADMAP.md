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

### ▶ P2 — Per-connection handshake (NEXT)
Promote `HostSessionAccept` legs onto `NapiNPProtocol::handle_client_hello/_join/_session/_goodbye`
and `JoinerSession` onto a client `NapiNPConnection`, decoupled from `GameServerRuntime`. Keep the
SCRK/seq/ack + handshake `Phase` state on `NapiNPConnection` (the node already has the
`ConnectionPhase` enum and a `netsim::Connection link`). Port the **F3 dcb-timing fix** and the
**D.0 name-match** verbatim.
- Read first: `libs/novaworld/include/novaworld/host_session_accept.h` + `src/host_session_accept.cpp`
  (server legs, `PeerState`, F3, the `0x41→0x81 / 0x42→0x82 / 0x43→0x83` dispatch), and
  `joiner_session.{h,cpp}` (client legs, D.0 self-ID), plus `game_server_runtime.{h,cpp}` to see
  the coupling being removed. Framing helpers: `nw_session_framing.h`, `protocol_message.h`.
- Bar: ported `host_session_accept_test` / `joiner_session_test` pass against `npruntime`; the
  golden `retail-lan-host-join-session.pcapng` `0x81`/`0x82` reply bytes match (seed-inject the
  session id / SCRK / `host_start_tick` from the golden, byte-compare after a small normalization
  mask for monotonic seq/timestamps).

### P3 — World bring-up + initial S2C burst
`Server_InitNewRoundState → ProcessPendingPlayerSpawns → BuildPlayerInfoAndAdd → PlayerAdd`
spawns the pool-0 player in `World` (ADR 0012; writes `entity+0x78` ownerConnectionId).
`Server_SendInitialGameStateToPlayer` = the §5.2a two-track machine, every body from
`ingame_encode` over real `World`+`bms::File` state (ADR 0003, no fixtures):
- player-sync track: `0x2C → 0x08 → 0x2A×6 → 0x1C → 0x0B → 0x66 → 0x76 → 0x11`
- world-stream track: `0x10 → 0x0D → 0x0C → 0x20 → 0x45 → 0x7E → 0x1A` → game-state 9

**Retire `game_session.cpp` fixtures here.** Bar: `retail-lan-host-join.pcapng` burst order + bodies
match §5.2a; `World` has the spawned player.

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
