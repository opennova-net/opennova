# ONED editor-layer improvement program

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

## Remaining phases

### A8 — shell decomposition 3/3 (DECIDED 2026-07-04: FULL extraction)

The maintainer chose the full extraction: `editor/shell/popover_dock.gd`
(camera/env/settings popover exclusivity + detachable-panel restore),
`settings_panel.gd` (settings popup content + handlers), and
`layout_persistence.gd` (splits, browser pane, min-size), updating the ~30
test reach-in sites that poke the moved internals
(`_set_browser_pane_visible`, `_browser_pane`, `_set_*_popup_visible`,
`_camera/_environment_panel_host`, `_panel_restore_for`). The popover block
is entangled with the editor binding, view guides, and the env action bar —
extract it with its seams intact rather than redesigning them.

### B1 — SnapshotEditSession extraction

Extract `editor/editor_document.gd`'s history block into
`framework/snapshot_edit_session.gd` (`SnapshotEditSession` over native
`NovaEditHistory`: begin/commit/flush_edit, record/push_undo_step,
can_undo/can_redo, undo/redo, clear; limit 100). EditorDocument keeps its
exact API and delegates (zero churn for strings/sound/environment/object).
`framework/editor_resource_document.gd` gains the identical opt-in tier
(+`clear_history()` in create_new/adopt_loaded; hooks `_snapshot()`,
`_apply_snapshot()`, `_history_applied()`); fonts/mnu documents don't opt in.
Gate: strings/environment/sound/object/music suites, full GUT.

### B2 — Credits undo

`credits_editor_document.gd` opts in: `_snapshot() = resource.to_text()`,
`_apply_snapshot = resource.from_text()` (the `CbinCreditsResource` pair,
production-proven by the source view; entry-identity reset on undo matches
source-Apply — accepted, documented in the test). Mutation sites: block list
insert/delete/move/drop (list gains `set_document()`); card text typing
begin-on-first-change / commit-on-focus-exit; color commit on `popup_closed`;
align/font/image singles via `push_undo_step`; image X/Y spins commit on
`get_line_edit().focus_exited` (mirror `environment_inspector.gd`);
source-view `apply_pending` = one step. Shell: nothing (the base derives
can_undo/undo from `get_editor_document()`). New
`tests/credits_editor_undo_test.gd`. Gate: nine credits test files,
fnt_workspace_test, mnu suite, document_tabs_test.

### B3 — NovaObjectData native snapshot pair (C++)

`NovaObjectData` has no byte serializer. Add
`snapshot_edit_state() -> PackedByteArray` / `apply_edit_state(bytes) ->
Error` in `godot/engine/object/nova_object_data.{h,cpp}`: version tag +
object_name + source-kind + poly_collision_lod + per-LOD TdpLod scalars +
`ir.materials[]` / `ir.lights[]` / per-LOD `part_animations[]` (verified POD
in `libs/threedi/include/threedi/threedi_ir.h:131-232`) + `oed_dirty_mask`.
Apply validates counts (materials/lights/LODs fixed by geometry →
ERR_INVALID_DATA; part-anim arrays realloc since add/delete changes count)
and ends with `_notify_object_changed(UPDATE_ALL)`. Rebuild the GDExtension,
delete any stale `build/Debug/opennova.dll`, full editor restart, scoped
ctest `-R "object|threedi"`. Gate: object_editor_test + new native cases.

### B4 — Object undo (shadow-step funnel)

Every object mutation already funnels through `object_changed` →
`ObjectEditor._on_object_data_changed` (CONNECT_DEFERRED) — record
equal-gated undo steps there against a cached pre-mutation baseline, so the
~40 inspector call sites need zero changes. Apply wrapped in a
`_suspend_history` guard (mirror `environment_editor.gd`); `clear_history()`
+ baseline reset on new/open (mirror `strings_editor.gd`). New
`tests/object_editor_undo_test.gd`: field roundtrips, part-anim add/delete
restore, native byte roundtrip, mismatched-geometry rejection,
`ws.can_undo()/undo()`. Closes the "Object has no undo" gap.

### B5 — Object undo session coalescing

The object workspace subscribes to the viewport's `gui_focus_changed` while
active: focus entering a LineEdit/SpinBox under the inspector →
`begin_edit()`, leaving → `commit_edit()`; ColorPickerButton sites bracket on
first `color_changed` / `popup_closed`. Typing/dragging = one undo step.

### B6 — Global shortcuts + handler deletions + dirty-override collapse

New `EditorWorkstation._shortcut_input(event)` (fires after `_gui_input` —
focused text fields keep native Ctrl+Z — and before `_unhandled_input`, so
viewport routers never see claimed keys): **Ctrl+S** → the flow module's
save-pressed path (already falls through to Save As on ERR_INVALID_PARAMETER);
**Ctrl+Shift+S** → save-as; **Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y** → the active
workspace's can_undo()/undo()/redo() hooks + sync. ONE focus guard for
undo/redo only (focus owner is LineEdit/TextEdit/SpinBox; Ctrl+S deliberately
unguarded — flush covers buffers). Mission's sim gate survives inside
`undo()` itself (`_reject_edit_while_simulating`), so the shell needs no sim
awareness.

Delete the per-workspace key handlers: `mission_controller.gd:1357-1361`
(+ drop KEY_Z/KEY_Y from `is_mutator` at :1292, keep Delete/Backspace),
`terrain_editor.gd:466` viewport Ctrl+Z arm, `mnu_editor.gd:995-1000`,
`mnu/mns_editor.gd:390-395` (moved into `mnu/` by the #176 adapter moves),
`fonts/fnt_editor.gd:914-919` Z/Y arms only (keep Ctrl+C/V + tool keys).
Music has no handler today (gains keyboard undo through the global path).

Dirty contract: KEEP the dual shape (`is_dirty()` method vs `is_dirty`
property — renaming would churn 176 accesses across 39 files and
name-collides with the var in GDScript); instead the base
`has_unsaved_changes()` gains a tab fold (any `get_document_tabs()` row
dirty) and the three overrides collapse: delete
`strings_workspace.gd:209-214`, delete `fonts_workspace.gd:188-189` (add
`FntEditor.is_dirty()` forwarder), slim `mnu_workspace.gd:726` to
`mns_document.is_dirty or super()`. Also delete music's unused undo aliases
(`music_editor_document.gd:1300-1303` — `undo_bank`/`redo_bank`/
`undo_script`/`redo_script`; 11 call sites across two test files).

Rewrite `mission_controller_test.gd:1660-1669` (drove undo via a viewport
key event) keeping a sim-gate rejection case on `controller.undo()`; new
`editor_shortcuts_test.gd` pins the guard matrix. Behavior deltas to flag:
ItemList-focused Ctrl+Z now undoes (was blocked in mission); music gains
keyboard undo. Gate: mission/mnu/music/fnt suites, editor_shell_redesign.

### B7 — Forms unification (one mechanical commit)

Move `object/ui/object_ui_helpers.gd` → `framework/inspector_forms.gd`,
class_name `InspectorForms` — the rename propagates everywhere (147
occurrences / 16 files re-counted post-#178: mission_inspector 23 + its seven
section components under `mission/inspectors/` (scripting 18, zones 13,
waypoints 10, properties 7, loadout_groups 6, place_palette 3,
objects_browser 3) + `param_slot.gd` 1 + `mission_controller.gd` 1,
framework/inspector.gd 20, sound_inspector 18, strings_inspector 8,
strings_detail_dock 8, terrain_inspector 7, plus the definition site). The two ctrl-reg builders (depend on
`object/ui/widgets/ctrl_reg_picker.gd`) split into new
`object/ui/object_forms.gd` (`ObjectForms`); `build_channel_card` moves
(generic).

De-couple `framework/inspector.gd`: `WorkflowInspector` gets untyped `_ws`,
`_init(workspace = null)`, generic wrappers, no-op
`_rebuild_detail_dock`/`_notify_shell` defaults; object accessors move to two
thin bases `object/ui/inspectors/object_inspector.gd` +
`object_list_detail_inspector.gd` (~30 documented duplicated lines — GDScript
has no mixins); extends updates: preview/lods → ObjectInspector;
materials/lights/part_anims/anims → ObjectListDetailInspector. Retire the
terrain fork: `terrain/ui/terrain_inspector.gd` extends WorkflowInspector,
its duplicated wrappers and hook re-declarations deleted, its five subclasses
untouched. `MnuUiHelpers`: only `add_heading`/`add_muted` become delegates
(row builders deliberately stay per-domain per the recorded UiBox decision;
`music/mus_forms.gd` is a construct palette, not a forms lib — untouched).

Zero stale `ObjectUiHelpers` grep pre-commit; FULL GUT with the silent-drop
protocol (class_name moves are the known silent-test-drop hazard).

### B8 — SearchField shared widget

`framework/search_field.gd` (`class_name SearchField extends LineEdit`;
`signal search_changed(text)`; `_init(placeholder)` sets placeholder +
clear_button_enabled + expand flags + text_changed relay). Adopt at six sites
preserving node names (mission's two moved into the #178 section components):
`strings/ui/strings_inspector.gd:41`,
`mission/inspectors/objects_browser_inspector.gd:72`,
`mission/inspectors/place_palette_inspector.gd:43`,
`mnu/mnu_string_picker.gd:29-30`, `editor/editor_pff_tool.gd:143`,
`framework/resource_table.gd:46`. New `search_field_test.gd`.

### B9 — Shell services formalization

Protected helpers on `EditorWorkspace` (guards inside, documented in the
contract header): `_notify_status(message, severity := &"info", duration :=
0.0)`, `_sync_shell()`, `_host_under_shell(node)`. Replace the duck-typed
blocks: sync ×4 — the `_sync_shell_title()` guards on `sync_from_editor_state`
(`strings:322`, `sound:176`, `mission:588`, and `object:384` — COLLISION:
object's richer private `_sync_shell` renames to `_after_document_changed()`);
status ×7 (`mnu_workspace:520,599`; `mission_workspace:440,483,581,596`;
`terrain_editor.gd:236`) — the two `modtools/mcp/` guards stay as-is (the MCP
service is not a workspace; it keeps its own shell seam); hosting sites
*re-grep at execution*. Mission VFS: `mission_workspace.gd:288/297` → the
base `_resource_root()`; the controller's four `get_resource_root` reach-ins
(≈:484,:577 + two later, *re-grep at execution*) → one private controller
`_resource_root()` wrapper (the controller runs headless — don't reroute
through the workspace). Inspector reach-throughs
(`strings_inspector.gd:106`, `anims_inspector.gd:347`) get
workspace-mediated accessors. Gate: mission/mnu/strings/sound workspace
tests.

### B10 — Toast severity

`show_status_message(text, duration := 0.0, severity := &"info")` — duration
<= 0 → per-severity default (info 4.0 / success 3.5 / warn 6.0 / error 8.0);
the status module maps severity → theme variation; add `Info` + `Success`
(green ≈ 0.55,0.8,0.55 per the credits source view) variations to the theme
(`Warn`/`Error` exist). Classify existing callers; route the five
user-actionable push_errors to toasts keeping the console line
(`music/ui/bank_mode.gd:359,363,384,430`,
`terrain/terrain_editor_document.gd:708`). Gate: editor_shell_redesign +
an oned-run visual pass.

### B11 — Theme move + accent single-source

Move `modtools/terrain/ui/theme/` → `modtools/editor/ui/theme/` (joins
`editor/ui/icons/`). Rewrite ALL references in the same commit:
`editor_workstation.tscn` (uid + path), `fnt_editor.gd` const, the `.tres`'s
four icon ext_resource paths, four `.svg.import` source_file lines (bulk
rewrite via bash/python — the BOM-less UTF-8 rule; the uid survives but paths
must be truthful for uid-cache-cold headless runs).

Accent single-source: add `EditorPalette/colors/accent =
Color(0.8392, 0.5529, 0.2902, 1)` as a named theme color; the six hardcode
sites (`editor_workstation.gd` active-row stylebox,
`credits_editor_block_card.gd` — which drifted to 0.84/0.55/0.29,
`fnt_editor.gd` ×2, `mnu_canvas.gd`, `quadrant_board.gd`) become
`get_theme_color(&"accent", &"EditorPalette")`, alpha variants via
`Color(accent, a)` — all are Controls under the themed tree. Chosen over a
const file because the theme already holds the accent ~20 times; a const
file would be a second source needing a parity test. One guard test pins the
palette value and regex-sweeps for hardcoded accent triples. Gate: FULL GUT
+ oned-run + screenshot smoke.

### B12 — Vocab dedup + docs/driver refresh

New `framework/resource_kinds.gd` (`ResourceKinds.JUMP_KIND`, `jump_kind()`,
`label()` lifted from `resource_browser.gd:230-257`); adopt at
reference_strip, resource_browser_pane, resource_ref_widget (fixes its copy's
missing `sbf`/`music_script → music` rows — a latent music-jump bug),
resource_browser; the pane's `_KIND_FILTERS` stays local. Docs (trimmed
2026-07-04 — the hud/terrain README fixes landed in PR #180 and #169 already
added HUD to the `modtools/README.md` table): `object/README.md` +
`framework/README.md` ride-alongs only. `tools/screenshot_capture.gd` adds
the HUD shot (nil-asset branch if HUD opens document-less → eleven shots).
Gate: editor_open_in_workspace_test, full GUT.

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
