# ADR 0036: one in-match session, wire-first compatibility

- **Status**: accepted (2026-08-22; full cutover). **Superseded in full by
  [ADR 0043](0043-canonical-cpp-and-godot-hard-cut.md) (2026-09-02)**: the
  session's banking, state machine and input retention survive unchanged as
  `inmatch::Session`, which now OWNS a `Role` (Local/Host/Joiner) instead of
  driving a `TickTarget`; `netsim`/`npruntime`/`inmatch` move under
  `runtime/` so `net/` means wire (ADR 0042 d8 taken). The witnessed frame
  order and every retail-wire requirement remain in force there.
- **Updated**: [ADR 0042](0042-godot-permanent-shell-one-mission-kernel.md)
  (2026-08-28) updates decision 2's implementer sentence: the concrete
  targets (`Simulation`, `apps/nw_server`) embed `mission::MissionKernel` and
  call `inmatch::listen_host::frame` (PR #587) — the kernel and the frame are
  the one implementation.
- **Owners**: runtime architecture, in-match networking, gameplay
- **Supersedes**: ADR 0009 decisions 1-2 as a public module topology and ADR
  0035 decision 1's `opennova::inmatch::MissionSession` name/location. The witnessed
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
2. `opennova::inmatch::Session` in `engine/runtime/inmatch/session.*` is the one
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
The concrete `World` also has stable mission-lifetime identity. It is neither
copyable nor movable: `EntityCommands` binds the owning world, the default net
sink points into it, and registered systems retain relationships to it.
Constructors and fixtures therefore build a world directly at its final
address instead of returning one by value. `tests/world/world_test.cpp` pins
that ownership invariant at compile time.

## Consequences

- Retail byte compatibility can be reviewed independently of our object model.
- Gameplay modes share one scoring/outcome model and one round-end network
  transaction, so adding a mode tests the architecture instead of copying it.
- Godot and `nw-server` consume the same lifecycle API while retaining their
  necessary device/socket adapters.
- The source tree still contains `npruntime` and `netsim`; their presence does
  not authorize callers to treat them as stable product APIs.
- Internal API breakage is resolved by updating all callers in one commit.
- A `World` cannot be relocated after construction; mission owners keep it at
  one address for the entire active-session lifetime.

## Verification

- `tests/world/match_test.cpp` pins every retail score schema, primary-score
  selector, automatic win arm, clock/draw rule, exact collision-fed flag
  transition, and immutable result. `tests/world/collision_test.cpp` pins the
  MoveCB/Powerup callback-before-force branch and authority contact stream.
  These branches follow `Entity_MovementCollisionResolver
  @0x4B2F8D..0x4B2FF5`, `Server_CheckWinConditions @0x51AD40`,
  `GameEvent_ProcessScoring @0x52F550`, and
  `GameType_CreateDefaultSettings @0x52DD00`. It also preserves Flag Me's
  retail row-12 defect: objective wire transitions remain live, but the
  12-row scorer/status table rejects its score row.
- `tests/npruntime/round_end_test.cpp` pins multiplayer and both co-op variants
  through the shared live transition: stock Co-op reaches it from a real WAC
  `Win` execution and Objective Co-op from a real BMS `RedWin` event, while
  competitive modes use their gameplay producers. It also pins real
  remote-body objective collisions, exact
  objective-state/event routing, `0x61`/`0x1D`
  delivery, requester-only `0x2B`/`0x56` board pulls, and the 2790-tick linger
  (`WacAction_Win @0x4ED4A0`; `EventAction_Dispatch RedWin @0x454495`;
  `Server_ProcessRoundEnd @0x5164F0`).
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
