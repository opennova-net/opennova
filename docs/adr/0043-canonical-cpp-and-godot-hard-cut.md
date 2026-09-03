# ADR 0043: canonical C++ and canonical Godot — one session with roles, the world is C++ Nodes, tests drive real fixtures

- **Status**: accepted (2026-09-02; maintainer directive — the hard-cut architecture refactor)
- **Owners**: runtime architecture, the Godot layer, tooling, CI governance
- **Supersedes/updates**: this is THE ONE current-architecture record. It supersedes
  [ADR 0031](0031-adapter-composition-contract.md), [ADR 0032](0032-direct-document-io.md),
  [ADR 0033](0033-engine-owned-loops-device-shells.md),
  [ADR 0035](0035-mission-session-game-frame-pipeline.md) and
  [ADR 0036](0036-one-inmatch-session-wire-first.md) in full (their still-true rules are
  restated below; the files stay as the record of their rounds) and amends
  [ADR 0042](0042-godot-permanent-shell-one-mission-kernel.md) (d2, d3, d5, d7; takes d8),
  [ADR 0034](0034-first-class-godot.md) (d6's rewrite queue is executed; the
  "overridable names invoked as `_world.<name>()`" clause is deleted),
  [ADR 0018](0018-public-api-testability.md) (a test never subclasses a production Node),
  [ADR 0020](0020-world-terrain-query-seam.md) (the `runtime/mission` whitelist),
  [ADR 0029](0029-engine-group-targets.md) (the chain order). ADR 0017 (typed records),
  ADR 0022 (the ledger), ADR 0028 d1 (four groups), ADR 0037, ADR 0039, ADR 0040 and
  ADR 0041 stand unchanged.

## Context

The 2026-09-02 architecture audit (three audits and two design passes, every
load-bearing claim re-verified by reading the cited file) found the codebase
worst where its own governance had shaped it:

- The Godot world was one object split into five "annex" files that reach into
  each other's privates (632 foreign `._member` accesses in shipping GDScript),
  split to satisfy a 1200-line ratchet and typed `Object`/`Variant` at its
  seams so GUT subclass-doubles could bind (80 double classes, 15 of them
  `extends GameWorld`). The frame was a 22-leg scheduler in GDScript with
  three timing systems and the order written twice.
- `Simulation` was a 322-method `Node3D` never placed in the tree, assembled
  from two include-a-class-body fragments, switching between three engine
  tick paths per role.
- `World`, `Entity` and `MissionKernel` were C structs with `class` written on
  them (engine-wide 1152 `struct` vs 118 `class`; 100 header free functions
  taking `World &` first); eight `*Perf` structs put 124 null-checked branches
  through the hot path; one tick crossed five layers
  (`Session -> TickTarget -> listen_host::frame -> host_session_pump ->
  Server_TickUpdate -> run_logic_tick`) with three parallel frame
  implementations.
- Every record crossed the Godot boundary three times (engine struct, a
  mirrored-field RefCounted, GDScript); two entity present walks shared two
  mutable Dictionaries by reference across the language boundary.
- 42 ADRs, of which six described the one current architecture; 12 lint
  scripts, several of which mechanically vetoed the refactor that fixes the
  above (the two citation gauges, the GDScript size ratchet, `host_lint`).

The maintainer's directive: a hard cut toward canonical C++ and canonical
Godot with proper OOP design, structural only — no behavior change, every
`[orig:]` citation carried with the code it cites, retail wire/format parity
untouched — landed as ONE pull request of committed-as-we-go slices.

## Decision

1. **One record.** This ADR is the current architecture. The superseded ADRs
   are historical; `docs/runtime-architecture.md` describes the live path and
   is rewritten by the slice that changes it, never ahead of it.

2. **The engine object model (`runtime/world`, `runtime/mission`).**
   `Entity` stays a POD row owned by `EntityRegistry`; behavior moves onto the
   SYSTEM that owns the state it mutates, never onto `Entity` (retail's
   `Entity_*` functions mutate several rows at once). `Entity` gains only pure
   predicates. `World` is the one aggregate whose invariants are
   ORDER (`run_logic_tick`) and IDENTITY (non-copyable); its public members
   regroup by lifetime (`script`, `tables`, `rules`, `out`; the sim members
   stay flat). `AiSystem` is owned BY VALUE on `World`; its per-slot
   handle-to-index table STAYS (slice E4, 2026-09-02): a per-row
   `Entity::ai_index` would change which brain a reused pool slot resolves
   to, and retail's entity+100 init on respawn is unwitnessed, so that move
   waits for a grill. `EntityCommands` stays the one script/
   tool command layer. New systems own what free functions used to reach
   through `World &`: `VehicleSystem`, `ZoneSystem`, `LocalPlayer`. The
   feed builders stay free functions under `world/feeds/`. `MissionKernel` is
   boot + state, owns no tick, inherits no provider interface; one
   `IPoseProvider` with one `simassets::SimPoseProvider` replaces the three
   provider interfaces; the dead seams go outright (`INetCommandSink` /
   `LocalSink` / `SerializingSink`, an unwitnessed 0x23 payload with no
   caller; `mission_systems.h`; the kernel's registry and command
   forwarders). `runtime/world` gains subdirectories per subsystem as
   slices touch them; leg-split TUs fold back into their owner.
   `AiEntity`'s 16.16 pose mirror STAYS: unifying it with the float
   `Entity::position` is behavior-bearing and is a ledgered follow-up.

3. **One session with roles.** `inmatch::Session` (banking, state machine,
   input retention unchanged) owns a `Role` strategy —
   `LocalRole`, `HostRole` (listen and dedicated), `JoinerRole` — and the one
   non-virtual `run_one_tick`: the shared input prologue through the role (the
   spectator gate and the medic send are the role's facts), then the role's
   `run_tick`, each role's current leg order line for line, then the shell's
   `TickObserver`. `TickTarget`, `listen_host::frame`,
   `MissionKernel::tick_no_net`, `JoinerWorldBridge` and its `PumpHooks` die
   (E8a landed the roles and the frames; E8b folded the bridge into
   `JoinerRole` and split its nineteen shell hooks). The hook-split rule as
   built: a leg the sim reads before the frame ends is engine-side and
   synchronous (the socket seam the shell installs as an `IDatagramSocket`
   plus the host address, the decoded-row asset resolution through the
   kernel's own verbs — `adm_id_for_runtime_type`,
   `wire_collision_shape_for_type`, `retire_replica_entity`,
   `resweep_item_traits`, `refresh_collision_instances`,
   `occlusion_init_mission` — the local-player pumps, the charattr words);
   a leg that only feeds the shell is an observer read after the tick (the
   role's one-shot diagnostic sample, its world-sync serial the shell's
   registry-derived caches re-derive from). Only the loadout profile seams
   stay shell-installed functions (`JoinerRole::KitSeams`: the weapon.sav
   page composition and the respawn rebuild). (Amends ADR 0042 d3: the
   kernel owns no tick; the "no runtime class" clause is superseded by this
   consolidation, which is not the mirror facade ADR 0036 d4 rejected.)

4. **`net/` means wire.** `net/` keeps `novacrypto`, `napi`, `npwire`,
   `novaworld`; `netsim` becomes `runtime/replication/`, `npruntime` +
   `inmatch` + the transports become `runtime/inmatch/` (ONE namespace,
   `inmatch`, absorbing the former `np::`; `session` was rejected as a
   namespace name because hundreds of locals are named `session` and would
   shadow it). The two wire-only survivors stay in `net/npwire` in the plain
   `opennova` namespace: the LAN discovery codec and the datagram-socket
   seam. The chain flips to `io -> crt -> formats -> base -> net -> runtime`;
   `opennova_net` never links `opennova_runtime`; the protocol tests,
   `nw_lan_probe` and `apps/common` link `opennova_net` only (linker-enforced,
   `link_graph_check.py`; `nw_pp` links the runtime for the HUD feed-kind
   table it prints, and the NovaWorld server for the in-match host session
   it hosts JO peers through); the NET-AGNOSTIC tree rule keeps
   `runtime/{world,wac,mission,terrain,terrain_query,simassets,anim,audio,
   particle,renderer,hud,menu,environment,controls,devtools}` free of `net/`,
   `runtime/inmatch/` and `runtime/replication/` includes
   (`include_graph_check.py`). The one `net -> runtime` include today
   (`npwire/game_type.h`) becomes `base/gameprofile/game_type.h`. Per-peer:
   `PeerLink` and the three parallel maps fold into `NapiNPConnection`. The
   1065-line `on_server_session` becomes a `switch` over private
   `on_s2c_<name>` methods split by message family (retail's own dispatch is a
   table). (Takes ADR 0042 d8.)

5. **One profile collector.** `io::TickProfile` + RAII `ProfileScope` (the
   FrameStats slot enum unchanged) replace `LogicTickPerf`, `ServerTickPerf`,
   `AiTickPerf`, `HostSessionPerf`, `ClientFramePerf`, `JoinerPumpPerf`,
   `ConnectionS2CPerf` and the collision perf structs; every `*Perf *`
   parameter leaves the tick signatures. `inmatch::FramePerf` stays as the
   outcome value.

6. **Entity representations.** Survive, each a witnessed boundary:
   `bms::Entity`, `world::Entity`, `world::AiEntity`, `ClientEntityState`, the
   npwire decode records, `GameEntitySnapshot`, `inspect::EntityRow/Card`.
   Die: `mission::EntityRecord`/`EntityProperties`, `StreamedPlacementRecord`
   (E10: the shell reads the materializer's placed registry rows). The
   identity pair on `MaterializedRow` (`spawn_origin`, `bms_id`) STAYS (E10
   decision): it is the placed identity's tombstone — it outlives the
   registry row so a vanished stamped slot still retires its id to the shell
   and a same-type re-spawn inherits it after the previous lifetime is gone;
   deriving it from the row alone changes those two edges. One joiner path:
   wire -> decode record -> `ClientEntityState` -> `world::Entity`.

7. **The terrain seam stays; the kernel loads its own terrain.**
   `terrain_query/terrain_field_build.h` is whitelisted for
   `engine/runtime/mission` only, so `MissionKernel::boot` fills its own
   `terrain_store` through its asset index when the embedder built none
   (`KernelBootOptions::terrain`; E9: the dedicated host and the retail rig
   stopped pre-building). The shell keeps the parsed-document entry (its
   `TerrainData` through `terrain_field_store_build` before the boot, with
   `terrain = false`): one builder, two entries, both through
   `height_field_apply_trn` (amends ADR 0020 d3).

8. **`formats/mission` is `bms::File` plus free functions.** `MissionDocument`
   (the 90-method pimpl with string-keyed setters) retires; its capabilities
   become `bms.h` (parse/write/header-only/default), `mis.h`, `bms_edit.h`,
   `bms_names.h`. `formats/wac` vs `runtime/wac` and `formats/particle` vs
   `runtime/particle` stay (file format vs execution).

9. **The Godot world is C++ Nodes with real ownership.** Everything under the
   world node is C++ under `godot/src`: `GameWorld` (the load plan, the frame
   driver, `OcclusionFrame`, `SessionDrive`), `MissionRoot` (the per-mission
   subtree; owns `Ref<Simulation>` and implements the C++ `TickSink`),
   `EntityPresenter` (ONE present walk with a `PresentRow` kind discriminant;
   fire/destruction/throwable presenters as members; `ScarPresenter` absorbs
   its pass), `EffectWorld`, `MissionAudio`, `LocalPlayerPresenter`. The shell
   stays GDScript: `MainGame`'s state machine (its menu front-end folded back
   in; typed getters replace `GameShellSeams`), the screen presenters behind
   a `ShellScreen` base, the compiled-menu companions, the loading screen,
   the MCP transport, the probes, the fly camera, the runtime root.
   `Simulation` becomes `RefCounted` (it was never in the tree) and stays the
   session/kernel embedder; its role switch collapses onto decision 3's
   `Role`. The witnessed frame order lives in ONE static leg table in C++
   with the `[orig:]` comments on the leg bodies, run by one loop with one
   RAII `LegScope` timing (the three timing mechanisms die); the frozen-pose
   capture replay is a second table through the same loop. The env nodes lose
   their `_process` forwards. Visibility is two bits on `ObjectModel`; the
   shared Dictionaries die. The nine Callable slots become direct typed
   references, one signal (`wire_node_spawned`) and the `TickSink`. The
   present-row enrichment in the binding moves to
   `runtime/replication/client_replica_present_projection`. (Amends ADR 0042
   d2: there is no sanctioned untyped seam; the pipeline is C++.)

10. **Carriers: two mechanisms.** A record crosses to GDScript as a value
    wrapper over the engine struct (`hud/feed_row.h`: `engine::X value_` plus
    forwarding accessors; the X-macro may generate forwarders, never mirrored
    members) or, when per-frame and plural, as a `Packed*Array` with an
    engine-owned layout or ONE frame object holding native vectors. Per-frame
    drains whose consumers are C++ are unbound `const std::vector<T> &`
    reads. Test-only ClassDB classes leave ClassDB. Debug overlays are drawn in
    C++ from engine snapshots (the ImGui windows are the F3 surface).

11. **Tests drive real fixtures.** A test never subclasses a production Node
    to override behavior; it boots a real fixture through public load seams
    (`godot/tests/support/world_fixture.gd`) or fakes a GDScript interface
    (`ShellScreen`). Overriding a PUBLIC verb of a GDScript shell class stays
    acceptable; overriding a private never is. The sanctioned `_world: Node`
    double seam is deleted. (Amends ADR 0018 and ADR 0034.)

12. **Tooling.** The MCP transport stays GDScript (ADR 0034 d6) but leaves the
    release export and loads by path under `--mcp-port`; the debug-control
    table is C++ (`godot/src/devtools/debug_control_table`, typed owners,
    `DebugInvokeResult`), shared by F3 and MCP (amends ADR 0042 d5); the
    probe object model folds from eight types to four.

13. **Governance.** `godot_orig_cites` is ONE non-increasing count over
    `godot/src` + the game-level GDScript (a cite moves freely between the
    two Godot-side languages; the count falls only when code reaches
    `engine/` or dies; amends ADR 0042 d7). `gd_foreign_private_accesses`
    (baseline 632, target 0) replaces `oversize_gd_files`: a script that
    reaches into another object's privates is a method annex, not a class.
    The 2500-line C++ ratchet keeps its number with the rule "split by
    responsibility, never by leg; a TU under ~100 lines with one owner folds
    back". `scripts/lint/cite_census.py` keeps the SET of witness addresses
    from losing a member silently (it absorbed `host_lint.py --frozen-audit`
    as `--audit-range`). `tests/mission/tick_digest_test.cpp` +
    `scripts/parity/tick_digest.py` pin the per-tick state chain a structural
    slice must leave unchanged (the synthetic chain is committed; retail
    chains are compared on one machine). `host_lint.py` and the em-dash /
    checked-box prose rules are retired: vocabulary is a review concern. A
    `maturity_lint.py` dict-contract allowlist row dies with its file.

14. **Style, last.** Per-lib C -> C++ (`.c` -> `.cpp`, no `extern "C"`,
    `namespace opennova::<lib>`, the `DEF_*` macros as `inline constexpr` of
    the same names), `#pragma once` everywhere, a `.clang-format` and one
    whitespace-only commit per group with `.git-blame-ignore-revs`, after
    every structural slice has landed.

15. **Out of scope, named.** The pose-store unification (decision 2), and the
    ADR 0042 outstanding retail-interop recipe, which these slices neither
    change nor re-prove.

## Consequences

- One PR, many commits: each slice builds, passes the ctests and GUT files
  it touches, the lints, and attaches its proof (the tick digest, the wire
  goldens, the screenshot fixtures, the LAN pair) in its commit message; the
  full ctest + GUT + lint bar runs at every slice boundary.
- The engine keeps every property the audit found good: `base/io`, the
  globals discipline, zero string-keyed dispatch, `EntityRegistry`, the
  State -> Compiler -> DrawList render shape, the five-target DAG, the
  citation discipline, black-box tests.
- The Godot layer keeps `render/`, the thin network pumps, `HudOverlay` and
  `MenuFrame` drawing engine draw lists, `ResourceRoot` and its principled
  `ResourceLoader` bypass, `ImGuiPassNode`, the three autoloads, the typed
  records that already work.
- CONTEXT.md loses the one-member suffix "families" as the code that carried
  them goes; `Presenter` and `Present pass` survive as the two that name real
  types.

## Verification

- Every slice: `scripts/build.sh`, `scripts/test_godot.sh`, every
  `scripts/lint/*.py --enforce`, `ratchet_counts.py` and `cite_census.py`
  against their baselines. Retail-gated tests report Skipped in CI; a slice's
  commit carries the local gated run.
- Engine slices: the tick digest (synthetic, committed; 00TRg/00TRa/04TR
  locally before and after), the wire goldens (`initial_state_burst`,
  `client_runtime`, `two_peer_fanout`, `round_end`, the capture-driven LAN
  tests), and after the net move the linker itself.
- Godot slices: the render fixtures (`render_fixture_capture` over
  `docs/render/render-fixtures-v1.json`, PNGs byte-identical, diagnostics
  key-identical), the frame-order GUT pin over the bound leg names, the
  `frame_stats` / `runtime_root_window` / `window_fullscreen` probes, the LAN
  pair with `game_entities` diffed key-identical, the ESC pause cycle.
