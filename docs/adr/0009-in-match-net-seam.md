# ADR 0009 — In-match networking enters the world tick through one seam

Status: accepted (design only; no implementation in this ADR)

## Context

The NovaWorld service stack (gate, session, browser, accounts) is landed and covers
matchmaking. In-match gameplay traffic is a separate protocol layer: at CLIENT_HELLO the
`PN` field selects the message set, and `PN="JointOperations"` switches the session to
the NAPI msginfo dispatch — the per-tag S2C/C2S tables enumerated in
[docs/net/novaworld-net-re.md §4](../net/novaworld-net-re.md). That layer moves entity
state, player input, chat, spawn flow, and file transfer between a game host and its
players. We are not implementing it yet, but code that will eventually sit under it is
already in the tree, and the engine APIs it plugs into are being designed now. This ADR
fixes the seam so nothing lands that the real implementation would have to undo.

The original engine's frame order is `input -> net -> logic tick -> render -> audio`
(`Game_ProcessMainFrame @ 0x5263f0`); the logic tick is the single consolidated
`World::run_logic_tick` this engine already has (one `World`, `ISystem`s WAC -> BMS ->
AI, 62-frame cadence, `TickContext.is_authority` as the authority seam).

## Decision

1. **One entry point.** In-match traffic enters the simulation as a `NetSystem :
   ISystem` registered ahead of WAC in the system order, mirroring the original's
   net-before-logic frame position. Inbound messages decoded by `libs/novaworld` are
   queued as entity commands and drained into the world at the top of the net system's
   `tick`; outbound deltas are gathered after the logic tick from the same snapshot
   machinery the present pass uses. No other system talks to a socket, ever.
2. **`INetCommandSink` is the outbound boundary.** `libs/world/world.h` already defines
   it (`is_authority(EntityHandle)` + `send_command(owner, command_id, args, argc)`,
   modeled on `NapiNPServer_SendFiltered(..., 0x23, ...)`), with `LocalSink` as the
   single-player default. The net implementation replaces the sink; gameplay systems
   keep calling the same interface and stay network-unaware.
3. **The wire enumeration is the RE record, not the code.** The §4 dispatch tables
   (S2C `0x82AE28`, C2S `0x82B5D8`) are the authoritative tag inventory. Implementation
   proceeds tag-by-tag with IDA witnesses, the same way the matchmaking stack was built.
   Until then, `PN="JointOperations"` sessions are accepted at the session layer and
   routed to the experimental `GameServerRuntime`, so real-world traffic shapes the
   priority order. As of the ADR 0010 join leg the OpenNova *client* now
   *emits* this seam hello: after NWJoin it opens a session to the host and sends a
   `ClientHello` with `PN="JointOperations"` (`NovaWorldClient::send_jointops_hello`),
   then stops. The server now has the PN route, but the game-runtime implementation
   remains experimental and tag coverage is still being built out.
4. **Existing in-match groundwork is experimental.** `libs/novaworld`'s `game_session`,
   `game_server_runtime`, `replication_min`, and `libs/bms` (the net-side spawn-point
   parser) relanded with the PR #37 stack and keep their tests, but they are
   explicitly experimental until they are rebuilt against this seam with per-tag
   witnesses. New gameplay-net code must not grow on top of them in the meantime.
   `libs/bms` additionally duplicates a slice of `libs/mission`'s BMS support;
   convergence on one parser is part of the real implementation.
   *(Update 2026-07-04: that convergence landed — `libs/bms` was deleted in the
   2026-07 restructure (#179) and `libs/mission` is the one BMS parser.)*

## Consequences

- The 62-frame logic cadence and tick determinism are preserved by construction:
  networking injects commands before the tick and reads state after it, never during.
- Single-player keeps `LocalSink` and pays nothing.
- The first real in-match milestone (host serves the §5.5 BMS-state bundle and the
  spawn flow to a retail client) slots in without touching WAC/BMS/AI code.
- Anything currently reading `TickContext.is_authority` is already on the correct side
  of the authority split and needs no change when a real server arrives.

## Amendment (2026-06-16) — superseded by ADR 0011/0012 where noted

The single-player implementation grill (docs/net/novaworld-net-re.md §5.0/§5.2a, `[orig:
SinglePlayer_StartMission @ 0x561af0]` and `[orig: Server_SendInitialGameStateToPlayer @ 0x51bba0]`)
witnessed that the original runs single-player **through** the in-match seam — SP is an in-process
listen server (`SetConnectionMode(3)` + socketless `SetTransportMode(1)`), serializing real entity
state to its own local client. This refines three points of this ADR:

1. **Consequence "single-player keeps `LocalSink` and pays nothing" is superseded by
   [ADR 0011](0011-single-player-in-process-listen-server.md).** SP runs through `NetSystem` and a
   serializing sink over an in-process byte loopback; it pays the serialize/parse round-trip the
   original pays. `is_authority` / `INetCommandSink` remain the seam exactly as decided here.

2. **Decision 1 (`NetSystem : ISystem` ahead of WAC) is now the active implementation target,** not a
   design placeholder. The host frame order is `input → net(drain C2S) → World::run_logic_tick →
   net(emit S2C) → present` (`Game_ProcessMainFrame @ 0x5263f0`), per ADR 0011.

3. **Decision 4 (the wire libs are "experimental") is lifted tag-by-tag as ENCODE witnesses land.** The
   decode side is already witnessed (`ingame_decode.h`); the host serializer map is now witnessed (§5.2a
   "Server-side S2C serializer map"). `game_session`/`game_server_runtime`/`replication_min` are rebuilt
   against the seam with the map-locked fixture blobs replaced by real-state encoders (ADR 0003). The
   player model is fixed by [ADR 0012](0012-player-is-host-side-server-entity.md).
