# ADR 0013 — Consolidated in-match net core: one message registry, one server-state model

Status: accepted

## Context

Three independent servers sit behind an opennova multiplayer session (see
[docs/net/novaworld-net-re.md](../net/novaworld-net-re.md) §6.9 and the corrected
architecture): the **gate** (:7597) points a client at NovaWorld; the **NovaWorld
server** (docker :64206 / :8080) is matchmaking / NAT rendezvous only — its World-less
responder is correct by design and never hosts a match; and the **game server is the
opennova Godot game itself** — `NovaSimulation` owns a `libs/npruntime` `HostOwner`,
`start_host_session` stands up a real `world::World` + a mode-3 listen server, and
`host_session_pump` drives the 62 Hz owner loop (ADRs [0009](0009-in-match-net-seam.md)–
[0012](0012-player-is-host-side-server-entity.md)). Aligning that game server with the
witnessed original so a retail JO client joins and plays fully is the ongoing work.

Before that alignment could proceed, the net code had drifted into three
consolidation problems:

1. **Message knowledge was split across three unsynced places** — docs prose (§4/§5.x),
   a code catalog (`ingame_message_catalog.h`), and the decoders — with no tool to diff
   *our* emitted stream against a golden retail capture.
2. **Server state was fragmented and partly reconstructed at emit time** — the wire
   handle was rebuilt from `pool<<12 | slot` each frame instead of carried off the
   registry; per-connection state duplicated fields the authoritative pool-0 entity
   already owns; a client-side load gate was modeled as host state.
3. **The host bring-up / config / sequencing was copy-pasted** across the CLI server,
   the Godot host, and the LAN responder.

The hard constraint throughout: **no wire changes.** Every consolidation must keep byte
output identical, proven by the golden harness and the byte-parity ctests.

## Decision

### 1. One message registry + a capture→golden-diff harness (the "identify / validate" seam)

`libs/novaworld/ingame_message_catalog.h` is the **single source of truth** tying
`(dir, tag) → name → coverage class → decoder → doc §`, shared by `nw_pp`'s labels and
the `nw_message_coverage` CI gate so a tag cannot be handled in one place and forgotten
in the other. `nw_pp --histogram` emits a stable per-`(dir,tag)` machine-readable line;
`nw_pp --coverage` ranks the undecoded backlog by wire volume.

Capturing traffic and validating it against the goldens in `.scratch/golden/` is a
one-command loop: `scripts/net/diff_vs_golden.ps1` classifies every `(dir,tag)` as
OK / GAP / SPURIOUS + counts decode failures, and the env-gated `nw_golden_diff` ctest
pins the result in CI (skips clean when unset). GAP rows are the prioritized worklist for
aligning the server; SPURIOUS rows are wire-illegal regressions.

### 2. The authoritative server-state model: `EntityRegistry` is the one source of truth

The live `world::EntityRegistry` (5 pools, handle = `pool<<12 | slot`) is the single
authority for entity identity/pose/team; the net layer reads *through* it rather than
caching parallel copies. Consequences:

- **Carry the wire handle, don't rebuild it.** `snapshot_of` copies
  `world::Entity::handle.packed` into `GameEntitySnapshot::wire_handle`, and the per-frame
  S2C `0x0A` builder emits that value instead of re-deriving `pool<<12 | slot` at emit
  time. The value is equal by construction, so the bytes are unchanged.
- **One named pre-spawn fallback.** The joiner's pre-spawn C2S `0x0C` pose is consolidated
  from loose `SessionReplyState.client_*` fields into a `PreSpawnJoinerPose`, valid only
  until the spawn pipeline binds `owned_entity`; `pose_for_conn` then prefers the live
  registry entity.
- **Client gates are not host state.** The write-only `loading_progress` (the client's
  `dword_A82370` load-progress walk) is removed from `NapiNPServerCtx`; the host's
  spawn/load clock is the per-connection `InitialStateBurst` cursor (`conn.burst`).

**Target end-state (D-NET-132, deferred):** per §6.9 the original derives
team/class/slot/kills/deaths from the pool-0 entity itself, so the per-connection reply
identity (`binding_valid` / `player_entity_handle` / `team`) collapses onto
`link.owned_entity` as the single binding, read through the entity. That collapse is
deferred to the server-state pass because it must first resolve the reactive-reply path
that can set a roster identity *before* a World binds (the session-responder / test
path); `player_slot` (roster order, not stored on the entity) stays a field regardless.

### 3. One host bring-up, folded to the shared helper

The witnessed §5.0 listen-host bring-up (`set_connection_mode(3)` →
`set_transport_mode` → `create_session(&loopback)` [→ `Server_InitNewRoundState`] →
`configure_session_runtime`, then the serve-and-play own-player spawn + in-match latch)
lives once in `np::start_host_session(HostOwner&, HostConfig&)`
[orig: `SinglePlayer_StartMission @ 0x561af0`]. The Godot host (`NovaSimulation`) now
delegates to it instead of hand-composing the primitives, matching the CLI server and the
host-session tests. `create_session` owns the single `Server_InitNewRoundState` call.

## Consequences

- **Wire output is unchanged.** Verified after each step by the byte-parity ctests
  (`npruntime_golden_lan_join`, `npruntime_golden_gameplay`, `nw_dvxc1_groundtruth`,
  `nw_ingame_pool_records`, `nw_capture_decoder`) and the coverage gates
  (`nw_message_coverage`, `nw_golden_diff`) staying green, plus the GDExtension building.
- **Adding a message is one path**, not three: registry entry → decoder → round-trip
  test → doc §5.x → `nw_pp` verify, and `--coverage` surfaces what is still missing.
- **Scoped-next (this ADR names them so they are not silently skipped):** the
  `binding → owned_entity` collapse above; merging `NapiGameSettings` + `ServerRules` +
  `SessionReplyConfig` into one `GameConfig` mirroring the `CAdminServer` `SET` field set
  (§6.9); a shared `SessionSequencing` / `SessionCrypto` retiring the 3× seq/ack + SCRK
  copies; and a shared spawn-batch chunker. Each is wire-neutral and belongs to the
  server-state pass, driven by the golden-diff worklist.
