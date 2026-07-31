# ONED workspace maturity program (the maturity program's ONED track)

**This document is the ONED track detail of the
[maturity program](../maturity-program.md)** — the umbrella owned waves,
freeze policy, cross-track ordering, and enforcement; this doc owns the
editor-side phases. **The umbrella program closed 2026-07-12 (freeze
lifted); this doc survives as the editor's standing roadmap** — phases
execute as ordinary slices under the standing ADRs, RE gates unchanged.
Standing rules this track builds under: ADR 0015 (two
products, serve mode), ADR 0016 (editor over public engine APIs — no
bypasses), ADR 0017 (typed records, named constants), ADR 0018 (public-API
testability).

Successor to [editor-layer-program.md](editor-layer-program.md) (complete
2026-07-04: app root, shell decomposition, undo everywhere, global
shortcuts, shared widgets/theme/vocabulary). That program made the
workspaces *uniform*; this one makes them *equally deep* — and grew them to
**twelve** (Avatars joined in phase AVA, merged 2026-07-04).

Facts below were re-verified 2026-07-04 against the post-program tree;
anything marked *re-grep at execution* drifts too easily to pin.

> **Counting note (2026-07-22).** Every "twelve workspaces" in this document is
> this program's scope as it stood at close. ONED has since gained a thirteenth,
> Particles, which landed with the `.ptl` stack on 2026-07-12 and was never on
> this program's matrix. The live count is
> `EditorWorkstation._workspace_defs()`; the live list is
> [`godot/modtools/README.md`](../../godot/modtools/README.md).

## The bar (what "up to snuff" means, per workspace)

Four capabilities, each either met, or covered by a tracked divergence
decision (ADR / RE-record entry) saying why not:

- **R — Read.** Every retail on-disk form of the workspace's format opens
  (loose, VFS, PFF-entry). Known-excluded forms are pinned by a test and a
  doc entry, not silently unreadable.
- **W — Write.** Save/export produces bytes from scratch (ADR 0003), gated by
  roundtrip tests and, where retail corpora exist, byte-parity sweeps. No
  read-only workspaces without a recorded decision.
- **E — Engine preview.** The preview IS the ported engine path — the same
  nodes/code the game runs, not an editor-drawn approximation (ADR 0016).
  (The terrain viewport and the credits `NovaCreditsPlayer` are the
  exemplars; the HUD's hand-drawn `_draw()` canvas is the anti-pattern.)
- **G — Game loop.** The author saves canonical loose assets, then launches
  the real game: F5 runs the normal standalone game and F6 runs the current
  saved, top-level loose mission. F3 is the standalone game's debug UI, not
  an editor launcher. Authoring that cannot be seen in the running game is
  not done.

## Current matrix (verified 2026-07-04; live-run policy refreshed 2026-07-29)

| Workspace | R | W | E | G | Weakest axis today |
|---|---|---|---|---|---|
| Terrain | ✓ | ✓ | ✓ (viewport is the renderer) | ✓ (save loose assets, then F5/F6 runs the standalone game) | at bar — the E/G exemplar (TER-1, 2026-07-12) |
| Environment | ✓ | ✓ | ✓ (runtime nodes shared; water unified) | ✓ (save loose assets, then F5/F6 runs the standalone game) | at bar (ENV-1, 2026-07-12) |
| Object | ✓ (v8/v9; LW v10 on branch) | ✓ (.3dp + .3di export) | partial (isolated preview; env-lit but no world context, no LOD-by-distance) | — | E |
| Mission | ✓ | ✓ | ✓ (authoring previews reuse runtime seams) | ✓ (F6 runs the current saved loose `.bms` in the standalone game) | data semantics (heading, ANIMNUM) |
| Credits | ✓ | ✓ | ✓ (`NovaCreditsPlayer` hosted in the editor; play/pause/scrub transport, CRE-1) | — | G (F3 wiring = CRE-2) |
| Fonts | ✓ | ✓ | ✓ (type-a-line sample through the game's draw path beside the glyph paint canvas, FNT-1 2026-07-12) | — | G (F3 wiring = FNT-3) |
| Strings | ✓ | ✓ | ✓ (selected entry rendered through the engine path with a font picker, STR-1 2026-07-12; encoding pinned by STR-2, D-FNT-4 minted) | — | G (F3 wiring = STR-3) |
| Sound | ✓ | ✓ (`write_lwf`) | ✓ (audition rides the runtime set→member→wav path) | — | G |
| Music | ✓ (headerless form excluded by pinned decision, D-SCR-1/2) | ✓ | ✓ (live director runs the VM) | — | G + usability (see MUS-D) |
| Menus | ✓ (.mnu/.mns) | ✓ | ✓ (canvas hosts live `NovaMnuMenu`, edit_mode on) | partial (no flow run; actions authored blind) | G |
| HUD | ✓ (`NovaHudPos`, read-only getters) | — | — (hand-drawn `HudLayoutPreview._draw()`) | — | everything but R |
| Avatars | ✓ | ✓ (from-scratch writer, ADR 0021) | ✓ (workspace + 3D preview) | partial (runtime `player.mnu` population; in-world appearance open, D-PLAYERINFO-1) | G |

Two corrections to earlier working assumptions, verified: environment/water
unification is DONE (editor-runtime-parity.md: "water is fully unified
(parameterized, not forked)"), and credits' preview already hosts the real
`NovaCreditsPlayer`. The headerless MUS cipher stays out of scope by the
recorded decision in docs/audio/mus-sbf-re.md (`vfs_mus_decode_test` pins
the pass-through); it is a bar-satisfying exclusion, not a gap.

## AVA — the Avatars workspace merge (twelfth workspace; first ONED train) — LANDED 2026-07-04

Merged from `playerinfo-runtime` as the twelfth workspace (the umbrella's
ONED-A train): editor (`modtools/avatar/` — workspace, document, preview,
three inspectors), engine (`nova_avatar_database` native Avatars.def
support), fixtures, five test files, and the RE doc. The branch's ADR
renumbered 0013 → 0021 (it collided with master's consolidated-net-core
0013) with a repo-wide reference sweep, and the eleven→twelve sweep is done
(CONTEXT.md, the `modtools/README.md` table, the screenshot driver's
twelfth shot, this doc's matrix row). Gate ran: the branch's five avatar
test files + FULL GUT + the boot probe + the screenshot driver.

## Foundation phases (F1–F5; umbrella Wave 1)

### F1 — WorldContextPreview service

A reusable framework piece that mounts a workspace's subject into the real
world: terrain world root + environment + fog + grounding, the same nodes the
game renders with (docs/oned/editor-runtime-parity.md patterns). Extracted
from the seams mission/terrain already share — placement grounding from the
controller's placer, viewport mechanics via `framework/viewport_mount.gd` —
with the seams intact, A8-style, not redesigned. Consumers: Object (OBJ-1
place-on-terrain preview), Sound (SND-2 at-distance audition), later any
workspace needing in-world context. Gate: new framework test + object/mission
suites; no behavior change for existing mounts.

### F2 — EngineTextPreview widget

One widget that renders sample text through the ported font path the game
uses (the runtime draw that credits/killfeed/menus text rides — exact seam
*re-grep at execution*), replacing nothing but giving Fonts/Strings/HUD a
truthful "how the game draws it" panel. Gate: widget test + a golden-ish
visual check in the driver run.

### F3 — Play-with-overrides launcher ("See it in game")

The editor gains one affordance that launches the game runtime with the
authoring directory as the loose-file override — the engine's own mechanism
([orig: `/d` loose-override @ 0x4a7310], already ported per the runtime
launch flags). The game runtime stays PFF-mount-first; `/d` overlays the
authored files exactly as retail did, so every workspace's G axis is the same
gesture: save, launch, see it. Editor side: a shell action (enabled when a
runtime binary/scene is available) + per-workspace doc of what to look at.
Packaged-product side is already guarded: the packaging boot smoke landed
2026-07-04 with the `run/main_scene.modtools` fix (every exported exe must
boot headless before it ships).

### F4 — Writer-parity convention (doc-only) — RECORDED (this section is the record)

**The standing W gate.** A workspace's Write axis counts as met only when all
four ship together:

- **(a) Writer from scratch** — output produced entirely from the in-memory
  model; never raw input bytes smuggled through
  ([ADR 0003](../adr/0003-no-raw-passthrough-create-from-scratch.md)).
- **(b) Roundtrip test** — write → read → semantic equality, in the format
  lib's ctest suite.
- **(c) Retail-corpus byte sweep** — where a retail corpus exists, a
  byte-parity sweep over it (asset-gated per
  [docs/asset-gated-tests.md](../asset-gated-tests.md); skip-as-pass never
  substitutes for a local attested run).
- **(d) A `D-<DOMAIN>-n` entry when output legitimately differs** — minted in
  the format's RE record via re-doc and ledgered in
  [docs/divergence-ledger.md](../divergence-ledger.md) at birth, per
  [ADR 0022](../adr/0022-divergence-burn-down.md).

Already practiced by the terrain, 3di, lwf, mnu, and mission writers; recorded
here it is the stated gate for the new writers (hudpos in HUD-1, avatars in
AVT-1) and every W phase after them. `libs/CLAUDE.md` points here from its
parity-writer rule.

### F5 — MissionController decomposition — DONE (#204 + #373)

`mission/mission_controller.gd` was 4,033 lines (largest file under any
workspace; backlogged since #179). The decomposition landed in two mechanical
stages, both before any MIS phase (the sequencing constraint held): the
Wave-1 trunk (#204, 2026-07-05) cut it to 1,645 lines and created
`mission/controller/` (the section base plus io/placement/reground/sim/
viewport/waypoint/zone ops), and the quality campaign's W4-6b (#373,
2026-07-30) extracted the history/selection/scripting/edit sections; #376
later added `preview_ops` and retired `sim_ops` with the standalone runtime.
The ~1,040-line residual is the composing facade the section framework
prescribes — document/selection/overlay state stays on the controller by
design (TODO.md records the same disposition).

## TST — public-seam test refits (umbrella Wave 2; ADR 0018)

The suite pokes `_private` members of other objects on ~1,300 lines; the
top offenders get refit slices, by ADR 0018's categories: (A) child-control
reach-ins → public accessors (the `get_workspace_adapter` precedent), (B)
private methods as entry points → public verbs, (C) internal state reads →
public getters for contract state. Order: `mission_inspector_test` (301
lines), `terrain_editor_workstation_test` (152 — the canary, handled with
extra care), `mission_controller_test` (100; F5 is done, so unblocked),
`mnu_canvas_test`
(92), the music suite (rides MUS-I's rebuild). Refit slices are
refactor-only: identical assert counts prove it. The long tail converts
adopt-on-touch under the ratchet.

## RSP — responsive shell (umbrella Wave 2)

Today: no content scaling anywhere, TWO conflicting window floors (1366×768
in editor_app.gd vs 1024×640 in layout_persistence.gd), split offsets
persisted as raw pixels, 21 `custom_minimum_size` sites in the workstation
scene alone. The bones exist (full-rect anchors, expand flags, draggable
splits, and `HudLayout.scale_rect` as scaler prior art).

- RSP-1: the responsive-shell policy ADR — the scale model (content scale
  vs chrome scaler), ONE window floor, ratio-based split persistence with a
  versioned migration for existing user state, and the fixed-size audit
  policy.
- RSP-2: implementation — floors unified, persistence migrated, the
  workstation chrome adapted, per-workspace fixed-size hotspots worked
  through the audit list.
- RSP-3: exactly one visual re-baseline slice (screenshot driver re-run;
  all shots change once, reviewed once).
- Gate: persistence-migration test; FULL GUT; the canary's deliberate edits
  called out in review; the re-baseline pass.

## MUS-D / MUS-I — the Music workspace redesign (Waves 2 → 3)

The maintainer's verdict: very hard to use. The structural causes are
verified: a single screen owning eight concerns (transport, breadcrumbs,
section map, program view, variables, volume meter, event log, follow-live)
— 1,951 lines when verified; the quality campaign's W4-6a (#372, 2026-07-30)
has since split it mechanically into a 779-line core plus four delegate
sections, relocating the mass without changing any of these causes; a modal
map↔section canvas swap (you never see both);
breadcrumbs implying a hierarchy the flat state machine doesn't have;
nested block widgets edited through popovers; the workspace opting out of
the shell left lane (the only one); and a separate Bank mode.

- MUS-D (Wave 2): a bounded design spike — UX model first (what an artist
  does, in what order), a prototype of the screen layout, and a design
  section landing in this doc. Signed constraints: the document/VM layer is
  untouched and its 46 tests stay green UNMODIFIED; shell left-lane
  conformance (workflow picker + inspector like every other workspace); no
  modal map/section swap (both visible or explicitly split); navigation
  honest about the flat machine; Live/Bank mode resolution; responsive-first
  (post-RSP policy). Ends at a maintainer gate. **The design landed 2026-07-12
  (the section below); the maintainer gate is the open item.**
- MUS-I (Wave 3): implementation in shippable slices — decompose the screen
  along its eight concerns, unify the canvas, rework block/expr editing per
  the accepted design. Every slice leaves the workspace usable.
- Gate: the 46 doc/VM tests unmodified-green throughout; new screen tests;
  FULL GUT; visual pass.

### The MUS-D design (2026-07-12; at the maintainer gate)

Deliverable of the MUS-D design spike (Wave 2). Grounded in the code as of
the design date: the screen `godot/modtools/music/ui/live_mode.gd`
(1,951 lines then; since W4-6a (#372) a 779-line core plus the
`live_mode_map_view/authoring_ops/nav_ops/transport_log` delegate sections) +
`live_mode.tscn`, the adapter `music_workspace.gd`, the untouched document
layer `music_editor_document.gd` (1,315 lines then, 1,306 today), and the
format/VM facts in `docs/audio/mus-sbf-re.md`. Ends at the maintainer gate;
MUS-I implements it.

The 46 doc/VM tests that stay green unmodified are exactly:
`godot/tests/modtools/music/music_document_test.gd` (19) +
`music_document_move_test.gd` (9) + `music_authoring_test.gd` (18) — all three
exercise `MusicEditorDocument` only, no UI. Everything below changes UI code only.

One copy decision applied throughout: the artist-facing word is **section**
(the format's own word — `get_section_names`, `add_section`, "Jump to section").
Today's UI mixes "state" and "section" ("Add State" button, "States" sidebar,
"Section map" header). The redesign unifies on "section" everywhere
(rename-misnomers-everywhere applies to copy too).

#### 1. The artist's workflow model

What `.mus` is, per the RE record: a **bank of tracks** (`.sbf`) plus a
**script** (`.bin`) of sections — a flat, variable-driven state machine. The VM
sits in one section at a time; it advances **only when the playing track
finishes** (witnessed pacing: one `play` per track completion,
`audio_stream_update @ 0x671c60`), so a section is best understood as *a
playlist with decisions between tracks*, and transitions always land on track
boundaries. The game drives it by writing **dials** (the 17 shared variables):
menu screens push their MUSICVAR, missions pump health %, threat distance,
team, etc. per frame. A per-script discriminator dial picks the path (menumus
reads dial 2, gamemus dial 1).

The artist's loop, in order — this ordering is what the screen must mirror:

1. **Gather tracks.** Import WAVs into the bank; name them (15 chars); preview.
   Tracks are the raw material; everything else points into this list by index.
2. **Sketch sections.** One per musical situation: intro, calm loop, combat,
   win sting. First section declared = where playback begins.
3. **Sequence each section.** Play steps top to bottom (`Play combat1 — waits`),
   repeats, then the decision logic: if / choose-by-value on dials, set/adjust
   a dial, go to another section.
4. **Wire the machine.** Transitions between sections; decide which dial
   selects the mood and name it (the profile sidecar carries friendly names).
5. **Audition as the game.** Start playback; watch the map light up and the
   program glow step by step; poke the dials the way the game would; jump
   straight to sections the dials never reach (retail gamemus' win/lose
   sections are unreachable dead content — the Jump affordance exists exactly
   for auditioning those); watch the meter and the activity feed.
6. **Iterate live.** Edits are parity-gated and undoable while playing; Save;
   run the standalone game with F5 (MUS-G rides MUS-I's tail).

Two hats, one canvas: *composing* (2–4) and *auditioning* (5) both stare at the
same map + program; audition just lights them up. That observation drives the
workflow split below.

#### 2. The screen layout

Today: the adapter opts out of the shell left lane (`uses_left_lane() → false`
in `music_workspace.gd` — the only workspace), and the whole UI lives in the
viewport: a private left dock (Tracks + Game Dials), a center column whose
canvas **swaps** between the section map (`%SectionMap` GraphEdit) and the
drilled-in program view (`MusicSectionProgramView`), a breadcrumb row, and a
bottom log strip. The redesign:

```
+--------------------------------------------------------------------------------+
| Shell top bar (Save / Run game (F5) / Run current mission (F6) / ...)            |
+----------------+---------------------------------------------------+-----------+
| SHELL LEFT     |  Transport: ▶ Play  ⏹ Stop | PLAYING | ▮▮ meter    | TRACKS    |
| LANE           |  Now playing: Combat (♪ combat1) | [x] Follow      | (asset    |
|                +---------------------------------------------------+  dock)    |
| Workflows      |  Section map — always visible (GraphEdit)         |           |
|  ● Compose     |   [★ Begin] ──→ [Calm ↻] ──→ [Combat ↻]           | ♪ intro   |
|  ○ Audition    |        └──⇒ fan-out       ↪ [Win sting ⚠]         | ♪ calm1   |
| ------------   |                                                   | ♪ combat1 |
| Sections       +===================== VSplit ======================+ ♪ combat2 |
|  ★ Begin       |  Section: Combat   ↻ loops · ▶ playing   [＋ Add] | ♪ sting   |
|  Calm ↻        |  ♪  Play  [combat1 ▾]           (waits)  ✕ ⋮      |           |
|  ▶ Combat ↻    |  ◇  If  (Threat distance < 10)           ✕ ⋮      | [＋ Add]  |
|  Win sting ⚠   |  |    →  Go to  [Combat ▾]                        | [Rename]  |
| [＋ Add]       |  ✎  Set  [Mood ▾]  =  (value…)           ✕ ⋮      | [Replace] |
| ------------   |  →  Go to  [Calm ▾]                      ✕ ⋮      | [▲][▼][✕] |
| Combat (detail)|                                                   |           |
|  name, rename, +---------------------------------------------------+-----------+
|  delete,       |  Activity  ▸ 12:03:01 section → Combat   [section][sound][…]  |
|  arrives from /|  (collapsible strip; expands to the full filtered feed)       |
|  leads to,     |                                                               |
|  caller inputs |                                                               |
+----------------+---------------------------------------------------------------+
```

**The canvas is a `VSplitContainer`: the section map on top, the selected
section's program below — both always visible.** No swap, no drill-in. The map
is wide-and-shallow (the layered layout runs left to right), the program is a
vertical block stack; each gets its natural aspect. The split ratio is
draggable and persisted as a ratio (RSP-conformant: no pixel offsets, no new
`custom_minimum_size`; everything below is containers + size flags).

Where the eight concerns land:

| Concern (today, in live_mode.gd) | Home in the redesign | Visibility |
|---|---|---|
| Transport (Start/Pause/Resume/Stop, state label, now-playing, jump dropdown) | **Transport bar** across the canvas top: ▶ Play (start/resume) · ⏸ Pause · ⏹ Stop, status, now-playing, Follow toggle, mini level meter. The jump *dropdown* dies — jumping is a map/list affordance (§3) | always |
| Breadcrumbs (`music_nav.gd` trail, rename/delete crumb buttons) | **deleted** — replaced by the selection model (§3); rename/delete move to the Compose inspector + context menus | — |
| Section map (GraphEdit, layout, badges, add-section, empty state) | **top canvas pane**, unchanged in content (chips, ★/↻/⚠ badges, edge colors, live glow) | always |
| Program view (`MusicSectionProgramView` block stack) | **bottom canvas pane**, permanently showing the *selected* section; header names the section + its badges | always |
| Variables (`var_inspector.gd` Game Dials) | **Audition inspector** (left lane) | while Audition workflow active |
| Volume meter (`volume_meter.gd`) | mini meter in the transport bar; the full L/R meter in the Audition inspector | always (mini) |
| Event log (typed/coalesced ItemList + filters) | **Activity strip** at the bottom: a one-line ticker showing the latest event, expandable to the full feed with filters | always (ticker); expanded on demand |
| Follow-live (`_follow_live` flag threading) | one Follow toggle on the transport bar; semantics unchanged (manual selection pins, Start/Stop re-arm) but the flag lives in the selection model, not the screen | always |

**Left-lane conformance.** `uses_left_lane()` returns true; the adapter
declares two `InspectorDef` rows (the same registry mechanics as
`terrain_workspace.gd:_build_inspector_defs()`):

- **Compose** (default) — a `ListDetailInspector` over sections. The list *is*
  today's `%StatesList` sidebar, relocated to where every other workspace puts
  its index: one row per section with ★ start / ↻ loops / ⚠ unreachable / ▶
  playing badges, plus **＋ Add section**. The detail pane for the selected
  section: name + Rename/Delete (today's breadcrumb buttons, re-homed),
  "arrives from / leads to" transition chips (click one to select that
  section), and the caller-inputs card (`inputs_card.gd` naming stays).
- **Audition** — a `WorkflowInspector` hosting the Game Dials
  (`var_inspector.gd` unchanged), the full volume meter, and the activity-feed
  filters. This is the "be the game" hat.

Switching workflow swaps only the inspector — the canvas never changes
(terrain's pattern: Sculpt→Paint doesn't touch the viewport). Playback keeps
running across the switch.

**Tracks = the shell asset dock.** `uses_asset_dock()` returns true and the
bank panel mounts there (the right dock terrain/object already use). Tracks
are the workspace's asset palette: always visible in every workflow, always
draggable onto the program ("drop a track right where it should play" keeps
working from any workflow). Import/rename/reorder/replace/delete and per-row
preview carry over from `bank_mode.gd` unchanged in behavior.

**Empty state** (no project open): the existing centered "＋ New music program
/ or use Open" prompt fills the canvas area; the inspectors show the shell's
standard empty-state panel.

#### 3. Navigation model

The machine is flat; the navigation is flat:

- **One selection: the selected section.** Clicking a map node, a list row, a
  "leads to" chip in the inspector, or a *Go to [section ▾]* row's open ▸
  selects it; the program pane, the list highlight, and the map highlight all
  follow the same `music_selection.gd` model (a small RefCounted replacing
  `music_nav.gd`'s location role). There is no "inside" to drill into and no
  trail to climb out of.
- **No breadcrumbs.** The program pane's header is a *name* ("Section: Combat
  · ↻ loops to itself"), not a path. `music_nav.gd`'s history/trail/crumb
  rendering and live_mode's `_rebuild_breadcrumb` / `MAX_CRUMBS` / crumb-button
  block are deleted.
- **Follow playback** auto-selects the section the VM enters (replace, never a
  history push — there is no history); clicking anything pins (toggle off);
  Start/Stop and the toggle re-arm. Same behavior as today's `_follow_live`,
  minus the trail-flooding workarounds.
- **Selection ≠ jump.** Today a single map click *jumps the running VM* and
  flashes a warning when stopped — the click's meaning depends on transport
  state. Redesigned: click always selects (safe, mode-free); **Play from
  here** is the explicit jump — a button in the transport bar + the section
  context menu, enabled while playing (the honest home of today's jump
  dropdown, which duplicated the map).
- Alt+Left/Right browser keys: dropped with the trail (open question 1 keeps
  the door open for plain selection history if missed).

#### 4. Live/Bank resolution

**Dissolve modes entirely.** Today's names are fossils: `MusicLiveMode` is the
whole screen, `MusicBankMode` is a legacy standalone screen instanced WHOLE
inside `live_mode.tscn` and reached by recursive `find_child("Bank")`
(`music_workspace_root.gd`), and the adapter still says "the old Bank / Script
/ Live modes are coexisting docks". Concretely:

- **Live is a transport state, not a place.** The screen becomes
  `music_screen.gd` / `MusicScreen`; "live" survives only as
  PLAYING/PAUSED/STOPPED on the transport bar.
- **Bank is the Tracks palette, not a mode.** `bank_mode.gd` →
  `music_tracks_dock.gd` / `MusicTracksDock`, mounted through the framework's
  asset-dock tier (`set_asset_dock`), killing the find_child reach-through and
  the nested-legacy-screen arrangement.
- **The relationship, made explicit in the layout:** tracks are the material,
  sections consume them by index — so the palette sits permanently beside the
  canvas, and every *Play* row's track dropdown and chip names resolve against
  it. One document, one undo stack (already true:
  `music_editor_document.gd`'s unified history), now visibly one place.
- Copy sweep: no user-visible "mode", "bank", or "live" as nouns; the dock is
  "Tracks", the meter is "Level", playback is "playback".

#### 5. Block editing

The block-stack paradigm stays (per the prior-art warning: the node-graph
*program* editor was built and deleted in PR #67; the block stack is its
replacement and it works). What changes is where nested values get edited:

- **Rows keep in-place commits.** `stmt_row.gd`'s live sentence — track /
  section / dial dropdowns that commit a canonical replacement line on pick —
  is already the right model ("EDITING IS THE ROW") and is untouched.
- **The floating expression popover is replaced by an inline expression
  band.** Today a condition/value chip opens `MusicExprPopover`, a
  manually-positioned floating panel (`z_index`, `reposition()` on scroll,
  clamp math, 340 px minimum) with hairy survival machinery
  (`_resolve_popover_after_render`, `_apply_callable_for`,
  `_find_row_by_key` re-anchoring after every document re-render). Redesigned:
  clicking the chip **expands an editor band directly under that row, inside
  the stack** — same two tabs (Build = `expr_row.gd` structured editor with the
  live ✓/✗ sentence preview; Type it = canonical text with validation), same
  explicit Apply/Cancel (expressions need validation; atomic picks don't).
  The band is an ordinary container child: it reflows with the stack
  (responsive-safe, no floating geometry), and it survives re-renders the way
  fold state already does — keyed by `{ordinal, slot, branch_key}` like
  `_unfolded`, re-opened after `show_section` rebuilds. One band open at a
  time, opening elsewhere moves it (today's popover rule, kept).
- **If / choose-by-value blocks** (`stmt_if_block.gd`, `stmt_switch_block.gd`)
  keep their indented lanes, lane ＋ menus, and the regenerate-whole-block
  write path (`MusStmtText.if_block` → `replace_statement`, one ordinal, one
  undo step — a document contract we don't touch). Their condition/selector
  chips open the same inline band, indented into the block.
- **Section-level editing moves to the inspector**, not into rows: rename,
  delete, transitions summary, input naming (§2). Row-level editing stays at
  the row. Nothing edits through a popup except confirmation dialogs.

#### 6. Decomposition plan for MUS-I

`live_mode.gd` becomes a thin composition root plus components, one
shippable slice per move. Starting point updated 2026-07-30: W4-6a (#372)
already cut the 1,951-line screen to a 779-line core plus four mechanical
delegate sections (map_view/authoring_ops/nav_ops/transport_log). Those are
mass relocations — the core still owns the behavior — not this design's
intent-signal components, so every slice below stands; S3/S4's extractions
now start from those helpers rather than from a monolith. Every slice ends with: the 46 doc/VM tests green
**unmodified**, the music screen suite (rewritten per-slice, absorbing the
TST refit — the umbrella doc already routes "the music suite" through MUS-I's
rebuild), the canary, and FULL GUT at the end of the phase. Every slice leaves
the workspace fully usable.

| Slice | Move | New/renamed files | Screen-test impact |
|---|---|---|---|
| S1 — shell conformance + Tracks dock | `uses_left_lane()` → true, `uses_asset_dock()` → true; Bank panel out of `live_mode.tscn` into the asset dock; declare Compose/Audition `InspectorDef`s (Compose hosts the relocated section list; Audition hosts the relocated Game Dials + meter); the screen's private LeftDock dies | `music_tracks_dock.gd` (from `bank_mode.gd`), `ui/inspectors/compose_inspector.gd`, `ui/inspectors/audition_inspector.gd` | `music_bank_mode_test` → `music_tracks_dock_test` (path/name only); `music_workspace_test` updated for the new hooks |
| S2 — unify the canvas | CanvasStack → VSplit (map above, program below, program bound to selection); delete drill-in / chrome-swap / breadcrumb code; introduce `music_selection.gd` (selection + follow pin) | `music_selection.gd`; `music_nav.gd` retired | `music_nav_test` retired with it; new `music_selection_test`; `music_live_mode_test` sheds its swap/crumb cases |
| S3 — transport extraction | Transport bar component owns buttons, status label + warning flash, now-playing, Follow toggle, mini meter; Play folds start/resume | `ui/transport_bar.gd` | transport cases move to `music_transport_test` |
| S4 — map panel extraction | GraphEdit build/layout/fit/highlight/context-menu/add-section/empty-state into a component emitting `section_selected` / `play_from_here` / rename/delete/add intents | `ui/section_map_panel.gd` | `music_live_map_test` → `music_section_map_test` |
| S5 — activity feed extraction | De-spam/idle/coalescing rules into a testable model; the strip widget renders it (ticker + expanded feed + filters) | `music_event_feed.gd` (RefCounted), `ui/activity_strip.gd` | log cases leave `music_live_mode_test` for `music_event_feed_test` |
| S6 — program pane + inline band | Wrap `MusicSectionProgramView` with the section header; `expr_popover.gd` → inline `ui/expr_band.gd`; delete the re-anchor machinery; keyed reopen after re-render | `ui/program_pane.gd`, `ui/expr_band.gd` | `music_expr_popover_test` → `music_expr_band_test` (same assertions, new owner) |
| S7 — inspector depth | Compose becomes the full `ListDetailInspector` (detail: rename/delete/transitions/inputs); Audition binds feed filters; selection sync across list/map/pane | inspectors from S1 grow | new `music_compose_inspector_test` |
| S8 — root diet + sweep | `live_mode.gd/tscn` → `music_screen.gd/tscn` (composition root only); state→section copy sweep; README rewrite; screenshot re-baseline; MUS-G (F3 wiring) rides this tail | renames | suite-wide path sweep + GUT silent-drop check |

Ordering rationale: S1 first because it is pure relocation (framework tiers
absorb existing panels — lowest risk, immediate conformance win); S2 is the
one behavioral change (the swap dies) and everything after it is extraction
along seams that already exist as signal boundaries in the code.

#### 7. Constraint compliance table

| Signed constraint | How the design satisfies it |
|---|---|
| Document/VM layer untouched; 46 tests green unmodified | Every change is under `modtools/music/ui/`, the adapter, or new UI files. The document's write API (`insert_statement`, `replace_statement`, `add_section`, …) is consumed as-is; `music_document_test` + `music_document_move_test` + `music_authoring_test` (19+9+18=46) are never edited and gate every slice |
| Shell left-lane conformance (workflow picker + inspector) | `uses_left_lane()` true; two `InspectorDef` rows (Compose, Audition) through the standard `_build_inspector_defs()` / `build_workflow_inspector()` path; the section index lives in the lane like every other workspace's list |
| No modal map/section swap | The canvas is an explicit VSplit; map and selected-section program are simultaneously visible at all times; no visibility toggling between them anywhere |
| Navigation honest about the flat machine | Breadcrumb trail deleted; a single selection over an always-visible flat index (list + map); the program header is a name, never a path; follow is selection-replace, not history |
| Live/Bank mode resolution | Modes dissolved: Bank → Tracks asset dock (framework tier, no find_child seam), Live → transport state; classes/files renamed; copy sweep removes the nouns |
| Responsive-first | Containers + size flags throughout; split ratios persisted as ratios; the floating popover (fixed 340 px, manual clamp math) becomes an in-flow band; no new `custom_minimum_size` sites; adopts RSP-1 policy when it lands |
| No node graph (prior art) | The block stack remains the only program-editing surface; the GraphEdit stays what it is today — the *between-sections* map (the paradigm PR #67 shipped), and this design adds no graph-based program authoring |

#### 8. Open questions for the maintainer gate

1. **Selection history:** with drill-in gone, Alt+Left/Right back/forward
   becomes plain "previous selection". Drop it entirely (lean: yes — the flat
   index makes it near-redundant), or keep a two-button history?
2. **Workflow split:** Compose / Audition as proposed, or a single-pane
   inspector with no picker (also framework-conformant — the Sound/Fonts
   pattern)? The two-hat split is the recommendation; it keeps the dials from
   crowding the section detail.
3. **Tracks placement:** right asset dock (proposed; drag-to-program available
   from every workflow) vs a third "Tracks" workflow in the left lane
   (narrower screen, but drag requires that workflow active)?
4. **Click-to-jump retirement:** OK to change the running-playback map click
   from "jump the VM" to "select", with **Play from here** as the explicit
   jump? This removes the transport-dependent click meaning but changes a
   shipped behavior.
5. **Map-edge authoring:** should MUS-I add "drag from a section's output pin
   to create a *Go to*" on the map, or is transition authoring staying in the
   program rows only? (New authoring surface on a graph — flagging it rather
   than assuming, given the prior-art warning.)
6. **Activity strip default:** collapsed ticker (proposed) vs the always-open
   list of today?
7. **Test retirement accounting:** `music_nav_test.gd` (12 tests) dies with
   the trail; the TST ratchet treats the music suite as riding MUS-I's
   rebuild — confirm the count reduction is acceptable under that rule.
8. **Copy unification on "section":** confirm the state→section sweep (button
   labels, tooltips, README) — it touches strings the existing screenshot
   baseline shows.

## Per-workspace phases

### Terrain (bar-setter; verification only)

- TER-1: historical bar audit — confirm all four axes against this doc's
  definitions, wire the former staged-terrain launcher entry ("open the
  exported terrain's mission in game"), and record terrain as the E/G
  exemplar. No new capability. Gate: existing
  terrain suites; driver overview shot. **DONE 2026-07-12.** Audit: R ✓
  (loose `.trn` projects and imported game assets, plus VFS/PFF opens via
  `open_trn`'s resource-root branch); W ✓ (F4 practiced: from-scratch
  TrnGen-parity bake + the roundtrip/import-export suites); E ✓ (the editor
  viewport IS the ported terrain renderer — the E exemplar); G ✓ (after an
  explicit save, F5/F6 run the standalone game over the mounted loose assets).
  The historical terrain staging launcher described below was superseded by
  the saved-loose-assets boundary in ADR 0025: Run no longer exports, copies,
  or stages terrain data. One correction to the phase's wording remains:
  terrain export bakes the terrain data set (`.trn`/`.cpt`/`.til`/maps), not a
  mission dir — there is no "exported terrain's mission" to boot directly.
  The retired launcher also introduced a per-workspace
  `EditorWorkspace.get_game_launch_note(launch_dir)` -> `GameLaunchNote`
  staging-note seam, consumed by the then-launcher's tooltip and post-launch
  status; that seam was deleted with the launcher (ADR 0025 / PR #376) —
  today's F5/F6 runs carry no per-workspace launch notes.

### Environment

- ENV-1: bar audit (E already exemplary — direct runtime-node reuse). Verify G
  (authored `.env` override → standalone launch; the runtime loads the same
  saved file). Gate:
  environment suite. **DONE 2026-07-12.** Audit: R ✓ (loose opens plus
  VFS/PFF entries via `open_env_from_resource_root`); W ✓ (from-scratch save
  through libs/env `EnvFile.save_to_path`, roundtrip-pinned by
  env_file_test); E ✓ re-verified — the exemplar (editor edits drive the SAME
  `NovaEnvironment`/`NovaSky`/`NovaWater` runtime nodes;
  editor-runtime-parity.md: water parameterized, not forked); G ✓ (after an
  explicit save, F5/F6 launch the standalone game against the mounted loose
  root, and the mission resolves `<mission env ref>.env` from that same disk
  state). Unsaved popup edits are warned about and excluded exactly like
  unsaved edits in the active workspace. The former per-workspace launch note
  and staging language is retired by ADR 0025.

### Object

- OBJ-1: **in-world preview** — adopt F1: "Preview on terrain" toggle places
  the object at a grounded point in the world context with env+fog, next to
  the existing isolated preview. Gate: new object preview test + visual shot.
- OBJ-2: **LOD-by-distance simulation** — RE-gated on R3 (the original LOD
  select rule). After witness: a distance slider/camera dolly in the world
  context that switches render LODs the way the engine does, with the
  thresholds labeled in artist terms ("draw distance"). Gate: test pinning
  the witnessed thresholds.
- OBJ-3: material preview truthfulness audit vs the canonical 45-entry
  shader-tag table (no .fx parsing); fix any tag whose preview diverges from
  the ported fixed-function look. Gate: per-tag smoke via the parity harness.
- OBJ-4: inspector test uplift — six workflows currently ride 4 test files;
  add per-workflow coverage (materials/lights/part-anims/LODs mutation +
  undo + rejection paths) to match the mission/mnu ratio. Gate: full GUT.
- OBJ-5: wire F3 (see the object in a mission in the real game).

### Mission

- MIS-1: **heading semantics** — RE-gated on R1; then expose heading
  authoring honestly (today it is UNKNOWN — the one field we write without a
  witnessed meaning). Gate: corpus roundtrip stays 117/117 + a new
  heading-pinning test against witnessed behavior.
- MIS-2: **friendly animation names** — RE-gated on R2 (ANIMNUM table); the
  scripting UI then shows names, storing the witnessed numbers. Gate:
  scripting inspector tests.
- MIS-3: scripting depth audit — event/param coverage vs the 218-file WAC/BMS
  corpus; close typing gaps found (each new param type cited). Gate: corpus
  parse sweep, no regressions.
- MIS-4 (= F5): controller decomposition — DONE before any MIS work
  (#204 + #373; see F5).

### Credits

- CRE-1: bar audit — E is already real (`NovaCreditsPlayer`); add a
  play/pause/scrub transport over the hosted player so the roll (not just the
  layout) previews in-editor. Verify the player's cadence citations while
  there (R5 only if uncited). Gate: credits suites (20 files).
  **DONE 2026-07-12.** Play/pause/stop + speed already drove the hosted
  player (since the workspace landed, #22); the missing transport piece was
  direct scrubbing — a scrub bar under the preview now drives the player's
  scroll offset over the roll's full extent and follows playback, wheel, and
  card-selection seeks. Cadence-citation verdict: **UNCITED — R5 is OPEN.**
  The SCROLL_RATE format side is witnessed
  ([orig: marquee_load_credits_from_ini @ 0x65c5a0]), but
  `NovaCreditsPlayer::_process_scroll` converts the per-frame rate to
  per-second with an assumed 60 fps cadence (`* 60.0f`) and the ~F overlay
  fade zone is a bare 50 px (`kFadeZonePixels`) — neither carries a witness
  of `CMarqueeWnd`'s update/render cadence (note: the engine tick elsewhere
  is 62 Hz, so the 60 is doubly suspect). Marked in the source as
  do-not-cite-without-a-grill; not invented here.
- CRE-2: wire F3 (authored `nlist.kda` → game credits screen).

### Fonts

- FNT-1: **engine text sample** — adopt F2: a type-a-line panel rendered
  through the game's draw path with the edited font (hi/lo variants where the
  format carries them), beside the glyph canvas (which stays — it is a paint
  surface, not a preview claim). Gate: fnt suite + widget test.
- FNT-2: format completeness audit — every `.fnt` field the runtime consumes
  is visible/editable or pinned as intentionally fixed. Gate: roundtrip test
  extended to audited fields. (The rasterizer's format facts move engine-side
  under the umbrella's ENG-4.)
- FNT-3: wire F3.

### Strings

- STR-1: **in-context render** — adopt F2: selected entry rendered through
  the engine path with a chooseable font, closing the "how will this read
  in-game" gap. Gate: strings suite.
- STR-2: encoding audit — pin the retail code-page behavior end to end
  (table bytes → glyphs) with a test; document any exclusion. Gate: new test.
- STR-3: wire F3 (menus consuming the authored table).

### Sound

- SND-1: audition completeness audit — every LWF field the runtime consumes
  (`libs/lwf` + audio RE docs) is editable and audible in the workspace;
  close gaps found. Gate: sound suites (12 files).
- SND-2: **at-distance audition** — RE-gated on R7 if the attenuation model
  lacks witness; then an F1-hosted emitter auditioned from a movable listener
  with the engine's falloff. Gate: test pinning the witnessed curve.
- SND-3: wire F3.

### Music

The redesign (MUS-D/MUS-I above) IS this workspace's maturity work; its
R/W/E axes are already at bar. MUS-G: wire F3 (authored gamemus/menumus
override → menu/game music) rides MUS-I's tail.

### Menus

- MNU-1: **flow run mode** — run the runtime `NovaMenuShell` (nova_menu_shell.gd's
  node: the same `NovaMnuMenu` with `edit_mode` off, fully interactive) over
  the authored document in a sandboxed "Run" tab: navigation, back stack,
  window show/hide, per-screen MUSICVAR all execute for real; the shell-policy
  verbs (cross-file jump, quit, gameplay launch) route to an editor
  interceptor that logs/navigates instead of quitting or launching. Gate: new
  flow-run test + mnu suites (32 files).
- MNU-2: action-coverage audit vs the witnessed action set in the menu-UI RE
  docs (R6 for anything found unwitnessed). Gate: corpus open sweep over the
  17 shipped .mnu/.mns.
- MNU-3: wire F3 (real game boots the authored menu set, no sandbox).

### HUD (the flagship: R-only today, full bring-up)

- HUD-1: **format grill + writer** — grill `NovaHudPos`'s read against the
  retail binary layout (every field accounted for, R4 for consumption
  semantics), then `libs` writer + `set_*`/`write` bindings, byte-parity
  roundtrip vs retail `hudpos.def` (F4 convention; writer from scratch).
  Gate: new native tests + roundtrip corpus check.
- HUD-2: **authoring UI** — the read-only inspector becomes an editor:
  positions/rects (drag handles on the canvas), colors, fonts, stance
  anchors, icons — InspectorForms rows, snapshot undo (the B1 tier), dirty +
  save wired through the standard document hooks. The layout canvas stays as
  the authoring surface. Gate: new `hud_editor_undo_test` + hud suite.
- HUD-3: **live engine preview** — replace the truth-claim of the hand-drawn
  preview: run the game's HUD draw (GameHud path from the runtime bring-up)
  fed by a sample-state source (health/ammo cycling, stance switching,
  static-frame animation), so the preview is the engine rendering the edited
  layout. RE-gated on R4 for any element whose draw is still unwitnessed
  (the objective-line/GameText gap is known open). Gate: visual driver shot
  goes live-rendered + hud suite.
- HUD-4: wire F3 (authored hudpos.def → real game HUD).

### Avatars (post-AVA phases)

- AVT-1: bar audit against this doc's definitions once merged (the branch
  work predates the bar); fill the matrix row honestly and file gaps as
  phases. Expected early items: writer parity per F4, F3 wiring, preview
  truthfulness vs the runtime player.mnu flow.

## REF — reference/link infrastructure gaps (umbrella Wave 3)

The index and jump surfaces are solid (RefGraph + extractors for
.env/.kda/.3di/.bms/.mis/.mnu/items.def/cbin; ReferenceStrip + link
widgets). Close the extractor gaps via the documented plug-in point
(refs.cpp `extract()`/`can_extract()`, the `extract_items_def` pattern):

- REF-1: `weapon.def` extractor (graphic/husk/sound edges like items.def).
- REF-2: `Avatars.def` extractor (post-AVA).
- REF-3: `.wac` extractor (script → referenced resources).
- REF-4: source coverage — `.trn`, `.sbf`/`.lwf`, strings tables as edge
  SOURCES so "Used by" answers are complete.
- Gate per slice: refs ctests + the reference_index GUT suite; no editor
  changes needed (the strips consume whatever edges exist).

## REQ — required-resources conveniences (umbrella Wave 3)

Over the umbrella's ENG-6 manifest (the witnessed boot-required resource
set): ONED surfaces missing-required diagnostics (browser/status), and the
"new game" scaffold seed — create-from-minimal-set wiring the per-workspace
`new_current()` hooks. Defines the editor half of "what a person starts
with to make a new game"; the Game workspace itself stays out of scope.

## RE ledger (all engine-research/grill-ida routed, landing via re-doc)

- R1 mission placed-object heading semantics (blocks MIS-1)
- R2 ANIMNUM → animation-name table (blocks MIS-2)
- R3 3di LOD select rule — distance thresholds/CDEP interaction (blocks OBJ-2)
- R4 hudpos.def consumption semantics — full-field draw behavior incl. the
  objective line/GameText (blocks HUD-1 completeness + HUD-3)
- R5 credits roll cadence citations — CRE-1 verdict 2026-07-12: UNCITED, so
  R5 is OPEN (the 60 fps frame→second conversion in
  `NovaCreditsPlayer::_process_scroll` and the 50 px ~F fade zone; the
  witnessed side stops at the SCROLL_RATE parse @ 0x65c5a0)
- R6 menu action verbs beyond current witness (only if MNU-2 finds gaps)
- R7 sound attenuation/falloff model (blocks SND-2 if unwitnessed)
- R8 boot-required resource enumeration — owned by the umbrella's ENG-6;
  listed here because REQ consumes it.

Rule: UI is never built ahead of the witness — a grill that invalidates an
assumption re-scopes the phase before code lands (the faithful-port rule).

## Sequencing (keyed to the umbrella's waves)

- **Umbrella Wave 1:** AVA (landed 2026-07-04), then F1–F5 (F5 before any
  MIS phase — satisfied: F5 landed via #204 + #373).
- **Umbrella Wave 2:** the no-RE adoptions — OBJ-1/3/4, FNT-1/2, STR-1/2,
  CRE-1, MNU-1, SND-1, TER-1, ENV-1, every F3 wiring — plus TST refits,
  RSP-1..3, and MUS-D (maintainer gate at its end). R1–R7 grills run
  throughout (freeze-exempt).
- **Umbrella Wave 3:** the RE-gated bring-ups — HUD-1..4, MIS-1..3, OBJ-2,
  SND-2, MNU-2 — plus MUS-I, REF-1..4, REQ, AVT-1.
- **Umbrella Wave 4:** the matrix re-audit over twelve workspaces (every
  cell ✓ or a tracked decision), the driver's new truth shots
  (object-in-world, HUD live, menu flow run, avatars), consolidated
  oned-run pass, status recorded here.

## Execution conventions

The umbrella's conventions apply (one green slice = one commit; FULL GUT at
F5, post-HUD, post-MUS-I, and wave boundaries; canary
`terrain_editor_workstation_test.gd`; GUT silent-drop protocol on every
class_name/path move; native work rebuilds the GDExtension with the
stale-DLL check + scoped ctest + dumpbin on C-ABI touches; packaged boot
smokes; UI copy artist-facing; ADR 0003 writers; [orig] citations;
divergences as D-entries via re-doc). New work is built to ADRs 0016/0017/
0018 from the start — engine facts via public APIs, typed records, no
private-poking tests.

## Top risks

1. **RE unknowns invalidate authoring assumptions** (heading, hudpos
   semantics): grills run first; UI phases are re-scoped, not patched, when
   the witness disagrees.
2. **F1 extraction entangles mission seams** the way A8's popover block did:
   extract with seams intact, mechanical first, redesign never mid-move.
3. **F5 churn** — retired: F5 landed purely mechanically (#204 + #373)
   before any MIS phase; the risk did not materialize.
4. **Flow-run sandbox escape** (MNU-1): the interceptor must catch every
   shell-policy verb (quit/launch/cross-file) or a menu action closes the
   editor; the flow-run test enumerates the verbs.
5. **Game-session scope creep** (F3): the shell owns exactly one standalone
   child, three lifecycle gestures (F5/F6/F8), and the stable loopback debug
   proxy. It does not run gameplay, stage assets, or mirror runtime state.
6. **Avatars drift** (AVA): the branch predates the editor-layer program AND
   this program's standards — expect adapter-contract and framework deltas;
   the merge train budgets a conformance pass, not a blind rebase.
7. **Music redesign creep** (MUS-D/I): bounded spike, signed constraints,
   maintainer gate, shippable slices (also tracked at the umbrella level).
