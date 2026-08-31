# TODO

Everything here is work that is **not** a parity divergence: ONED and OpenNova Launcher UX, code
hardening, and project health. Divergences from the original engine belong in
[docs/divergence-ledger.md](docs/divergence-ledger.md) instead, and
[docs/current-state.md](docs/current-state.md) explains which is which.

## Cleanup & verification backlog

- [ ] `TerrainFieldStore` publishes a sector layout with no extent.
      `TerrainFieldStore::build` (`engine/runtime/terrain_query/terrain_field_store.cpp:21-63`)
      fills `layout.sector_grid`, `origin_x`, `origin_y` and `locks` but never
      `layout.sector_count` / `layout.sector_rows`, and nothing else in
      `engine/runtime/terrain_query/` assigns them either — they keep the `= 0` defaults
      from `coords.h:41-42`, which is precisely the extent the bounds-reject guard reads
      (`coords_sector_id_at_cell` `coords.h:169-170`, `coords_world_to_cell_source`
      `coords.h:200`). Inert today only because the sim exclusively passes
      `coords_runtime_options()` (`bounds_reject = false`, so the grid index wraps `& 0xF`
      instead); the first sim-side consumer to adopt the bounds-checked path would find
      EVERY cell rejected, with terrain silently reading as absent. Set both from the
      `.trn` (`sector_count` is the grid WIDTH, `trn.h:36-40`) when building the store,
      before anything depends on them. Found 2026-08-30 while clearing the minimal set's
      movement failure — not the cause of it, but on the same path.

- [ ] The player's own clip set is hostage to the DEFAULT infantry `.adm`. Infantry
      locomotion is entirely root-motion driven, and `MissionKernel::install_infantry_anim`
      (`engine/runtime/mission/mission_kernel.cpp:285-301`) resolves per-entity `anim_def`
      rows only when the default set registered as id 0 (`:299`), while
      `resolve_new_infantry_adm_ids` itself short-circuits on `root_motion.empty()`
      (`:253`). So when `kDefaultInfantryAdm` (`engine/runtime/mission/runtime_boot.h:35`,
      `"E_STAND.adm"`) is absent from the mounted root, `ai.root_motion` is null (`:298`)
      and the LOCAL PLAYER never picks up its own `items.def` `anim_def` even though the
      model, the `.adm` and every `.bad` are present — it spawns, renders and plays
      `anim_idle` (presentation loads the `.adm` down a separate path,
      `godot/src/mission/mission_object_placer.cpp:1213`) but cannot walk. Cost a session
      to diagnose on the minimal set (2026-08-30); staging `E_STAND.adm` fixed it. A game
      whose only body is the player should not need an AI body's clip map to move: decouple
      the per-entity resolve from the default set, and give `resolve_new_infantry_adm_ids`
      a path to publish `ai.root_motion` (today only `install_infantry_anim` sets it).
      The one useful diagnostic already exists — `root_dx`/`root_dy` vs `res_dx`/`res_dy`
      on the entity card (`engine/runtime/world/inspect.cpp:181-184`); note `cmd_fwd` is
      `veh.cmd_speed` (`inspect.cpp:160`) and is structurally always 0 for infantry, so it
      is not an instrument for this.

- [ ] Retail-LAN parity four-topology verdict: the tracked 24-cell matrix harness
      (`run_parity_matrix.ps1`/`generate_parity_manifest.ps1`/`verify_parity_matrix.ps1`
      over `export_parity_corpus.py`) was retired with the Python FFI (ADR 0038; last
      at 5820432c1), so no PARITY_STATUS verdict exists. To produce one: run the four
      `scripts/net/run_parity_topology.ps1` cells (RR/RO/OR/OO) per case under one
      RunId prefix (`.agents/retail-lan-parity.md`), gate each on the wire-ready witness,
      and re-express the retired verifier's RO/OR/OO-vs-RR
      comparison (both directions, packet grouping, 0x0A/0x0C state) as a ctest or
      PowerShell verifier. Upstream blockers still open in opennova-int (the runner stops the
      `deploy_hold` RR/RO cells and the OR retail joiner with a named UPSTREAM BLOCKER
      error until they land): the single-role `onhook_host_lan`/`onhook_join_lan` MCP
      tools pass role config via child env and never render `onhook.cfg` (make
      `LaunchLanRole` render it like the `onhook_run_lan_pair` half), and they must
      return `pid`/`instance_id`/`run_id`/`capture_path` like `onhook_run_lan_pair`;
      onhook-mcp advertises protocol version `2026-07-28`, which Claude Code rejects
      (negotiate `2025-06-18`/`2025-03-26`); optionally an `onhook_exercise_input`
      tool would retire `exercise_retail_input.ps1`. Baseline: the 2026-08-05 suites captured 24/24 cells
      cleanly, wire-ready + `diff_vs_golden` GREEN.
- [ ] Terrain native `[orig]` citation pass: sweep the remaining uncited chains —
      every file under `engine/runtime/terrain` now carries an anchor, and the cpt/til/trn
      resource formats moved to `engine/formats/` carrying theirs, so the gap is
      `godot/src/terrain/`, where the foliage def/map pair and the tile cache device/entry files carry
      none, `terrain_tile_info.cpp` carries only its foliage-block anchor and the
      static-shadow rasterizer only its collector/tile-walk anchors; `docs/terrain/terrain-re.md`
      is still partial (PAR-R1); narrow or close this entry after the sweep
- [ ] Present-pass / entity-reconcile citation pass: the present anchors live at
      the native walks (`present_applier.{h,cpp}` 10, the wire walk +
      cold path `godot/src/simulation/present_applier_wire.cpp`, the
      held-weapon reference math `engine/net/npruntime/client_replica_present.h`) —
      but the native `EntityIndex` (`godot/src/object/entity_index.cpp` —
      the registry's successor) still carries none. Remaining:
      engine-research the original entity-reconcile chain and cite it into
      `docs/runtime-architecture.md` + `docs/correspondence.md`
- [ ] Env/water/sky/weather singleton `_process` set: re-measure at the ASH_I5A
      vantage before slicing — the #403 present-side rework (parked idle models,
      `env_generation_changed`, the staggered per-model light restamp) invalidated
      the earlier ~1 ms reading. The F3 "Outside shell spans" row went with ADR 0039's
      hard cut; the metric survives as the `outside_shell` summary
      the `perf_mission_rows` runtime probe reports, and that is where the next
      attribution round reads it (the #403 live sessions observed the remaining frame
      cost concentrated outside the model system after the park/submission gates)
- [ ] Main-loop order grill: `docs/runtime-architecture.md` cites the exact main-loop /
      entity-render order from existing RE notes; a focused grill-ida pass to pin
      `WacScript_AdvanceTick`'s surroundings + the original entity-render function would
      witness it directly
- [ ] `opennova::io` adoption continuation: migrate remaining per-lib byte readers on-touch (policy in engine/CLAUDE.md); excluded: mus/wac VM cursors (faithful-port surface) and cpt (a real migration, tracked as its own row below)
- [ ] NovaWorld disconnect-state reset (owner: `godot/game/novaworld_panel.gd`): clear all connection-derived rows, login/join state, pending mission/player data, and disable Host/Join/Login on disconnect or error. Acceptance (`godot/tests/novaworld_panel_test.gd`): a populated, logged-in, pending-join panel returns to a clean disconnected state and cannot submit a stale row. Coordinate with the unlanded novaworld_panel rework held in the `gsb` worktree (WIP commit 0fd58850b on `worktree-gsb`; the branch's earlier commits landed via #300) before landing.
- [ ] Converge `engine/formats/cpt`'s bit codec on `engine/base/io/bit_stream.h` (owner: `engine/formats/cpt/cpt_io.cpp`): the two have diverged (cpt's writer carries a normalizing `set_position` and a `write_to_file`; its reader now carries `remaining_bits`), so this is a real migration, not a swap — the reason it is tracked separately in `engine/CLAUDE.md`. Acceptance: `tests/cpt/cpt_roundtrip_test` (ctest `cpt_roundtrip`) stays green AND a by-hand retail-corpus byte diff still reports byte-identical CPT output after cpt drops its private copy; the corpus diff is not in ctest, so it has to be run by hand (`engine/CLAUDE.md` § migration exceptions).
- [ ] Vehicle-drive slice start (retail-join-0a): the `game-server` worktree holds WIP commit a6bf98a30 on `worktree-game-server` — VehicleTraits `ground_family`/`is_eweap` groundwork (6 files; based pre-#403, snapshot-committed 2026-08-04). Reconcile onto current master when the local vehicle-drive slice runs (#403's `VehicleTraits` since gained the items.def-derived family tag + air/water params, so this is a rebase-and-rethink, not an apply).
- [ ] Dev-tools windows (ADR 0039; `engine/runtime/devtools/README.md` is the
      recipe): the retired F3 pages return as engine ImGui windows as they are
      wanted (the Entities window landed as the ADR 0042 d6 template, PR #587;
      the Weapon window landed as the dope-sheet ACTION editor over the equipped
      weapon's FSM, which also covers the FP-weapon half of "animation");
      still open: sim transport
      (play/pause/step; the MCP
      `game_debug` control plane still drives these), script vars, net, particles,
      occlusion, rounds, terrain, rendering/world-view toggles (`GameWorld`'s
      typed API), audio, animation, player (with the pose dump the
      retired `DebugSnapshotWriter` produced for `pose_replay_probe` /
      `terrain_seam_probe`), and the `PerfTimeline` ring. Data the shell alone
      has crosses as VALUE slots or typed records, never Godot objects.
- [ ] Stats window info cells not carried over from the retired page (they read
      Godot objects at refresh): Performance draws/objs/prims/nodes on the Render
      row, the a11y flag on Flush tail, effects live count,
      fire/destruction/throwable/wire present stats, occlusion counts (`occl`).
      Each returns as a VALUE slot fed by the shell sampler that owns the source.
- [ ] Managed-game shutdown: ONED close/Stop may still require forced termination.
      Add a bounded graceful-quit window before the current forced termination,
      and keep the process-handle lifecycle reliable so a stopped retail child
      releases the staged files before the next pack.

## Project health follow-ups

- [ ] NovaWorld production deploy + cutover (operator-executed, one-time): the
      stack (gate + NovaWorld server + legacy HTTP services + web portal) has
      never been deployed to production — steps in [DEPLOY.md](DEPLOY.md), the
      operator sequence in `plan/pr21-cutover-runbook.md` (its "#136 open"
      premise is historical; commands remain current)
- [ ] Release-gate parity: make tag releases run the same required quality gates as PR/master CI, or reject release tags whose commit is not on `master`. Acceptance: an off-master tag cannot publish, and a valid release commit passes the shared maturity, native, and Godot gates.
- [ ] Full Linux core tests on PRs: `test-linux` (`.github/workflows/ci.yml`) already runs the whole ctest suite on ubuntu for master pushes and manual runs; extend it to pull requests once its cost is acceptable, after triaging any platform-only failures. Acceptance: the full CTest suite runs on Linux for every PR without relying on the net-only or packaging jobs.
- [ ] Incremental conventional linting (vocabulary conventions already ride `scripts/lint/conventions_lint.py` as a CI gate; this row is formatting + per-language linters): establish project-owned formatting settings, then add per-language lint checks in advisory or changed-file mode before enforcing them. Acceptance: CI checks new changes without requiring a repository-wide reformat, with documented local commands for each enabled linter.
- [ ] Serve mode (PROD-1, ADR 0015): `opennova.exe --server` / `--headless
      --server` and a packaging boot-smoke leg — tracked future work, never
      implemented; specs live in `docs/maturity-program.md` §PROD.
- [ ] NovaWorld session-builder residue (PAR-NET): the 0x81/0x82 builders
      (`build_server_hello` / `build_server_auth`, `engine/net/npwire/session_hello.h` +
      `engine/net/npwire/session/session_hello.cpp`) were grilled and fixed 2026-06-27
      (ROADMAP Wave 3); what remains is the host-specific seed values the golden byte-diff
      still shows (the 0x81 `CI` host-node index, the 0x82 `MI` host dcb, the identity
      strings) and the ~8 §5.2a initial-state serializers `log_deferred_once` skips
      (`engine/net/npruntime/server_initial_state.cpp`, "emitted as nothing pending the
      grill wave"), which stay unemitted-and-logged rather than faked. Witness each at the
      addresses cited there -> `ingame_encode`, land the record via `re-doc`. Detail:
      `engine/net/npruntime/ROADMAP.md`.
