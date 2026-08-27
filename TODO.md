# TODO

Everything here is work that is **not** a parity divergence: ONED and OpenNova Launcher UX, code
hardening, and project health. Divergences from the original engine belong in
[docs/divergence-ledger.md](docs/divergence-ledger.md) instead, and
[docs/current-state.md](docs/current-state.md) explains which is which.

## Cleanup & verification backlog

- [ ] Retail-LAN parity four-topology verdict: the tracked 24-cell matrix harness
      (`run_parity_matrix.ps1`/`generate_parity_manifest.ps1`/`verify_parity_matrix.ps1`
      over `export_parity_corpus.py`) was retired with the Python FFI (ADR 0038; last
      at 5820432c1), so no PARITY_STATUS verdict exists. To produce one: run the four
      `scripts/net/run_parity_topology.ps1` cells (RR/RO/OR/OO) per case under one
      RunId prefix (`.agents/retail-lan-parity.md`), gate each on the wire-ready witness
      + `diff_vs_golden.ps1`, and re-express the retired verifier's RO/OR/OO-vs-RR
      comparison (both directions, packet grouping, 0x0A/0x0C state) as a ctest or
      PowerShell verifier. Upstream blocker still open in opennova-int: the single-role
      `onhook_host_lan`/`onhook_join_lan` MCP tools pass role config via child env and
      never render `onhook.cfg` (make `LaunchLanRole` render it like the
      `onhook_run_lan_pair` half). Baseline: the 2026-08-05 suites captured 24/24 cells
      cleanly, wire-ready + `diff_vs_golden` GREEN.
- [ ] Terrain native `[orig]` citation pass: sweep the remaining uncited chains —
      `engine/runtime/terrain` carries 23 anchors across 15 files and `godot/src/terrain/` 18
      across 8 of its ~16 source files (the cpt/til/trn resource-format files and the
      remaining builder files carry none); `docs/terrain/terrain-re.md` is still partial
      (PAR-R1); narrow or close this entry after the sweep
- [ ] Present-pass / entity-reconcile citation pass: the present anchors live at
      the native walks (`present_applier.{h,cpp}` 12, the wire walk +
      cold path `godot/src/simulation/present_applier_wire.cpp`, the
      held-weapon reference math `engine/net/npruntime/client_replica_present.h`) —
      but the native `EntityIndex` (`godot/src/object/entity_index.cpp` —
      the registry's successor) still carries none. Remaining:
      engine-research the original entity-reconcile chain and cite it into
      `docs/runtime-architecture.md` + `docs/correspondence.md`
- [ ] Env/water/sky/weather singleton `_process` set: re-measure at the ASH_I5A
      vantage before slicing — the #403 present-side rework (parked idle models,
      `env_generation_changed`, the staggered per-model light restamp) invalidated
      the earlier ~1 ms reading. The next perf attribution round starts from the F3
      "Outside shell spans" row (the #403 live sessions observed the remaining frame
      cost concentrated outside the model system after the park/submission gates)
- [ ] Main-loop order grill: `docs/runtime-architecture.md` cites the exact main-loop /
      entity-render order from existing RE notes; a focused grill-ida pass to pin
      `WacScript_AdvanceTick`'s surroundings + the original entity-render function would
      witness it directly
- [ ] Incremental terrain-build tile load: `engine/runtime/terrain/build_quadtree.cpp` skips the
      witnessed cached-tile load leg (`process_quadtree_leaf` / `sub_402730`) and always
      regenerates from base meshes — correct on a clean build, but retail treats existing
      tile files as a cache. Decide: ledger it as a D-TERRAIN row (cache-trust is the
      witnessed semantic) or record it in `terrain-re.md` as a deliberate tool-side
      divergence
- [ ] `opennova::io` adoption continuation: migrate remaining per-lib byte readers on-touch (policy in engine/CLAUDE.md); excluded: mus/wac VM cursors (faithful-port surface) and cpt (a real migration, tracked as its own row below)
- [ ] NovaWorld disconnect-state reset (owner: `godot/game/novaworld_panel.gd`): clear all connection-derived rows, login/join state, pending mission/player data, and disable Host/Join/Login on disconnect or error. Acceptance (`godot/tests/novaworld_panel_test.gd`): a populated, logged-in, pending-join panel returns to a clean disconnected state and cannot submit a stale row. Coordinate with the unlanded novaworld_panel rework held in the `gsb` worktree (WIP commit 0fd58850b on `worktree-gsb`; the branch's earlier commits landed via #300) before landing.
- [ ] Converge `engine/formats/cpt`'s bit codec on `io/bit_stream.h` (owner: `engine/formats/cpt/cpt_io.cpp`): the two have diverged (cpt's writer carries a normalizing `set_position` and a `write_to_file`; its reader now carries `remaining_bits`), so this is a real migration, not a swap — the reason it is tracked separately in `engine/CLAUDE.md`. Acceptance: `parametric_parity_test` still reports byte-identical CPT output for all four fixtures after cpt drops its private copy.
- [ ] Vehicle-drive slice start (retail-join-0a): the `game-server` worktree holds WIP commit a6bf98a30 on `worktree-game-server` — VehicleTraits `ground_family`/`is_eweap` groundwork (6 files; based pre-#403, snapshot-committed 2026-08-04). Reconcile onto current master when the local vehicle-drive slice runs (#403's `VehicleTraits` since gained the items.def-derived family tag + air/water params, so this is a rebase-and-rethink, not an apply).
- [ ] W5 consolidations + push-downs (the 2026-07 quality campaign's last wave;
      waves 1-4 landed as #310-#375): perf-span unify, sim debug-snapshot
      narrowing (debug half only), and remaining runtime push-downs — opportunistic
- [ ] Cross-mission debug-intent replay: `DebugSession._explicit_values` is never
      cleared, so `sync()` replays explicit debug writes into a NEW mission's fresh
      sim/terrain targets. Decide the cross-mission scope of debug intent: clear on
      mission unload, or document the replay as intended.
- [ ] Dev-tools windows (ADR 0039; `engine/runtime/devtools/README.md` is the
      recipe): the retired F3 pages return as engine ImGui windows as they are
      wanted — entities + pick list, sim transport (play/pause/step; the MCP
      `game_debug` control plane still drives these), script vars, net, particles,
      occlusion, rounds, terrain, rendering/world-view toggles (`GameWorld`'s
      typed API), environment, audio, animation, player (with the pose dump the
      retired `DebugSnapshotWriter` produced for `pose_replay_probe` /
      `terrain_seam_probe`), and the `PerfTimeline` ring. Data the shell alone
      has crosses as VALUE slots or typed records, never Godot objects.
- [ ] Stats window info cells not carried over from the retired page (they read
      Godot objects at refresh): Performance draws/objs/prims/nodes on the Render
      row, the a11y flag on Flush tail, peer count (Net), effects live count,
      fire/destruction/throwable/wire present stats, occlusion counts (`occl`),
      the sim row's entity count + role. Each returns as a VALUE slot fed by the
      shell sampler that owns the source.
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
- [ ] Full Linux core tests: add an Ubuntu leg for the complete native suite after triaging any platform-only failures. Acceptance: the full CTest suite runs on Linux for every PR without relying on the net-only or packaging jobs.
- [ ] Incremental conventional linting: establish project-owned formatting settings, then add per-language lint checks in advisory or changed-file mode before enforcing them. Acceptance: CI checks new changes without requiring a repository-wide reformat, with documented local commands for each enabled linter.
- [ ] Two ctests are `DISABLED TRUE` in `tests/CMakeLists.txt` with reasons recorded but no owner: `parametric_parity` (long byte-identical CPT fixture run, disabled pending CI stability/perf cost) and `particle_smoke_all_fixtures` (waiting on the full 77-file corpus being mirrored into `fixtures/particle/`). Acceptance: each is either re-enabled or converted into an env-gated test alongside the rest of the asset-gated set (`docs/asset-gated-tests.md`).
- [ ] Serve mode (PROD-1, ADR 0015): `opennova.exe --server` / `--headless
      --server` and a packaging boot-smoke leg — tracked future work, never
      implemented; specs live in `docs/maturity-program.md` §PROD.
