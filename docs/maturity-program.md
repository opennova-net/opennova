# OpenNova maturity program

Before further reimplementation: rearchitect where it pays, refactor the
rest, and institutionalize the codebase design. Started 2026-07-04, after
the editor-layer program (docs/oned/editor-layer-program.md, complete) and
grounded in a three-way exploration of the tree (net/libs topology, the
ONED/engine boundary, products/standards) whose findings are folded into the
track descriptions below.

Two things hold at every commit of this program:

- **The wire-compat invariant** — our client joins retail servers, retail
  clients join ours (ADR 0010's standing amendment). Every net-touching
  slice proves it (see the NET-0 gate tiers).
- **The stop-anywhere property** — one slice = one independently-green,
  revertable commit; the program can halt at any boundary having paid down
  real debt.

The architecture constraint the whole program serves: **ONED is a detachable
layer over public engine APIs, and the engine never depends on the editor**
(ADR 0016). Products: exactly two shipped exes; the server is a serve MODE
of the game (ADR 0015).

## Status

| Field | Value |
|---|---|
| State | Wave 1 in flight (Wave 0 merged 2026-07-04) |
| Current wave | 1 — boundary moves + ONED foundations |
| Freeze | ON for non-PAR work: new reimplementation features wait for their foundation phase; IDA research/grills are exempt and continue. **PAR burn-down slices are exempt** (maintainer 2026-07-05, [ADR 0022](adr/0022-divergence-burn-down.md)). |
| Detail docs | ONED track: [docs/oned/workspace-maturity-program.md](oned/workspace-maturity-program.md) (all other tracks live here) |
| Decision ADRs | [0015](adr/0015-two-products-serve-mode.md) products/serve-mode, [0016](adr/0016-engine-editor-boundary.md) engine/editor boundary, [0017](adr/0017-typed-records-named-constants.md) records/constants, [0018](adr/0018-public-api-testability.md) testability, [0022](adr/0022-divergence-burn-down.md) divergence burn-down; boundary ADRs (npwire, world seam, lib topology, responsive shell) minted in their tracks at decision time |

Landed slices (hash per slice, newest first):

| Slice | Commit | Note |
|---|---|---|
| ONED F4 writer-parity convention | 29b5ded8 | the four-part W gate recorded in the track doc; libs/CLAUDE.md points at it; binds hudpos (HUD-1) and avatars (AVT-1) forward |
| PAR-0 ledger train | 4948c540 | divergence-ledger.md + ADR 0022 + dashboard wiring; stable D-catalogs minted in the four prose-only records (97f64a97) |
| PAR: D-ITEMDEF-1 closed | faec4b3e | `item_type_from_string` witnessed mapping [orig: ItemDef_ParseProperty @ 0x49eb00]; itemdef-re verdict → MATCHING; first ledger row to zero |
| NET-3 reclassifications + doc sweep | d47932dc | `.agents/network.md` → npruntime ROADMAP redirect, nw_server README (dev/golden-harness), repo-map one-liners; stale-doc sweep 40504652; no-internal-back-compat convention 98f924af |
| ENG-1 env parity vectors | 27cff422 | 137 vectors dumped once from the cited GDScript port (`env_parity_vectors_test.gd`, env-gated dump mode); the ENG-2 port's pre/post harness |
| ONED F1 WorldContextPreview | 9c666c7a | in-world context service extracted from TerrainEditor with seams intact; consumers OBJ-1/SND-2 |
| ONED-A avatars merge train | 18791098 | the twelfth workspace (Avatars) merged from `playerinfo-runtime`; avatars ADR renumbered 0013→0021; eleven→twelve sweep; ratchet `test_private_pokes` baseline 1319→1371 — maintainer-approved bump for the branch's two pre-ratchet white-box files (`player_info_menu_seam_test.gd` 26, `avatar_preview_test.gd` 26); ONED-TST claws it back |
| LIBS-1 world→terrain seam | bc8c920a | libs/terrain_query query leaf + the permanent forbidden-edge check (link_graph_check.py); ADR 0020 |
| NET-2 npwire extraction | 46cd0ac4 | wire+replay+framing legs → libs/npwire; ADR 0019; NET-0/STD-1 rode Wave 0 |
| GOV-1/2/3 bootstrap docs | 4274cfdf | umbrella + vocabulary + ADRs 0015–0018 |

## Tracks

Track codes prefix slice IDs and PR titles. Sizes are S/M/L feel, not time.

### GOV — governance, vocabulary, program docs

- **GOV-1** (M) this umbrella; docs/README.md index rows.
- **GOV-2** (S) CONTEXT.md vocabulary batch 1: Serve mode; In-match vs
  Matchmaking; wire codec / net runtime / net seam; Product (+ Title
  forward-note); Promote disambiguation; Required resources. (Avatars/
  twelve-workspace entries ride ONED-A.)
- **GOV-3** (M) the four settled-decision ADRs (0015–0018).
- **GOV-4** (S per wave) wave close-outs: ratchet-counter review, stale-doc
  sweep, status table update. (.agents/network.md rewrite rides NET-3.)

### NET — in-match net boundary (owns the wire-compat invariant)

Background finding: the in-match *runtime* already lives outside the
NovaWorld lib (`libs/netsim` seam glue, `libs/npruntime` 62 Hz runtime), but
the in-match *codec* (wire leg: `ingame_decode/encode`,
`ingame_message_catalog.h`, `replication_model.h`) and the replay leg sat
under `libs/novaworld` — a matchmaking name for game-protocol code — until
NET-2 moved them to `libs/npwire` (ADR 0019).
`.agents/network.md` is stale (names classes deleted by the npruntime
rebuild). `apps/nw_server` vs `apps/novaworld_server` is the deliberate
ADR-0013 matchmaking/in-match split, not duplication — the `nw_*` naming is
what confuses.

- **NET-0** (M) **golden-gate hardening, before anything moves.** Two tiers
  (retail captures are never committed — docs/asset-gated-tests.md):
  - *Tier 1, default CI, cannot skip:* codec identity vectors — a synthetic
    message corpus encoded/decoded through the catalog with committed
    byte/hash goldens — plus an opennova↔opennova loopback self-capture
    fixture driven by an `nw_golden_diff` self mode. Wired into ci.yml's
    Linux net job (whose explicit test list is a known sync hazard: update
    it in the same commit).
  - *Tier 2, local, mandatory protocol:* the retail pcap golden diff
    (`NW_GOLDEN_OURS` + `.scratch/golden/`) and the npruntime golden joins,
    run locally for every net-touching PR and attested in its description.
- **NET-1** (S) quiesce — **done 2026-07-04**, dispositions (maintainer calls):
  - `worktree-net-final` — **dropped after bundle**: tip `b28cbe8f` verified
    identical in the 2026-07-04 archive bundle and on the still-live
    `origin/worktree-net-final`; local branch deleted. Its five Apr-26/27
    commits (RE/nethook capture tooling, codec_validation.py, novaworld_shim
    spawn/movement) are superseded by wire_capture, nw_replay, and npruntime.
  - `worktree-game-server` — **kept live** (rebase-after): branch content fully
    merged; the worktree carries the active v34 vehicle-drive/EWeap WIP
    (uncommitted, IDA-cited); fast-forward onto master when that work lands.
  - shared nw-merge worktree + `web-nw-for-real-master` — **retired**: branch
    fully merged and deleted (remote already pruned); worktree removed. Its
    untracked `.scratch` (retail_join_v2–v15, `ov-*`, host logs) was destroyed
    with the removal; the three NW goldens survive in the main checkout's
    `.scratch/golden/` and the v16–v35 series in game-server's `.scratch`
    (see docs/asset-gated-tests.md), and the lost captures are re-derivable
    from the `start_v*_capture.ps1` recipes.
- **NET-2** (L) extract the wire + replay legs out of `libs/novaworld` into
  **`libs/npwire`** (final name settled by its ADR): ingame_decode/encode,
  ingame_message_catalog.h, replication_model.h, peer_addr.h,
  replay_timeline, serverlog_decode, wire_capture, and the shared session
  framing (protocol_message, nw_session_framing, session_hello,
  session_keys). Dependency direction becomes `novaworld → npwire →
  napi/novacrypto`: matchmaking sits ON the wire base, and sqlite/gate are
  structurally outside the game link path. Pure `git mv` + include/CMake/
  ci.yml rewrites, zero behavior edits, one revertable train, ADR accepted
  in the same PR.
- **NET-3** (S) reclassifications: `apps/nw_server` README (dev/golden-
  harness host; optional rename is the maintainer's pick);
  `.agents/network.md` rewritten as a redirect to `libs/npruntime/ROADMAP.md`
  + ADR 0013; the promote disambiguation lands (renaming
  `mission::promote_mission` is optional — if picked, full propagation per
  the rename-everywhere rule).
- **NET-4** (S, rides NET-2's ADR) net libs are formally OUTSIDE the C ABI
  (C++-linked only); the dumpbin export-identity check guards it.

### LIBS — topology and seams

Background finding: 44 libs is fine — one-lib-per-format is the tracked
rule. The real issues are one heavy edge and two families.

- **LIBS-1** (M/L) the **world→terrain seam** — DONE (ADR 0020):
  `libs/terrain_query` (height_field + coords, zero deps) is the query
  leaf `libs/world` links; `libs/terrain` sits on it; `wac`/`mission`/net
  closures dropped the terrain-format stack (cpt/til/trn/tpj/foliage);
  `scripts/lint/link_graph_check.py` (transitive-closure forbidden-edge
  check, soft mode in CI) makes the cut permanent. Ran AFTER NET-2 so
  link topology churned once.
- **LIBS-2** (S+M) family topology ADR + execution: affirm
  one-lib-per-format; the terrain and audio families become CMake
  link-interface groups (`opennova_terrain_family`, `opennova_audio_family`)
  rather than physical merges — except `libs/renderer` (4 files that exist
  to dodge one oed header), which folds.
- **LIBS-3** (S) document the two consumption models in libs/CLAUDE.md:
  Model A = the flat C ABI (`opennova_shared`, importer/Python/DCC),
  Model B = C++ static link (engine, apps, net). They already exist de
  facto; naming them makes the npwire ADR's "net stays out of the C ABI"
  clause legible.

### ENG — engine portability, boundary APIs, required resources

Background finding (the seed list — ADR 0016 is the rule): a GDScript
scalar height sampler duplicating the C++ one; a hand-rolled terrain
ray-march under mission picking; duplicated sector/atlas constants; FNT
format facts + a shelf packer in `fnt_rasterizer.gd`; `MATERIAL_FLAG_*`/
`OED_UPDATE_*` duplicated in `nova_object_model.gd`; and
`godot/engine/environment/*.gd` — ~1,167 lines of `[orig]`-cited
TOD/celestial/fog/weather math that belongs in `libs/env`.

- **ENG-1** (M) env parity harness: fixture `.env` files × a time/state
  grid; expected vectors dumped ONCE from the current GDScript (itself the
  cited port) and committed with an explicit tolerance policy. A vector
  divergence during the port triggers a re-grill against the binary — never
  tolerance widening.
- **ENG-2** (L) the env port: the five environment GDScript files →
  `libs/env`; Godot nodes become thin hosts; citations move and are
  re-verified; docs/env/env-honored-matrix.md updated; vectors green
  pre/post; the GDScript math is deleted.
- **ENG-3** (M) terrain query APIs + editor adoption, one slice per bypass:
  unify scalar/batch height on the C++ sampler (the parity test flips from
  pinning drift to pinning the single path); engine-side terrain raycast
  (the GDScript ray-march + slab test deleted; mission picking gated by the
  mission suites + a manual picking pass); sector/atlas constants and
  transforms exposed once and consumed everywhere.
- **ENG-4** (S/M) FNT packing port (shelf packer + format facts into
  `libs/fnt`; the Godot TextServer rasterization stays host-side) and
  object-flag single-sourcing.
- **ENG-5** (S per wave) **the generalized bypass sweep** — a standing
  audit instrument, run at every wave boundary: per-domain review of
  modtools/engine GDScript for math and constants that exist in `libs/`,
  plus grep heuristics (known engine constants, GDScript math adjacent to
  `Nova*` calls). Findings land on the conformance checklist below; each
  closes or carries a tracked exception. The exploration's list is the
  seed, not the boundary.
- **ENG-6** (M) **the required-resources manifest**: an engine-research
  session (R8) enumerates the boot-required, hardcoded-by-name resource set
  from the binary (menumus/gamemus banks, the game strings table, the
  main.mnu set, hudpos.def, default world files, items/weapon defs,
  controls, ...). **R8 landed 2026-07-05**:
  [docs/required-resources.md](required-resources.md) (+ engine-primer
  cross-ref; D-BOOT catalog minted, D-BOOT-1 ledgered). The Wave-2 leg is
  the engine-side manifest table (in `libs/`, near gameprofile) that BOTH
  the game (boot validation, honest missing-resource errors) and ONED
  (diagnostics, the future "new game" scaffold) consume. This defines
  "what a person starts with to make a new game"; the Game workspace
  itself stays out of scope.

#### Boundary conformance checklist (ENG-5 instrument; seeded 2026-07-04)

| Item | Where | Status |
|---|---|---|
| Scalar GDScript height sampler duplicating C++ | terrain editor mesh | open (ENG-3) |
| Hand-rolled terrain ray-march + slab test | terrain editor → mission picking | open (ENG-3) |
| Sector/atlas constants + coord math duplicated | terrain editor mesh | open (ENG-3) |
| FNT format facts + shelf packer in editor | fonts rasterizer | open (ENG-4) |
| MATERIAL_FLAG_* / OED_UPDATE_* duplicated | engine object model GDScript | open (ENG-4) |
| Env/TOD/celestial/weather math in GDScript | godot/engine/environment | open (ENG-1/2) |
| Menu absolute-rect math in canvas | mnu canvas | review (minor; canvas hosts the live engine node) |

### STD — records, constants, testability standards + enforcement

Rules are ADRs 0017/0018; this track is the tooling and the seeded
conversions.

- **STD-1** (M) enforcement tooling, soft mode from Wave 0:
  `scripts/lint/maturity_lint.py` (diff-scoped pattern checks) +
  `scripts/lint/ratchet_counts.py` + a committed baseline JSON, wired as one
  small step into existing CI jobs. See the enforcement table below.
- **STD-2** (M aggregate) seeded record conversions (Wave 2, after the
  pattern survives a wave of real use): document-tab rows, tile gizmo
  state, reference services (dedups its two copies), reference-index edge
  dicts, focus payloads, mission param schema rows, MCP tool defs/args,
  object material defs. Local dicts convert adopt-on-touch only. Templates:
  LinkPayload, WorkspaceDef/InspectorDef.
- **STD-3** (S) hard-fail flip at the Wave-2 boundary for lints that ran a
  wave without false positives. Ratchets are hard from day one (they are
  noise-free by construction).

### ONED — editor maturity

The detail doc is [docs/oned/workspace-maturity-program.md](oned/workspace-maturity-program.md)
(PR #184, rewritten in place as this track). Its R/W/E/G bar, foundation
phases F1–F5, per-workspace phases, and RE ledger stand; this program adds:

- **ONED-A** (M) the avatars merge train — DONE (18791098): the twelfth
  workspace (Avatars) merged from `playerinfo-runtime` across the #180 shell
  decomposition and Wave 0 — editor + engine `nova_avatar_database` +
  fixtures + five test files + RE doc; the branch's ADR renumbered
  0013 → 0021 (it collided with master's consolidated-net-core 0013) with a
  repo-wide reference sweep; the eleven→twelve sweep done (CONTEXT.md,
  modtools README table, screenshot driver, the track doc's matrix row).
- **ONED-F** = the track doc's F1–F5 (F5, the 4,033-line mission_controller
  decomposition, before any MIS phase).
- **ONED-W1/W2/W3** = the track doc's waves (W2's RE-gated bring-ups are
  the first phases the freeze releases).
- **ONED-TST** (M/L) public-seam test refits for the private-poking top
  offenders (ADR 0018's categories; the canary handled with care; identical
  assert counts prove refactor-only; the long tail rides the ratchet).
- **ONED-RSP** (M/L) responsiveness: policy ADR (scale model, ONE window
  floor — today there are two conflicting ones, ratio-based split
  persistence with a versioned migration, `custom_minimum_size` audit) →
  implementation → exactly one visual re-baseline slice.
- **ONED-MUS-D → ONED-MUS-I** (M then L) the music redesign: a bounded
  design spike with signed constraints (document/VM layer untouched; its 46
  tests stay green unmodified; shell left-lane conformance; no modal
  map/section canvas swap; honest navigation over the flat state machine;
  Live/Bank resolution; responsive-first), a maintainer gate, then
  implementation in shippable slices.
- **ONED-REF** (M) reference-extractor gaps: weapon.def, Avatars.def
  (post-A), .wac; .trn/.sbf/.lwf/strings as sources — via the documented
  refs.cpp plug-in point.
- **ONED-REQ** (S/M) required-resources conveniences over the ENG-6
  manifest: missing-required diagnostics; the "new game" scaffold seed.

### PROD — products and serve mode

- **PROD-1** (M) serve mode per ADR 0015: `opennova.exe --server`
  (+ `--headless`): windowed serve jumps the menu host to the
  server-options `.mnu`; headless serve drives the existing host-session
  bring-up (ADR 0013 helper, 62 Hz pump). No new seam, no new protocol.
  The packaging boot smoke gains a `--headless --server` leg.
- **PROD-2** (S) taxonomy conformance: export-presets audit (exactly two
  products), validate_release_deliverables expectations, the
  title-identity fragmentation note recorded as future work.
- **PROD-3** (S/M) program-end release dry run: package both exes, all
  smokes, tier-2 retail-join attestation on the runtime build.

### PAR — parity burn-down (divergence ledger)

Target: **zero OPEN divergences** — every tracked divergence ported-and-closed or
ratified permanent. The living dashboard is
[docs/divergence-ledger.md](divergence-ledger.md); the policy (zero-OPEN target,
canonical vocabulary, the freeze exemption, and the permanent register) is
[ADR 0022](adr/0022-divergence-burn-down.md). Freeze-exempt (maintainer 2026-07-05);
env closures land **libs/env-first** so ENG-2 does not pay twice.

- **PAR-0** (M) this train: the ledger + ADR 0022 + this wiring; the prose-only records
  (3di-gp, 3di-lw, ptl, mis) gain stable `D-` catalogs.
- **PAR-NET** (L) the open D-NET set (the ledger's Net table) + populate
  `nw_golden_diff` `kDeferredGaps` with the D-NET refs from the attested 21-gap baseline,
  so the golden diff names each deferral by ID.
- **PAR-ENV** (M) env #14/#15/#16/#17/#18/#19 (and #21) implemented **libs/env-first**.
- **PAR-WORLD** (M) the D-INF opens and the D-EVT set (D-ITEMDEF-1 closed
  faec4b3e — the first ledger row to zero; the 2026-07-05 D-EVT grill + slice
  closed D-EVT-2/-4, cats 5/6 of D-EVT-3, and minted-closed D-EVT-5, leaving
  D-EVT-1 and the cat-1/2 matrix family witnessed-ready-deferred on the
  TriggerRelations / deploy-POI ports).
- **PAR-UI** (M) D-MNU-5/6, D-CTRL-3, D-PLAYERINFO-11, D-SND-2, and D-HUD after its RE
  port lands.
- **PAR-R1..R7** (S/M each) the seven UNAUDITED-system audits (engine-research /
  grill-ida): terrain, foliage, tiles, fonts, credits, importer pipeline, VFS/PFF mount
  stack — each lands an RE record **with a D-catalog**.
- **Class-B research starters** (freeze-exempt engine-research): the HUD radar/crosshair,
  D-NET-49 (in-match PG), the in-world avatar binding (D-PLAYERINFO-1), and the full `.mis`
  grammar grill (`dfx2med.exe`, D-MIS-1/-3).

## Out of scope (tracked here so nobody re-litigates silently)

DFX2 / any title split (cheap later; no new hardcoded title identity
meanwhile); title-identity consolidation (note-only); NovaWorld
service/backend/web/launcher feature work; the music document/VM layer;
a netsim/npruntime merger (ADR 0013 already consolidated); physical family
merges unless LIBS-2's ADR chooses one; GOALS.md's Game workspace and
export-a-game (this program builds their base); expanding the C ABI to net
libs; new reimplementation outside ONED-W2's gated items (the freeze — IDA
research continues freely).

## Waves

| Wave | Contents | Exit gate |
|---|---|---|
| **0 — bootstrap** (M) | GOV-1/2/3, STD-1 soft, NET-0 | umbrella merged; tier-1 goldens green on all CI legs; lints reporting (not failing) |
| **1 — boundary moves + ONED foundations** (L) | NET-1 → NET-2 → NET-3; LIBS-1 (after NET-2); ENG-1; ENG-6 research start; ONED-A then ONED-F | npwire landed goldens-green + tier-2 attested; seam landed with the link-graph check permanent; env vectors committed; twelve workspaces merged; F1–F5 done (FULL GUT at F5) |
| **2 — portability + standards adoption** (L, widest; WIP cap: two trains) | ENG-2/3/4, ENG-5 sweep #1, ENG-6 manifest; LIBS-2/3; STD-2, STD-3 flip; ONED-W1, ONED-TST, ONED-RSP, ONED-MUS-D (gate at end) | env GDScript deleted, vectors green; conformance checklist closed-or-tracked; ratchets hard and trending down; responsiveness landed + one re-baseline; music design accepted |
| **3 — feature-bearing maturity** (L) | ONED-W2 (freeze releases here), ONED-MUS-I, ONED-REF, ONED-REQ; PROD-1/2; ENG-5 sweep #2 | targeted R/W/E/G cells at bar; music shipped (46 tests untouched-green); serve mode boot-smoked in the packaged exe |
| **4 — equalize + close** (M) | ONED-W3; PROD-3; GOV-4 close-out; ENG-5 final sweep | matrix re-audit over twelve workspaces; release dry run green; **freeze lifted** |

Cross-track ordering: NET-0 before NET-2 (protection precedes the move);
NET-1 before NET-2 (quiesce precedes the move); NET-2 before LIBS-1 (one
link-topology churn) and before PROD-1 (stable homes); ENG-1 before ENG-2;
GOV-3 + STD-1 before all Wave-1+ slices (adopt-on-touch needs minted
rules); ONED-A first in the ONED train (drift control); F5 before MIS
phases; ONED-RSP and ONED-MUS-D before ONED-MUS-I; R-items before their
ONED-W2 phases (UI never ahead of the witness).

## Gates

Standing, every wave: one slice = one green commit; FULL GUT + full ctest at
wave boundaries; the GUT silent-drop greps on every class_name/path move;
dumpbin export-identity on any C-ABI touch; packaging boot smokes; the
canary test (`terrain_editor_workstation_test.gd`).

| Phase | Gate |
|---|---|
| NET-0 | tier-1 vectors/self-capture in default ctest + Linux CI job; a forced one-byte codec change flips them red (sensitivity proof) |
| NET-2 | tier-1 green; `nw_message_coverage` green; full ctest; tier-2 retail diff + npruntime goldens attested; ci.yml list synced in-commit |
| LIBS-1/2 | full ctest; net/wac/mission ctests; link-graph assertion permanent |
| ENG-2 | env vectors pre/post; env-honored-matrix review; visual pass; FULL GUT |
| ENG-3 | flipped parity test; mission suites; canary; FULL GUT |
| STD-2 | per-contract GUT suites; FULL GUT keystones on class_name moves |
| ONED-A | the branch's five avatar test files; FULL GUT; 12-shot screenshot driver; boot probe |
| ONED-TST | refitted suites green at identical assert counts; ratchet decreases |
| ONED-RSP | persistence-migration test; FULL GUT; one visual re-baseline |
| ONED-MUS-I | the 46 music doc/VM tests unmodified-green; new screen tests; FULL GUT; visual pass |
| PROD-1 | boot smokes incl. `--headless --server`; tier-1 green; tier-2 retail-join attestation |
| PROD-3 | validate-deliverables; all smokes |

## Enforcement (STD-1 design)

Principles: diff-scoped for new-code bans (zero noise on untouched code);
repo-wide counts only as ratchets against a committed baseline; hard-fail
only for cheap unambiguous checks; every hard-fail has a maintainer escape
hatch (a baseline bump, logged in this doc).

| Check | Mechanism | Start | End state |
|---|---|---|---|
| Codec identity + self-capture goldens | default ctest | Wave 0 | hard-fail forever |
| Message-catalog coverage | existing CI gate | exists | unchanged |
| Tests poking privates | ratchet (non-self `._name` count in godot/tests), fail-on-increase | Wave 1 | hard from day one; top offenders driven down by ONED-TST |
| New Dictionary contracts | diff-scoped lint over modtools/engine signatures, allowlist for transport edges | Wave 0 soft | hard-fail at Wave-2 boundary |
| [orig] citation coverage | ratchet: libs/*/src files with zero citations (infra libs allowlisted), fail-on-increase | Wave 1 | hard on increase; semantic coverage stays a review concern |
| Magic numbers | diff-scoped advisory in the CI summary | Wave 2 | advisory permanently |
| Link-graph edges | forbidden-edge script (npwire !→ sqlite; wac/mission/net !→ terrain-format libs post-seam) | Wave 1 | hard-fail forever |
| GUT silent-drop greps / C-ABI identity | existing | exists | unchanged |

Home: `scripts/lint/` + baseline JSON; one small step in existing CI jobs
(no new workflow).

## Risks

1. **Wire-leg move breaks retail parity invisibly** (the retail gate is
   env-gated and skip-passes-as-green) → NET-0 lands first; NET-2 is a
   pure move; tier-2 attested; one revertable train.
2. **Freeze friction with in-flight net worktrees** → NET-1 quiesce with
   recorded dispositions; old→new path map published in the NET-2 PR body.
3. **Env port fidelity drift** (float math, GDScript doubles vs C++
   floats) → vectors before port; divergence triggers a re-grill, never a
   tolerance bump; vectors stay as permanent regression tests.
4. **Avatars ADR collision + branch drift** → merge early; renumber with a
   repo-wide reference sweep; the twelve-workspace sweep is a checklist.
5. **Music redesign scope creep** → bounded spike, signed constraints,
   maintainer gate before implementation, shippable slices throughout.
6. **Responsiveness churn** (raw-px persistence, 21 fixed-size sites, the
   canary) → policy ADR first; versioned persistence migration; floors
   unified before scale work; exactly one visual re-baseline slice.
7. **Enforcement noise poisons legitimacy** → diff-scoped bans, committed
   baselines, a soft wave before any hard-fail, logged escape hatch.
8. **Program sprawl vs review bandwidth** → hard wave boundaries; WIP cap
   of two concurrent PR trains; this doc is the only dashboard; ONED is
   the only track with a detail doc.
