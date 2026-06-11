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
   Until then, `PN="JointOperations"` sessions are accepted at the hello layer and
   recorded by the unknown-message tracker (`pn:` channel), so real-world traffic
   shapes the priority order.
4. **Existing in-match groundwork is experimental.** `libs/novaworld`'s `game_session`,
   `game_server_runtime`, `replication_min`, and `libs/bms` (the net-side spawn-point
   parser) relanded with the PR #37 stack and keep their tests, but they are
   explicitly experimental until they are rebuilt against this seam with per-tag
   witnesses. New gameplay-net code must not grow on top of them in the meantime.
   `libs/bms` additionally duplicates a slice of `libs/mission`'s BMS support;
   convergence on one parser is part of the real implementation.

## Consequences

- The 62-frame logic cadence and tick determinism are preserved by construction:
  networking injects commands before the tick and reads state after it, never during.
- Single-player keeps `LocalSink` and pays nothing.
- The first real in-match milestone (host serves the §5.5 BMS-state bundle and the
  spawn flow to a retail client) slots in without touching WAC/BMS/AI code.
- Anything currently reading `TickContext.is_authority` is already on the correct side
  of the authority split and needs no change when a real server arrives.
