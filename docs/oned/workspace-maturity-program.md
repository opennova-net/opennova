# ONED workspace maturity program

Successor program to [editor-layer-program.md](editor-layer-program.md)
(complete 2026-07-04: app root, shell decomposition, undo everywhere, global
shortcuts, shared widgets/theme/vocabulary). That program made the eleven
workspaces *uniform*; this one makes them *equally deep*: every workspace
reaches the same capability bar on top of that foundation, so the editor can
keep growing without a weakest-link workspace.

Authored 2026-07-04 against the post-program tree (`hygiene-2026-07` head
`f17be565`). Facts below (which preview hosts which engine node, which
writers exist) were re-verified against that tree — anything marked
*re-grep at execution* drifts too easily to pin.

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
  nodes/code the game runs, not an editor-drawn approximation. (The terrain
  viewport and the credits `NovaCreditsPlayer` are the exemplars; the HUD's
  hand-drawn `_draw()` canvas is the anti-pattern.)
- **G — Game loop.** One gesture takes the authored data into the real game:
  play-in-editor where the workspace has a world (mission), and the
  override launcher (F3) everywhere else. Authoring that cannot be seen in
  the running game is not done.

## Current matrix (verified 2026-07-04)

| Workspace | R | W | E | G | Weakest axis today |
|---|---|---|---|---|---|
| Terrain | ✓ | ✓ | ✓ (viewport is the renderer) | partial (via mission PIE) | G polish |
| Environment | ✓ | ✓ | ✓ (runtime nodes shared; water unified) | partial | G polish |
| Object | ✓ (v8/v9; LW v10 on branch) | ✓ (.3dp + .3di export) | partial (isolated preview; env-lit but no world context, no LOD-by-distance) | — | E |
| Mission | ✓ | ✓ | ✓ (PIE runs the real runtime) | ✓ | data semantics (heading, ANIMNUM) |
| Credits | ✓ | ✓ | ✓ (`NovaCreditsPlayer` hosted in the editor) | — | G |
| Fonts | ✓ | ✓ | partial (glyph canvas is editor-drawn; fine for painting, no engine text sample) | — | E |
| Strings | ✓ | ✓ | partial (no in-context render) | — | E (minor by format) |
| Sound | ✓ | ✓ (`write_lwf`) | ✓ (audition rides the runtime set→member→wav path) | — | G |
| Music | ✓ (headerless form excluded by pinned decision, D-SCR-1/2) | ✓ | ✓ (live director runs the VM) | — | G |
| Menus | ✓ (.mnu/.mns) | ✓ | ✓ (canvas hosts live `NovaMnuMenu`, edit_mode on) | partial (no flow run; actions authored blind) | G |
| HUD | ✓ (`NovaHudPos`, read-only getters) | — | — (hand-drawn `HudLayoutPreview._draw()`) | — | everything but R |

Two corrections to earlier working assumptions, now verified: environment/
water unification is DONE (editor-runtime-parity.md: "water is fully unified
(parameterized, not forked)"), and credits' preview already hosts the real
`NovaCreditsPlayer` (credits_editor.tscn) — both were previously believed
gaps. The headerless MUS cipher stays out of scope by the recorded decision
in docs/audio/mus-sbf-re.md (`vfs_mus_decode_test` pins the pass-through);
it is a bar-satisfying exclusion, not a gap.

## Foundation phases (Wave 0)

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

### F4 — Writer-parity convention (doc-only)

Record in this file + libs/CLAUDE.md pointers: every W phase ships (a) writer
from scratch, (b) roundtrip test, (c) retail-corpus byte sweep where a corpus
exists, (d) a D-entry when output legitimately differs. This is already
practiced (terrain, 3di, lwf, mnu, mission); writing it down makes it the
stated gate for the new writers (hudpos).

### F5 — MissionController decomposition

`mission/mission_controller.gd` is 4,033 lines (largest file under any
workspace; backlogged since #179). Decompose along its existing seams
(placement/grounding, sim driver, environment, selection/edit ops, IO) the
way #178 split the inspector — mechanical moves, tests remapped, no behavior
change. This is a foundation item because Mission's Wave-2 work (heading,
scripting depth) must land in reviewable files. Gate: mission suites (30
files) + canary + full GUT keystone.

## Per-workspace phases

### Terrain (bar-setter; verification only)

- TER-1: bar audit — confirm all four axes against this doc's definitions,
  wire the F3 launcher entry ("open the exported terrain's mission in game"),
  and record terrain as the E/G exemplar. No new capability. Gate: existing
  terrain suites; driver overview shot.

### Environment

- ENV-1: bar audit (E already exemplary — direct runtime-node reuse). Wire F3
  (authored `.env` override → launch; the runtime loads the same file). Gate:
  environment suite.

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
- CRE-2: wire F3 (authored `nlist.kda` → game credits screen).

### Fonts

- FNT-1: **engine text sample** — adopt F2: a type-a-line panel rendered
  through the game's draw path with the edited font (hi/lo variants where the
  format carries them), beside the glyph canvas (which stays — it is a paint
  surface, not a preview claim). Gate: fnt suite + widget test.
- FNT-2: format completeness audit — every `.fnt` field the runtime consumes
  is visible/editable or pinned as intentionally fixed. Gate: roundtrip test
  extended to audited fields.
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

- MUS-1: bar audit — R exclusion already pinned (headerless), E already the
  exemplar (live director = the VM). Verify play/edit parity gates still
  cover the full authoring surface post-editor-layer program. Gate: the 46
  music tests.
- MUS-2: wire F3 (authored gamemus/menumus override → menu/game music).

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

## RE ledger (all engine-research/grill-ida routed, landing via re-doc)

- R1 mission placed-object heading semantics (blocks MIS-1)
- R2 ANIMNUM → animation-name table (blocks MIS-2)
- R3 3di LOD select rule — distance thresholds/CDEP interaction (blocks OBJ-2)
- R4 hudpos.def consumption semantics — full-field draw behavior incl. the
  objective line/GameText (blocks HUD-1 completeness + HUD-3)
- R5 credits roll cadence citations (only if CRE-1 finds them uncited)
- R6 menu action verbs beyond current witness (only if MNU-2 finds gaps)
- R7 sound attenuation/falloff model (blocks SND-2 if unwitnessed)

Rule: UI is never built ahead of the witness — a grill that invalidates an
assumption re-scopes the phase before code lands (the faithful-port rule).

## Sequencing

- **Wave 0 — foundation:** F1, F2, F3, F4, F5. (F3's packaged-product boot
  smoke already landed with the `run/main_scene.modtools` fix.)
- **Wave 1 — no-RE adoptions:** OBJ-1/3/4, FNT-1/2, STR-1/2, CRE-1, MNU-1,
  SND-1, MUS-1, TER-1, ENV-1, and every F3 wiring (TER through MUS).
- **Wave 2 — RE-gated bring-ups:** HUD-1..4, MIS-1..3, OBJ-2, SND-2, MNU-2 —
  each preceded by its R-item; grills can start during Wave 1.
- **Wave 3 — equalize + end gate:** matrix re-audit (every cell ✓ or a
  tracked decision), FULL GUT, the screenshot driver grows the new truth
  shots (object-in-world, HUD live, menu flow run), consolidated oned-run
  pass, program status recorded here.

## Execution conventions

Unchanged from the editor-layer program, plus the new guards:

- One slice = one commit, independently green and revertable; scoped suites
  per slice; FULL GUT at F5, after the HUD block, and program end. Canary:
  `terrain_editor_workstation_test.gd`. GUT silent-drop protocol on every
  class_name/path move.
- Native work (HUD-1 writer, any binding): `scripts/build_godot.sh`,
  stale-DLL check, `--import`, scoped ctest, dumpbin export-identity where
  the C ABI is touched.
- Every packaged product must pass the boot smoke (now in
  `scripts/package_godot_windows.ps1`); artifact testing happens on CI
  packages, not dev trees.
- UI copy stays artist-facing; engine terms stay in code/docs.
- Parity rules: ADR 0003 (no raw passthrough), [orig] citations inline,
  divergences as D-entries via re-doc.

## Top risks

1. **RE unknowns invalidate authoring assumptions** (heading, hudpos
   semantics): grills run first; UI phases are re-scoped, not patched, when
   the witness disagrees.
2. **F1 extraction entangles mission seams** the way A8's popover block did:
   extract with seams intact, mechanical first, redesign never mid-move.
3. **F5 churn**: 4k lines moving while Wave-1 work proceeds elsewhere — land
   F5 before any MIS phase, and keep it purely mechanical.
4. **Flow-run sandbox escape** (MNU-1): the interceptor must catch every
   host-policy verb (quit/launch/cross-file) or a menu action closes the
   editor; the flow-run test enumerates the verbs.
5. **Scope creep toward a game-launcher IDE** (F3): the launcher is one
   gesture + docs, not a session manager; anything more is a new program.
