# ADR 0011 — Single-player is the in-process listen server (network-shaped from day one)

Status: accepted. Amended by [ADR 0043](0043-canonical-cpp-and-godot-hard-cut.md) (2026-09-02): the
`INetCommandSink` / `LocalSink` / `SerializingSink` seam is deleted. The 0x23 payload is the WAC VM's script remote
command (producer `WacScript_ExecuteBytecode @0x4F58B0`, consumer
`GameMode_DispatchRemoteCommand @0x4F81E0`), ported 2026-09-09: the outbound seam is the typed
`World::out.script_remote_commands` queue drained by `Server_TickUpdate`; every other entity command
runs locally through `EntityCommands`.

## Context

ADR 0009 fixed the in-match seam (one `NetSystem : ISystem` ahead of WAC, `INetCommandSink`
outbound, the wire enumeration as the RE record) and recorded the consequence *"single-player keeps
`LocalSink` and pays nothing."* That consequence assumed SP would run the simulation directly and
deliver entity state to the present pass without going through the wire.

The original engine does **not** work that way, and we now have the witness. From
[docs/net/novaworld-net-re.md §5.0/§5.2a](../net/novaworld-net-re.md) and a 2026-06-16 grill of the
delivery path:

- `[orig: SinglePlayer_StartMission @ 0x561af0]` starts SP via `[orig: CGameSession_SetConnectionMode
  @ 0x4c49f0]` mode **3** (host + client) and `[orig: CNapiNetwork_SetTransportMode @ 0x4c8750]` mode
  **1** (socketless — `[orig: CNapiNetwork_OpenTransportSocket @ 0x4c6a40]` is reached only for modes
  2/3/4), then the shared SP/MP `[orig: CNapiGameSession_CreateSession @ 0x4c97c0]` →
  `[orig: NapiNPProtocol_StartServer @ 0x62b5e0]`. SP, co-op, and MP are **one architecture**; only
  client count and transport differ. The host's own player is a **server-side entity** built by
  `[orig: Server_BuildPlayerInfoAndAdd @ 0x51d560]`, not a wire-received one (§5.2a).
- The host emits each S2C as a **wire byte message** — `[orig: NapiNPServer_SendFiltered @ 0x4C87E0]`
  → `[orig: NapiNPServer_SendToConn @ 0x4c4f20]` → `[orig: CNapiNPConnection_QueueMessage @ 0x628640]`
  — never by pointer to the client handler. The receive side `[orig: NapiNPProtocol_PumpRecvQueues @
  0x6266a0]` drains a **byte circular-buffer FIFO**, dispatches by opcode, then
  `[orig: CNapiNPConnection_ParseMessages @ 0x625bc0]` runs the msg_id dispatch — the **identical byte
  path** for socket and in-process. Mode 1 only skips `sendto`/`recvfrom`; it loops the serialized
  datagram back into the same recv FIFO in-process.

So the faithful SP path is a **literal in-process byte loopback**, not a direct snapshot delivery. The
in-engine serialize/parse round-trip — fixed-point position compression (`Network_CompressFixedPoint
@ 0x4C2780`), the per-class serialize callbacks at `ItemDef+356` (§5.10/§5.13/§5.14), and the
receive-side spawn-flag side effects (the per-frame `0x0A` `flags1 & 0x01` that clears the spawn gate,
§5.2a) — all run under SP. The user has chosen to build SP **network-shaped from day one** to match
this, so MP/co-op is the same loop with the transport swapped rather than a later retrofit.

## Decision

1. **SP runs through the seam, not around it.** This **supersedes ADR 0009's "single-player keeps
   `LocalSink` and pays nothing."** Single-player stands up the in-process listen server: the
   authoritative `World` tick (the host = `Server_TickUpdate` equivalent) serializes real entity state
   through `NetSystem`/the per-class callbacks, an in-process channel loops the datagrams back, and a
   local client decodes them via `libs/novaworld/ingame_decode`. `is_authority`
   stays the authority seam; `INetCommandSink` stays the outbound boundary, now backed by a serializing
   sink for SP rather than `LocalSink`.

   *Amended 2026-08-26 (D-NET-140 closed).* The original text made the present pass read the
   client-decoded state on every role. Retail does not: the listen host's own player receives a
   **header-only** 0x0A (anchor, flags1, phase byte, and the phase-0 block when `phase & 3 == 0`;
   no priority build `@0x517c1b`, no records/rounds/terminator `@0x50f07e`, parser return
   `@0x430174`) and its local client **reads process memory** (`Terrain_CollectVisibleEntitiesForTerrain
   @0x5c8c60` walks the pools). The port now does the same: the host's loopback carries the
   session/lifecycle stream (the 0x0C/0x0D/0x10/0x20 spawn batches, 0x50, kill feed, chat, timers)
   plus that header-only 0x0A, and **the host's present pass reads its own pools**
   (`Simulation::present_snapshot_from_world`); a joiner presents the state its
   `ClientReplicaPipeline` decoded. `Server_TickUpdate` builds the priority snapshot only when some
   in-match recipient takes entity records.

2. **The in-process channel is transport mode 1: byte serialize + loopback, socket bypassed.** The
   reimpl mirrors the witnessed path — host queues wire messages, the channel delivers the serialized
   datagrams into the local client's recv path in-process (no UDP socket). MP/listen-server (ADR 0009
   Phase, this plan's Phase 4) keeps this exact path for the host's own local client (mode 1) and adds
   real sockets (modes 2/3/4) for remote peers.

3. **Crypto on the loopback is scoped, pending one witness.** The `0x43`/`0x83` recv dispatch decrypts
   with SCRK, so the SCRK stream cipher (byte-exact in `libs/novacrypto`, NW-C1) **likely** runs
   end-to-end in-process. Until the transmit-side encrypt on the loopback is byte-witnessed (§5.0
   open item R2.b), the in-process channel MAY bypass crypto/framing as an implementation convenience;
   remote peers (Phase 4) always use the witnessed wire crypto. If the witness shows crypto runs
   in-process, the bypass is removed — no architectural change either way.

4. **Data structures are shaped from the witnessed wire field maps.** The player intent/state, entity,
   and serialize-contract types are the union of the §5.2a host-spawn track and the §5.9/§5.10/§5.13/
   §5.14 field maps, so the serializer is an encoder over a faithful model, never a model rewrite. A
   loopback identity test (serialize → decode → client view, asserted field-identical to the
   `ingame_decode.h` structs) guards the seam as gameplay grows on it.

## Consequences

- The 62-frame logic cadence and tick determinism are preserved (ADR 0009): net injects C2S before the
  tick and reads S2C state after it. SP frame order is `input → net(drain C2S) → World::run_logic_tick
  → net(emit S2C) → present` (`Game_ProcessMainFrame @ 0x5263f0`).
- The wire libs `game_session`/`game_server_runtime`/`replication_min` are promoted from ADR 0009's
  "experimental" to seam-backed **as their per-tag ENCODE side lands with IDA witnesses** (decode is
  already witnessed in `ingame_decode.h`). The map-locked fixture blobs are replaced by real-state
  encoders synthesized from `bms::File` + `World` state, per ADR 0003 (no raw passthrough).
- SP costs the serialize/parse round-trip it pays in the original — the price of parity and of MP being
  a transport swap, not a rewrite. The previously-assumed "SP pays nothing" optimization is given up
  deliberately. That round-trip is the session/lifecycle stream plus the header-only 0x0A; entity
  records are not part of it (retail's local player receives none — the 2026-08-26 amendment).
- The player is an authoritative pool-0 `World` entity spawned by the §5.2a host machine (see the
  companion ADR 0012); SP player input feeds it through the same `entity+286`/`entity+36` gates the
  wire path uses.
