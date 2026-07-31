# ONED editor-layer improvement program

> **Superseded.** This program is COMPLETE (2026-07-04, landed via the PR #180
> lineage) and this doc is a historical record. The active program is the
> [maturity program](../maturity-program.md); its editor track detail is
> [workspace-maturity-program.md](workspace-maturity-program.md).

Status doc for the editor-layer refactor program (started 2026-07-01 on
`oned-editor-layer`, A1–A7 merged to master; **resumed 2026-07-04 on
`oned-editor-layer-b`** for A8 + B1–B12, landing as one trunk PR with
independently-green slice commits). Workstream A realigns the architecture
(app root, shell decomposition); Workstream B closes UX gaps (undo everywhere,
global shortcuts) and unifies widgets/theme. Each phase is an
independently-green, commit-sized slice; the program can stop at any slice
boundary and still have paid down real debt.

Line references below were re-verified 2026-07-04 against post-#179 master
(the #176 adapter moves and #178 mission-inspector decomposition shifted the
originals); anything marked *re-grep at execution* drifts too easily to pin.

## Status 2026-07-04 (end of the second execution session — PROGRAM COMPLETE)

*Counts in this snapshot are frozen at completion time — eleven workspaces;
Avatars joined as the twelfth afterward (maturity program, ONED-A).*

ALL PHASES LANDED on `oned-editor-layer-b` (rebased onto the post-#181
`hygiene-2026-07` base), one green slice commit each: A8 (full), B1–B8, B10,
then this session **B9** (`3121fe71` — shell services formalization, with
B10's five deferred push_error→toast reroutes riding the new seam:
music `bank_mode.gd` ×4 and `terrain_editor_document.gd` ×1, wired
signal→owner→workspace), **B11** (`5dd1e4f5` — theme move + accent
single-source, guarded by `editor_theme_accent_test`), **B12** (`d109ed76` —
`ResourceKinds` vocab dedup, pinned by `resource_kinds_test`; the ref
widget's private 3-row jump map was missing sbf/music_script→music, a
latent music-jump bug now fixed).

END GATE: FULL GUT green — 189 scripts / 1927 tests / 27,281 asserts, no
parse drops. Visual pass via the screenshot driver over ALL ELEVEN
workspaces with real assets (`hud.png` is new; `scripts/
capture_screenshots.sh` validates eleven editor shots now): theme + accent
render correctly post-move (active-row emphasis reads the palette live),
the mission open captured the GREEN success toast in-frame (B9 notify →
B10 severity path), object/hud/menus/music/sound all present. The
JOX flat extract was missing the driver's MH53 hero set (REVX02 carried it
and was consolidated away 2026-07-04); MH53.3di + its nine textures were
re-extracted from retail `resource.pff` into `~/Desktop/JOX`.

Residual manual spot-checks (headless-pinned, not hand-verified this
session): error/warn toast colors (severity mapping tested;
info/success seen live), the Ctrl+S/Z/Y hand matrix incl. text-field focus
(pinned by `editor_shortcuts_test`), popover exclusivity + detach (pinned
by the A8 shell tests).

Design provenance: three parallel code sweeps plus two verification passes
against the code (2026-07-01). Where a design claim conflicted with the code,
the code won; corrections are folded in below.

## Why (the five debt clusters found)

1. **The app root was the terrain editor.** `editor_main.tscn`'s root script
   was `terrain/terrain_editor.gd`; MCP boot, window sizing, project-dir
   persistence, environment-document hosting, and a legacy save flow lived in
   one workspace's file, contradicting "terrain is one of eleven workspaces".
2. **Shell god-object.** `editor/editor_workstation.gd` (2,768 lines, ~15
   subsystems) with two remaining type-switches and two parallel
   unsaved-changes flows.
3. **Undo fragmentation and gaps.** Six-plus history implementations; Object
   and Credits have no undo at all; undo/redo keyboard handling re-implemented
   in five workspaces with divergent focus guards; no global Ctrl+S anywhere.
4. **Inspector/widget fragmentation.** The de-facto shared forms library lives
   under `object/ui/` and is imported by mission/terrain/sound and the
   framework base; `framework/inspector.gd` is object-coupled so terrain
   forked it; five bespoke search bars; shell services consumed via
   copy-pasted `has_method` guards.
5. **Polish drift.** Editor-wide theme mislocated under `terrain/ui/theme/`;
   accent orange hardcoded off-theme in six files (with precision drift); the
   status toast renders single-severity amber for successes AND errors; stale
   READMEs (HUD registration, terrain scene-backed claim); HUD missing from
   the README table and the screenshot driver.

## Landed (Workstream A, phases 1-7)

| Phase | Commit | What landed |
|---|---|---|
| A1 | `7e955a0e` | One unsaved-changes flow: legacy terrain pending-action path deleted; everything converges on `prompt_unsaved_for` + the shared `save_then` (Save As fallback for path-less documents); camera-Escape routes to the shell's unified close guard via new `request_close()` |
| A2 | `3aa07ab0` | Tile gizmo rides new `get_tile_gizmo_state()` / `run_tile_gizmo_action()` capability hooks; the shell's `editor.*tileinfo*` calls and terrain-enum branch are gone |
| A3 | `b3d772c1` | Popup workspaces route through the `WorkspaceDef.popup` registry path (`_popup_workspaces`); new public `get_workspace_adapter()` replaced all six external reach-ins (screenshot driver, four test sites, two MCP lookups that would have silently nulled) |
| A4 | `c407c7ba` | **EditorApp root swap**: `editor/editor_app.gd` is the scene root (boot wiring, window sizing, environment document, MCP service); TerrainEditor demoted to a child keeping TerrainWorldRoot (mission edit mode renders into the same world); adapters unwrap duck-typed; all scene-consuming tests converted; verified by full suite + the headless boot probe |
| A5 | `bda28c3a` | Camera strictly via `get_viewport_camera()` (camera popup disables in non-3D workspaces instead of silently editing the hidden terrain camera); `tests/editor_app_boot_probe.gd`; docs say eleven workspaces + EditorApp |
| A6 | `737818e2` | Shell decomposition 1/3: `editor/shell/` gains ShellActionBar (the WorkspaceAction enum moved in as `Action`; top bar + env popup are two instances), ShellDocumentTabs, ShellStatusBar, TileGizmoOverlay |
| A7 | `eaa1168a` | Shell decomposition 2/3: ShellSaveExportFlow (file dialogs, Save As routing, action dispatch, unsaved/CDEP/export-flavor confirms, `save_then`) + ShellExportProgress (modal overlay). Shell: 2,768 → ~2,010 lines |

Gate protocol used per slice: the canary file
(`terrain_editor_workstation_test.gd`, 89 tests) plus the slice's affected
files; full suite at A4. The full-run crash in
`terrain_editor_import_export_test.gd` is the suite's known shared-`user://`
flakiness (passes isolated); every file skipped by that crash was run
standalone green.

## All phases landed (A8 + B1–B12: what each became)

The full phase specs (design detail, call-site inventories, per-slice gates)
live in this doc's git history; the outcome per phase:

- **A8 — shell decomposition 3/3**: `editor/shell/popover_dock.gd` (popover
  exclusivity + detachable-panel restore), `settings_panel.gd`, and
  `layout_persistence.gd`; the test reach-in sites updated with the seams
  kept intact.
- **B1 — SnapshotEditSession**: `framework/snapshot_edit_session.gd` over
  native `NovaEditHistory`; `EditorDocument` delegates unchanged, and
  `framework/editor_resource_document.gd` gained the identical opt-in tier.
- **B2 — Credits undo**: credits documents opt in via the
  `CbinCreditsResource` `to_text()`/`from_text()` snapshot pair
  (`credits_editor_undo_test.gd`).
- **B3 — native object snapshots**: `NovaObjectData.snapshot_edit_state()` /
  `apply_edit_state()` in `godot/engine/object/nova_object_data.h` +
  `nova_object_data_state.cpp`.
- **B4/B5 — Object undo**: equal-gated shadow steps recorded at the
  `object_changed` funnel against a cached baseline, with focus-bracketed
  session coalescing (typing/dragging = one step).
- **B6 — global shortcuts**: Ctrl+S / Ctrl+Shift+S / Ctrl+Z / Ctrl+Shift+Z /
  Ctrl+Y in `EditorWorkstation._shortcut_input`; the per-workspace key
  handlers deleted; one focus guard (`editor_shortcuts_test.gd`). Standing
  decision kept: the dual dirty shape (`is_dirty()` method vs `is_dirty`
  property) stays — renaming would churn ~176 accesses across 39 files and
  name-collide with the var in GDScript.
- **B7 — forms unification**: `object/ui/object_ui_helpers.gd` →
  `framework/inspector_forms.gd` (`InspectorForms`); `WorkflowInspector`
  de-coupled from object; terrain's inspector fork retired. Row builders
  stay per-domain — the UiBox decision, recorded in
  `godot/engine/ui/ui_box.gd`.
- **B8 — SearchField**: `framework/search_field.gd`, adopted at the six
  bespoke search-bar sites (`search_field_test.gd`).
- **B9 — shell services**: protected `EditorWorkspace` helpers
  (`_notify_status`, `_sync_shell`, `_mount_under_shell`) replaced the
  duck-typed `has_method` blocks; five user-actionable `push_error`s reroute
  to toasts over the new seam.
- **B10 — toast severity**: `show_status_message(text, duration, severity)`
  with per-severity defaults and Info/Success/Warn/Error theme variations.
- **B11 — theme + accent**: theme moved to `modtools/editor/ui/theme/`;
  accent single-sourced as `EditorPalette/colors/accent`
  (`editor_theme_accent_test`).
- **B12 — vocab dedup**: `framework/resource_kinds.gd` (`ResourceKinds`)
  adopted at the four kind-map sites (fixing the ref widget's missing
  `sbf`/`music_script → music` jump rows); docs/driver refresh incl. the
  HUD screenshot.

## Execution conventions

- One slice = one commit, independently green and revertable; scoped GUT
  files first, full suite at B7, B11, and program end. Run build/test outside
  the sandbox (linked-worktree rule).
- Flaky protocol: the full suite shares `user://` state — re-run a failing
  file in isolation before believing it (`godot/tests/CLAUDE.md`);
  `terrain_editor_workstation_test.gd` is the canary.
- GUT exits 0 on parse errors and silently drops scripts — `scripts/
  test_godot.sh`'s greps (or equivalents) gate every class_name/path move.
- Manual oned-run passes after B6, B10, B11: all eleven workspaces
  (Ctrl+1..9), Ctrl+S/Ctrl+Z in each including text-field focus cases,
  dirty-guard prompts, popovers + detach, toast colors error vs success,
  screenshot driver run.
- Native (B3 only): `scripts/build_godot.sh`, stale-DLL check, editor
  restart, `--import` once, scoped ctest.

## Top risks

1. GUT silently drops scripts on parse errors — class_name/path moves (B7,
   B11) are the hazard; stale-name greps pre-commit.
2. Shortcut phase change (B6): the shell claims Z/Y in `_shortcut_input`;
   pinned guard-matrix test + per-workspace manual sweep.
3. Object native snapshot correctness (B3/B4): byte-roundtrip + structural
   GUT cases; the stale `build/Debug/opennova.dll` rule.
4. RefCounted Callable cycles (B9): capture shell/Node locals in lambdas,
   never a RefCounted self (pattern documented at `terrain_workspace.gd`,
   asset-dock services).
5. Theme move uid/path resolution in headless runs (B11): rewrite every
   reference in the same commit; settle imports; screenshot smoke.
