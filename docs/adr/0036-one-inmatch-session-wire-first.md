# ADR 0036: one in-match session, wire-first compatibility

- **Status**: accepted (2026-08-22; full cutover)
- **Owners**: runtime architecture, in-match networking, gameplay
- **Supersedes**: ADR 0009 decisions 1-2 as a public module topology and ADR
  0035 decision 1's `opennova::np::MissionSession` name/location. The witnessed
  frame order and all retail-wire requirements remain in force.

## Context

The first multiplayer gameplay slice joined three things that had previously
been developed in isolation: authoritative rules, the round-end transaction,
and the retail byte stream. The old architecture described `npwire`,
`npruntime`, and `netsim` as three peer layers and separately named a
`MissionSession` lifecycle seam. In practice those names exposed source
organization as architecture and encouraged forwarding boundaries without
adding information.

Retail interoperability is the non-negotiable constraint. The in-memory model
and module layout are not compatibility surfaces; preserving them would make
the code harder to change without helping a retail peer.

## Decision

1. `engine/net/npwire` is the retail compatibility boundary. Its encoders,
   decoders, message catalogue, signedness, field order, chunk sizes, and
   transaction order are proved against original-engine witnesses and exact
   bytes. OpenNova-to-OpenNova consistency never substitutes for that proof.
2. `opennova::inmatch::Session` in `engine/net/inmatch/session.*` is the one
   public owner of an active match's lifecycle, role policy, fixed cadence,
   retained input, and typed tick/frame outcomes. Its one inversion point is
   `inmatch::TickTarget`, implemented by the Godot simulation and headless host.
3. Authoritative gameplay belongs to `world::Match`, not to protocol handlers.
   It owns rules, the 42-field retail player stat block, team rows, the match
   clock, objective carrying, winner evaluation, and the immutable
   `MatchResult`. Every retail mode (DM, TDM, KOTH, TKOTH, S&D, A&D, CTF,
   FlagBall, Flag Me, A&S, C&C, and the co-op family) finishes through
   `World::process_round_end`; the network runtime serializes that frozen
   result and does not decide the outcome a second time.
4. `npruntime` and `netsim` remain implementation directories inside the net
   group, not public architectural layers or extension seams. Concrete target
   adapters and focused protocol tests may use their machinery directly. New
   forwarding facades, callback buses, and interfaces that merely mirror an
   adjacent module are rejected; deepen `Session`, `Match`, or `npwire`
   instead when a real invariant belongs there.
5. This is a full cutover. The old `npruntime/mission_session.*` files, types,
   target, tests, and Godot member are removed. There is no alias,
   compatibility include, deprecation period, or second lifecycle path.
6. Earlier instructions are evidence, not permanent constraints. When an ADR,
   plan, or repository mandate conflicts with the cleanest design that still
   preserves witnessed behavior and wire bytes, amend or supersede it in the
   same change rather than encoding the conflict into another layer.

## Consequences

- Retail byte compatibility can be reviewed independently of our object model.
- Gameplay modes share one scoring/outcome model and one round-end network
  transaction, so adding a mode tests the architecture instead of copying it.
- Godot and `nw-server` consume the same lifecycle API while retaining their
  necessary device/socket adapters.
- The source tree still contains `npruntime` and `netsim`; their presence does
  not authorize callers to treat them as stable product APIs.
- Internal API breakage is resolved by updating all callers in one commit.

## Verification

- `tests/world/match_test.cpp` pins every retail score schema, primary-score
  selector, automatic win arm, clock/draw rule, flag transition, and immutable
  result. These branches follow `Server_CheckWinConditions @0x51AD40`,
  `GameEvent_ProcessScoring @0x52F550`, and
  `GameType_CreateDefaultSettings @0x52DD00`. It also preserves Flag Me's
  retail row-12 defect: objective wire transitions remain live, but the
  12-row scorer/status table rejects its score row.
- `tests/npruntime/round_end_test.cpp` pins multiplayer and co-op through the
  shared live transition, exact objective-state/event routing, `0x61`/`0x1D`
  delivery, requester-only `0x2B`/`0x56` board pulls, and the 2790-tick linger
  (`Server_ProcessRoundEnd @0x5164F0`).
- `tests/npruntime/client_runtime_test.cpp` pins automatic multi-chunk
  client pulls and board reassembly.
- `tests/frame/inmatch_session_test.cpp` pins the sole lifecycle/cadence API.
- `tests/npruntime/server_session_test.cpp` and
  `tests/netsim/two_peer_fanout_test.cpp` pin StartDelay as one explicit World
  phase: the 62-tick authority decrement and transition frame, frozen match
  clock, phase-0 `0x0A` low byte, retained client mirror, and C2S
  drain-without-apply. The witnesses are `reset_round_counters @0x516C8D`,
  `Server_TickUpdate @0x51D8BD/@0x51DC20..0x51DC33`, and
  `NetPacket_WritePlayerState @0x4FF82D`.
