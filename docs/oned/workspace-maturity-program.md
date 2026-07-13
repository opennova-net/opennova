# ONED workspace maturity program (the maturity program's ONED track)

**This document is the ONED track detail of the
[maturity program](../maturity-program.md)** — the umbrella owns waves,
freeze policy, cross-track ordering, and enforcement; this doc owns the
editor-side phases. Standing rules this track builds under: ADR 0015 (two
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
- **G — Game loop.** One gesture takes the authored data into the real game:
  play-in-editor where the workspace has a world (mission), and the
  override launcher (F3) everywhere else. Authoring that cannot be seen in
  the running game is not done.

## Current matrix (verified 2026-07-04)

| Workspace | R | W | E | G | Weakest axis today |
|---|---|---|---|---|---|
| Terrain | ✓ | ✓ | ✓ (viewport is the renderer) | ✓ (mission PIE + the F3 launcher's terrain staging note) | at bar — the E/G exemplar (TER-1, 2026-07-12) |
| Environment | ✓ | ✓ | ✓ (runtime nodes shared; water unified) | ✓ (the F3 launcher carries the .env staging note) | at bar (ENV-1, 2026-07-12) |
| Object | ✓ (v8/v9; LW v10 on branch) | ✓ (.3dp + .3di export) | partial (isolated preview; env-lit but no world context, no LOD-by-distance) | — | E |
| Mission | ✓ | ✓ | ✓ (PIE runs the real runtime) | ✓ | data semantics (heading, ANIMNUM) |
| Credits | ✓ | ✓ | ✓ (`NovaCreditsPlayer` hosted in the editor; play/pause/scrub transport, CRE-1) | — | G (F3 wiring = CRE-2) |
| Fonts | ✓ | ✓ | partial (glyph canvas is editor-drawn; fine for painting, no engine text sample) | — | E |
| Strings | ✓ | ✓ | partial (no in-context render) | — | E (minor by format) |
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

### F5 — MissionController decomposition

`mission/mission_controller.gd` is 4,033 lines (largest file under any
workspace; backlogged since #179). Decompose along its existing seams
(placement/grounding, sim driver, environment, selection/edit ops, IO) the
way #178 split the inspector — mechanical moves, tests remapped, no behavior
change. Sequenced before any MIS phase. Gate: mission suites (30 files) +
canary + full GUT keystone.

## TST — public-seam test refits (umbrella Wave 2; ADR 0018)

The suite pokes `_private` members of other objects on ~1,300 lines; the
top offenders get refit slices, by ADR 0018's categories: (A) child-control
reach-ins → public accessors (the `get_workspace_adapter` precedent), (B)
private methods as entry points → public verbs, (C) internal state reads →
public getters for contract state. Order: `mission_inspector_test` (301
lines), `terrain_editor_workstation_test` (152 — the canary, handled with
extra care), `mission_controller_test` (100, after F5), `mnu_canvas_test`
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
verified: a 1,951-line single screen owning eight concerns (transport,
breadcrumbs, section map, program view, variables, volume meter, event log,
follow-live); a modal map↔section canvas swap (you never see both);
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
  (post-RSP policy). Ends at a maintainer gate.
- MUS-I (Wave 3): implementation in shippable slices — decompose the screen
  along its eight concerns, unify the canvas, rework block/expr editing per
  the accepted design. Every slice leaves the workspace usable.
- Gate: the 46 doc/VM tests unmodified-green throughout; new screen tests;
  FULL GUT; visual pass.

## Per-workspace phases

### Terrain (bar-setter; verification only)

- TER-1: bar audit — confirm all four axes against this doc's definitions,
  wire the F3 launcher entry ("open the exported terrain's mission in game"),
  and record terrain as the E/G exemplar. No new capability. Gate: existing
  terrain suites; driver overview shot. **DONE 2026-07-12.** Audit: R ✓
  (loose `.trn` projects and imported game assets, plus VFS/PFF opens via
  `open_trn`'s resource-root branch); W ✓ (F4 practiced: from-scratch
  TrnGen-parity bake + the roundtrip/import-export suites); E ✓ (the editor
  viewport IS the ported terrain renderer — the E exemplar); G ✓ (mission
  PIE runs the real runtime on the terrain, and the See-in-game launcher now
  carries terrain's staging note: exported-into-the-game-folder → "load a
  mission on it", otherwise an honest export-first pointer). One correction
  to the phase's wording: terrain's export bakes the terrain data set
  (`.trn`/`.cpt`/`.til`/maps), not a mission dir — there is no "exported
  terrain's mission" to boot directly, so the launcher entry points at the
  game's mission list over the staged terrain instead. The wiring introduced
  the per-workspace seam every later F3 phase reuses:
  `EditorWorkspace.get_game_launch_note(launch_dir)` returning a typed
  `GameLaunchNote` (staged + artist-facing detail), consumed by
  `ShellGameLaunch` in the tooltip and post-launch status.

### Environment

- ENV-1: bar audit (E already exemplary — direct runtime-node reuse). Wire F3
  (authored `.env` override → launch; the runtime loads the same file). Gate:
  environment suite. **DONE 2026-07-12.** Audit: R ✓ (loose opens plus
  VFS/PFF entries via `open_env_from_resource_root`); W ✓ (from-scratch save
  through libs/env `EnvFile.save_to_path`, roundtrip-pinned by
  env_file_test); E ✓ re-verified — the exemplar (editor edits drive the SAME
  `NovaEnvironment`/`NovaSky`/`NovaWater` runtime nodes;
  editor-runtime-parity.md: water parameterized, not forked); G ✓ (the
  launcher's env note over TER-1's `GameLaunchNote` seam: the launched game
  reads `<mission env ref>.env` from the mounted root, so Save into the
  launch dir IS the staging step — the note stages on a clean save there and
  un-stages on unsaved edits, since the game reads the disk copy). Because
  Environment is a popup workspace (never the active one), the shell routes
  the launcher's note through the open panel: panel open = the environment's
  note, panel closed = the active workspace's. No LaunchPlan change was
  needed: "the launch carries the authored .env" resolves to the
  `/d`-mounted directory carrying it.

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
- MIS-4 (= F5): controller decomposition, sequenced before MIS-1..3 land.

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

- MNU-1: **flow run mode** — host the runtime `NovaMenuHost` (menu_shell.gd's
  node: the same `NovaMnuMenu` with `edit_mode` off, fully interactive) over
  the authored document in a sandboxed "Run" tab: navigation, back stack,
  window show/hide, per-screen MUSICVAR all execute for real; the host-policy
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
  preview: host the game's HUD draw (GameHud path from the runtime bring-up)
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
  MIS phase).
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
3. **F5 churn**: 4k lines moving while Wave-2 work proceeds elsewhere — land
   F5 before any MIS phase, and keep it purely mechanical.
4. **Flow-run sandbox escape** (MNU-1): the interceptor must catch every
   host-policy verb (quit/launch/cross-file) or a menu action closes the
   editor; the flow-run test enumerates the verbs.
5. **Scope creep toward a game-launcher IDE** (F3): the launcher is one
   gesture + docs, not a session manager; anything more is a new program.
6. **Avatars drift** (AVA): the branch predates the editor-layer program AND
   this program's standards — expect adapter-contract and framework deltas;
   the merge train budgets a conformance pass, not a blind rebase.
7. **Music redesign creep** (MUS-D/I): bounded spike, signed constraints,
   maintainer gate, shippable slices (also tracked at the umbrella level).
