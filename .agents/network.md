# Network Architecture Guardrails

The intended runtime is one architecture with different discovery and transport
entry points. LAN and NovaWorld should differ before the game socket is chosen,
not after the in-match protocol starts.

## Canonical Path

- The host owns an authoritative `World`.
- The frame order is fixed: drain C2S input, run `World::run_logic_tick`, emit
  S2C snapshots, then present decoded client state.
- `HostSessionAccept` owns JointOperations host admission, load-gate handshake,
  SCRK/session state, and framing S2C for remote peers.
- `JoinerSession` owns the in-match joiner state machine and C2S framing.
- `NetSystem` owns per-frame world-to-wire replication after peers are admitted.
- `NovaUdpPump` owns UDP I/O in Godot. `libs/netsim` stays socket-free.

## LAN, NovaWorld, and Dedicated

- LAN host: `GameWorld.load_mission_as_host()` -> `MissionRuntime` ->
  `NovaSimulation.enable_host_listen()` -> `HostSessionAccept` + `NetSystem`.
- LAN joiner: `GameWorld.load_mission_as_joiner()` ->
  `NovaSimulation.enable_join()` -> `JoinerSession` + `NetClientView`.
- NovaWorld host: `NovaWorldHost` registers a real listen host with the gate and
  browser. It is not a second gameplay server.
- NovaWorld join: `NovaWorldClient` should stop at lobby, browser, NWJoin, and
  proto-switch plumbing. After resolving host, port, and mission, gameplay must
  enter the same joiner runtime used by LAN.
- Dedicated: a future world-backed authority with no local player, no loopback
  client, no camera, no present pass, and no audio. It should reuse
  `HostSessionAccept`, `NetSystem`, and the same mission/world simulation loop.

## Module Ownership

- `libs/novacrypto`: NWU, EPASK, PUB, CRC, and crypto primitives only.
- `libs/napi`: envelope, TLV/container, and session serialization helpers.
- `libs/novaworld`: retail protocol/session state, lobby/browser payloads,
  `ClientSession`, `HostSessionAccept`, `JoinerSession`, `GameSession`, and
  `GameServerRuntime`.
- `libs/netsim`: world-to-wire replication seam, connection table, client state,
  loopback/queue-shaped transports, and entity wire bridge.
- `godot/engine/network`: thin Godot socket, HTTP, and signal wrappers.
- `godot/engine/simulation`: authoritative runtime orchestration and frame order.
- `godot/engine/world`: mission flow, presentation, and local UI glue.
- `apps/novaworld_server`: NovaWorld backend service: gate UDP, lobby UDP, HTTP,
  DB, host registry, and partial JO responder. It is not the authoritative
  mission game server until it owns a real world-backed runtime.

## Paths Not To Confuse

- `NovaNetClient` / `NetWorldView` is replay/spectator receive plumbing, not the
  canonical co-op gameplay client.
- `ClientSession` is the NOVAWORLDUDP lobby verifier/proto-switch client, not
  in-match replication.
- `GameServerRuntime` sounds like the game server, but today it is bootstrap and
  session/load helper state. Live spawned peer replication belongs in
  `NetSystem`.
- `UdpSessionTransport` is an internal identity-frame conduit. NWU, SCRK, PN, and
  ProtocolMessage framing stay in `libs/novaworld`.

## Initial-Load Failure Triage

For a retail client that joins an OpenNova host but floats or sees no map
entities, do not start with a broad refactor. First capture the full handshake
and inspect exact S2C order around:

`0x10`, live `0x0C`, live `0x20`, `0x45`, `0x7E`, `0x1A`, and `0x0F`.

Then verify in order:

- Mission identity and local `.bms` availability.
- Terrain/load `0x45` pages for the chosen mission.
- Spawn position in `0x0F` against the mission start marker and terrain height.
- Spawn-marker `0x20` records, preserving original pool indices.
- Joiner self-ID/DCB: the self `0x0C` is sent before deploy with the retail
  ack value and local-player flags.

Do not reintroduce full static pool streaming as a blanket fix. Retail clients
load static mission data locally; the host stream should be the required dynamic
state and spawn markers.

