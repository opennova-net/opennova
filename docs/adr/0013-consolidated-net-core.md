# ADR 0013 — Consolidated in-match net core: one message registry, one server-state model

Status: accepted

## Context

Three independent servers sit behind an opennova multiplayer session (see
[docs/net/novaworld-net-re.md](../net/novaworld-net-re.md) §6.9 and the corrected
architecture): the **gate** (:7597) points a client at NovaWorld; the **NovaWorld
server** (docker :64206 / :8080) is matchmaking / NAT rendezvous only — its JO
game-session responder exists to answer gate probes and transport-level joins, never to
serve gameplay (2026-07-25: since the LAN-responder leg of decision 3 folded onto the
shared `start_host_session` bring-up, that responder rides the same `HostOwner` machinery
as the real hosts and therefore carries a MINIMAL world — an empty mission plus one
synthesized start marker so the shared spawn pipeline stays well-defined. That world is
service infrastructure, not a parity surface: no mission is loaded, and the service's
role remains matchmaking-only. The recorded caution: a mis-addressed retail client can
now walk admission further into that phantom world than the old World-less responder
allowed — capping the responder's admission depth is fair follow-up work); and the
**game server is the
opennova Godot game itself** — `NovaSimulation` owns a `libs/npruntime` `HostOwner`,
`start_host_session` stands up a real `world::World` plus the selected host role, and
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
   the Godot binding, and the LAN responder.

The hard constraint throughout: **no wire changes.** Every consolidation must keep byte
output identical, proven by the golden harness and the byte-parity ctests.

## Decision

### 1. One message registry + a capture→golden-diff harness (the "identify / validate" seam)

`libs/npwire/ingame_message_catalog.h` is the **single source of truth** tying
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

The witnessed §5.0 host bring-up lives once in
`np::start_host_session(HostOwner&, HostConfig&)`: serve-and-play selects mode 3 and
passes the type-2 loopback into `create_session`; serve-only selects mode 1 and passes
no local client. Both continue through `set_transport_mode` → `create_session`
[→ `Server_InitNewRoundState`] → `configure_session_runtime`; only mode 3 runs the
own-player spawn/initial-stream path
[orig: `SinglePlayer_StartMission @0x561af0`;
`UI_HandleHostSessionStart @0x556d00`]. The Godot binding (`NovaSimulation`) delegates to
the same helper as the CLI harness and host-session tests. `create_session` owns the
single `Server_InitNewRoundState` call.

## Consequences

- **Wire output is unchanged.** Verified after each step by the byte-parity ctests
  (`npruntime_golden_lan_join`, `npruntime_golden_gameplay`, `nw_dvxc1_groundtruth`,
  `nw_ingame_pool_records`, `nw_capture_decoder`) and the coverage gates
  (`nw_message_coverage`, `nw_golden_diff`) staying green, plus the GDExtension building.
- **Adding a message is one path**, not three: registry entry → decoder → round-trip
  test → doc §5.x → `nw_pp` verify, and `--coverage` surfaces what is still missing.
- **Scoped-next (this ADR names them so they are not silently skipped):**
  - **DONE (D-NET-132, 2026-06-30):** merged `NapiGameSettings` + `ServerRules` +
    `SessionReplyConfig` into one `GameConfig` (`libs/npruntime/.../game_config.h`) mirroring the
    `CAdminServer SET` field set (§6.9), collapsing the reimpl's three diverging `g_GameType` copies
    onto the one field IDA proves the 0x08 block AND the 0x7B body read (`ServerConfig_SerializeToPacket
    @0x505bd0` / `NapiNPMsg_0x7B_BuildPayload @0x507740`). And collapsed the reactive-reply roster
    identity onto `link.owned_entity`: team (@entity+344, `CAdminServer_HandleStatus @0x402e30`) + the
    wire handle are read THROUGH the live registry entity, retiring the
    `SessionReplyState.{binding_valid, player_entity_handle, team}` cache; the pre-World reactive path
    (`bind_session_reply_player`) stamps `owned_entity` with the bare wire handle. Wire-neutral on the
    byte-parity goldens.
  - **DONE (2026-06-30):** a shared spawn-batch chunker — `np::slice_batch_pages`
    (`libs/npruntime/.../batch_chunker.h`), the byte-budget world-stream pager factored out of
    `emit_paged_pool` so the §5.2a burst pages every pool through one named, unit-tested component
    [orig: `Server_SendInitialGameStateToPlayer @0x51bba0`].
  - **DONE (2026-06-30):** a shared `SessionSequencing` / `SessionCrypto` retiring the 3× seq/ack + SCRK
    copies — `frame_session_packet` / `deframe_session_packet` (`libs/novaworld/.../protocol_message.h`),
    used by the host S2C (`NapiNPConnection`), joiner C2S (`JoinerConnection`), and lobby C2S
    (`ClientSession`) framing paths (each keeps its own byte-identical outer NWU envelope). Wire-neutral.
  - **DONE (2026-07-25, PR #300):** `nw_udp_listener`'s JO responder folded onto
    `start_host_session` — the third copy-paste site from consolidation problem 3. Cost:
    the shared machinery requires a `ctx.world`, so the service now carries the minimal
    infrastructure world described in Context, with the admission-depth caution recorded
    there.
  - **DONE (2026-07-28, quality campaign W2-2):** the byte-identical outer framers are collapsed —
    `libs/novaworld`'s `encode_session_outbound` / `decode_session_inbound` are deleted and their
    call sites use npwire's `nw_encode_outbound` / `nw_decode_inbound` (it was four functions, both
    directions, not two). And `nw_udp_listener`'s server-direction **framing** leg now calls the
    shared `frame_session_packet` instead of hand-stamping a `ProtocolPacketHeader` beside the raw
    `encode_protocol_packet_plaintext` — the last copy of the stamping `npruntime`'s
    `frame_session_replies` already owned. Wire-neutral, proven before/after against the local
    goldens (472 decoded S2C tags, 2351 C2S `0x0C` / 2361 S2C `0x0A`, the `0x2A` byte-exact records,
    and the `0x81`/`0x82` first-diff offsets all unchanged; `golden_client`'s re-framed-datagram
    byte assertion covers the framing path directly).
  - **CLOSED (2026-07-28, quality campaign W2-3) — REFUTED, not implemented.** The remaining
    item read as "fold `nw_udp_listener`'s per-connection lobby state into the shared
    registry". Doing that would REGRESS the retransmit path, so the item is retired rather
    than done. `LobbyConnState` is not a duplicate of `ConnectionRegistry`: the two are a
    deliberate LIFETIME split. A `ClientHello` re-inserts the peer as Handshaking and clears
    the registry's session keys; the lobby record survives that and RESTORES them when an
    exact `ClientAuth` repeat arrives (`notify_active_addr` with the cached scrk) — the
    D-NET-104 invariant that a repeated `0x42` replays the cached `0x82` and never re-mints
    session material. One merged store cannot express "wire row reset, session material
    kept". Two supporting facts found while checking: eviction is ALREADY single
    (`erase_lobby_state` is the ConnectionManager `on_lost` sink, so there is one teardown
    entry point), and `ConnectionRegistry::find_by_addr` returns `Connection` BY VALUE on the
    per-datagram path, so moving the auth-datagram cache and reassembly buffers into it would
    copy kilobytes per packet. `HostedSnapshot` vs `HostRow` is likewise not duplication —
    `row_from_lobby` is the documented DB projection of the live view. The invariant is now
    recorded at both struct definitions so a later cleanup cannot merge it away.
