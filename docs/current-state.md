# Where the project is, and what is next

The one-page router for "what phase are we in, and where is the next step written
down". It holds no facts of its own: every number and every next step below is a
pointer into the tracked instrument that owns it. When they disagree, the
instrument wins and this page is stale.

## The phase

| Period | What it was | State |
|---|---|---|
| through 2026-07-12 | **The maturity program** — the pre-reimplementation rearchitecture: seven tracks, five waves, the boundary-conformance checklist, the enforcement ratchets | **CLOSED** 2026-07-12, freeze lifted ([maturity-program.md](maturity-program.md)). The standing rules (ADRs 0015–0018, 0022–0024, 0028's engine/shell split, 0037's ONED hard cut, and [ADR 0038](adr/0038-native-runtime-assets-glb-editor.md)'s native-runtime-assets hard cut) and the enforcement instruments survive it |
| since 2026-07-12 | **Retail-fidelity slices** — one system at a time, witnessed in IDA and ported | **current**. There is no separate program doc: the [divergence ledger](divergence-ledger.md) *is* the plan, and each slice's witness lands in its RE record |

The distinction matters for scoping a task. The maturity program was
structural (move code, draw boundaries, add gates) and had a schedule. The
current phase is behavioral (make it act like retail) and is driven by the
ledger's open rows rather than by a wave calendar.

THE structural program since 2026-09-02 is the **hard cut to canonical C++ and
Godot** ([ADR 0043](adr/0043-canonical-cpp-and-godot-hard-cut.md), the one
current-architecture record: behavior onto owning systems, one session with
roles, `net/` means wire, the Godot world as C++ Nodes, tests over real
fixtures), landing as one PR of committed-as-we-go slices. What led to it,
in order: the **rearchitecture**
([ADR 0033](adr/0033-engine-owned-loops-device-shells.md), approved
2026-08-09, replacing the ADR 0031/0032 adapter-seam regime in full): the
engine takes ownership of the main/tick/render loops behind a device
boundary. As of 2026-08-10 (trunk PR #460): R1 (the frame port) and R2
(draw-list presentation — terrain, foliage, HUD, and menus, whose cutover
deleted the MnuMenu Control tree for the compiled MenuFrame + MenuDriver
path) LANDED — and R1 was then superseded by
[ADR 0035](adr/0035-mission-session-game-frame-pipeline.md) (ADR 0033's
ladder state records it): PR #465 first rearchitected the lifecycle and
frame pipeline. [ADR 0036](adr/0036-one-inmatch-session-wire-first.md) then
completed the no-compat cutover to portable `opennova::inmatch::Session` in
`engine/runtime/inmatch`, made `world::Match` the shared all-retail-mode/co-op gameplay
owner, and demoted `npruntime`/`netsim` from public layers to implementation
directories. Flag-family gameplay now consumes the collision owner's exact
MoveCB/non-Powerup contact stream; the former gameplay radius scan is deleted
(`Entity_MovementCollisionResolver @0x4B2F90..0x4B2FF5`). The same typed host
request now carries every mode's rule inputs
(including flag limit/return time, KOTH delta, and two/four-side selection),
with an explicit Flag Me route for retail's otherwise-unreachable task-12
launch branch. `godot/src/world/game_world_frame.cpp`'s leg table (ADR 0043 slice G10) is the
device-order owner, superseding ADR 0033's R1 callback-bus design; PR #468 moved local-player camera placement plus
the ObjectModel material loop onto pipeline legs. The P1 rewrite queue that
followed (env/weather, placer,
present facades, composition, avatar, menus), the task-12
no-magic-in-the-godot-layer sweep, and the engine layout flatten (ADR
0024's amendment) are complete; R3 (the render frame) CLOSED not taken at its
2026-08-10 spike — the one-scene screenshot diff vs retail showed no
ordering-attributable delta (ADR 0033 §R3 spike result); R4 (a second
backend) CLOSED by [ADR 0042](adr/0042-godot-permanent-shell-one-mission-kernel.md)
(2026-08-28): no second backend, ever. **The next structural program is the
push-down campaign** opened by [ADR 0040](adr/0040-the-engine-is-one-namespace.md)
(2026-08-26, PR #580): with the names and include roots settled, the witnessed
behavior still living Godot-side moves home — the `godot_orig_cites` ratchet
(ONE count over `godot/src` and the game-level GDScript since ADR 0043; one
marker since ADR 0042 d7) is the gauge, and it runs as one commit per slice.
PR #580 landed thirteen of them: the pattern-setting moves (the local-player
view cluster, the minimap feed, LAN browse, the boot/pack/options policies, the
seat mirror + volume law + weapon-category rows), two `godot/src/simulation/`
slices (the occlusion frame camera, the iris march), and the game-level
GDScript presentation math (the mission load plan, the HUD config tokens, the
presenter and viewmodel frame math, the light-director constants, the loading
screen, the character registry). The remainder is queued in
[ADR 0040](adr/0040-the-engine-is-one-namespace.md)'s ladder, which is that
queue's only home: the Godot-only witnessed half of Train B (the present
rows, the sun feed, the scar gate, the joiner pump — on-touch after a grill;
A3 and the rig-twinned half landed with ADR 0042), the Train C tail including
the `mission_audio.cpp` reverb, and Train D in full. The counters
(read them from the baseline, not here) are not yet at a seam-contract floor;
the campaign is finished when both hold only documented seam contracts.
[ADR 0042](adr/0042-godot-permanent-shell-one-mission-kernel.md) (2026-08-28)
closed the boundary question: Godot is the permanent sole shell (ADR 0033 R4
CLOSED), and its campaign LANDED (PR #587): the mission kernel
(`engine/runtime/mission/mission_kernel`) + listen-host frame
(`engine/runtime/inmatch/host_role`, ex `listen_host`) promoted from the retail-mission rig with
`Simulation`, `nw_server` and the ctests as the three embedders, the
`world::inspect` typed records + the typed debug-control table behind MCP and
F3, and the F3 Entities window as the records-in/requests-out template. The
0040 ladder rows A3/B carry the hashes; the Godot-only witnessed remainder is
on-touch.
It subsumes the structural slot earlier programs
held: the **2026-07 quality campaign** closed at W4 (#310–#375; its W5
residue is absorbed: the perf-span unify became ADR 0039's `FrameStatsBoard`,
the debug-snapshot narrowing went with that hard cut, and the runtime
push-downs are this ladder), and the 2026-08-08 adapter-shape round
(#451–#456) was this program's census and ground-clearing.

## The standing loop for a fidelity slice

Every recent slice ran this same shape, and a new one should too:

1. **Witness first.** `engine-research` for a behavior nobody has looked at,
   `grill-ida` to verify an existing reimplementation against the binary. Never
   design the behavior yourself ([GOALS.md](../GOALS.md), root `CLAUDE.md`).
2. **Port as a structural translation**, citing the original inline at the port
   site: `[orig: Name @ 0xADDR]`.
3. **Land the record** with `re-doc`: the witness map, the verdicts, and stable
   `D-<DOMAIN>-n` ids go into the domain's RE record under `docs/`.
4. **Move the ledger in the same PR.** Rows the slice closed go `FIXED` with the
   closing witness; gaps the slice leaves behind get rows *at birth*. A slice
   that ports 80% of a system and opens rows for the other 20% is a good slice;
   one that ports 80% silently is not.
5. **Green bar.** Full CTest, the GUT suite, and the lint gates (`scripts/lint/`).
   See root `CLAUDE.md` for the commands and the asset-gated-test caveat.

## Where the open work sits

Counts come from the ledger's generated scoreboard — read them there, not here,
because `scripts/lint/ledger_check.py --check` keeps that table honest and
nothing keeps this sentence honest. As of the 2026-08-30 post-merge tidy (after
the #595 renderer, #597 weather, and #601 spectator merges landed) the shape
was: **World/AI** carries the largest share (56 of 121 domain-open; Fonts and
VFS emptied 2026-08-29), **UI** (29 — the 2026-08-04 D-SND/D-MNU/D-LOADSCR
catalog tabling's twelve register candidates were ratified `PERMANENT` on
2026-08-29; the rest are small), **Net** (22), and the freshly tabled
**Render — occlusion** (5 — the D-OCC-9..15 port divergences the record had
carried since 2026-07-17, tabled per standing rule 2) the next largest, and
every other domain is in single digits.

Each domain's next step is named in its own record, not centrally:

| Domain | Open rows live in | The record that names the next step |
|---|---|---|
| World / AI + gameplay | ledger § World | [world/world-wac-ai-re.md](world/world-wac-ai-re.md) (§14–§32); the #403 client mover/prediction record is [world/vehicle-client-movers-re.md](world/vehicle-client-movers-re.md) (ADR 0026 topology) |
| Net (in-match + matchmaking) | ledger § Net (`PAR-NET`) | [net/novaworld-net-re.md](net/novaworld-net-re.md) §8; the build record behind it is `engine/runtime/inmatch/ROADMAP.md` ; section 5.10 covers joiner vehicle confirmation and shared seat/overlay occupancy |
| UI (HUD, menus, sound, player info) | ledger § UI | [interface/hud-re.md](interface/hud-re.md), [interface/loading-screen-re.md](interface/loading-screen-re.md), [mnu/menu-re.md](mnu/menu-re.md), [playerinfo/avatars-re.md](playerinfo/avatars-re.md), [audio/lwf-dbf-sound-re.md](audio/lwf-dbf-sound-re.md) |
| Render (materials, order, lighting, occlusion) | ledger § Render — draw order + § Render — occlusion (the materials/state and lighting tables hold no open rows) | [render/README.md](render/README.md) |
| Terrain / foliage / tiles | ledger § Terrain, Foliage (§ Tiles holds no open rows: D-TIL-1..4 FIXED) | [terrain/terrain-re.md](terrain/terrain-re.md), [foliage/foliage-re.md](foliage/foliage-re.md) |
| Environment | ledger § Environment | [env/env-tod-re.md](env/env-tod-re.md), [env/env-honored-matrix.md](env/env-honored-matrix.md) |
| Formats (`.mis`, `.ptl`, LW `.3di`, CBIN, fonts, VFS) | ledger, per format | the matching record in [README.md](README.md) |

Work that is **not** a parity divergence — ONED and OpenNova Launcher UX, project health, code
hardening — lives in [`TODO.md`](../TODO.md) at the repo root instead; it is
the ONE non-parity backlog. Completed-effort records are not plans: `plan/`
(the NovaWorld-integration era), `engine/runtime/inmatch/ROADMAP.md`,
[oned/editor-layer-program.md](oned/editor-layer-program.md),
[oned/editor-runtime-parity.md](oned/editor-runtime-parity.md),
[oned/workspace-maturity-program.md](oned/workspace-maturity-program.md), and
[maturity-program.md](maturity-program.md)'s historical body (its two log
appendices stay live).

## The research queue

`NEEDS-RE` is the ledger's own name for "we cannot port this yet because nobody
has witnessed it". Those rows are the highest-signal starting points for an
`engine-research` session, because each one is a question the project has already
decided it needs answered. Get the current list straight from the ledger:

```bash
grep -n 'NEEDS-RE' docs/divergence-ledger.md
```

The former research starter D-THROW-6 closed on 2026-09-04: `lndm` mission
minefields occur in retail `00TRd` and `CP09`, and their init, contact, fire,
replica and marker-rendering chain is ported. See
[world-wac-ai-re.md §27.6a](world/world-wac-ai-re.md#276a-mission-minefields-lndm-d-throw-6)
for the evidence and tests. Select further research from the ledger's current
`NEEDS-RE` rows.

## The instruments that keep this honest

| Instrument | What it prevents | How it runs |
|---|---|---|
| [divergence-ledger.md](divergence-ledger.md) + `scripts/lint/ledger_check.py` | a divergence being known but untracked, or the scoreboard drifting from its own tables | CI, hard-fail; `--write` regenerates |
| `scripts/lint/ratchet_counts.py` | new uncited `engine/` source files; new tests poking privates; shipping GDScript reaching into another object's privates (`gd_foreign_private_accesses`, the annex pattern, ADR 0043); witness citations growing Godot-side (`godot_orig_cites`, ONE `[orig:` count over `godot/src` + `godot/game` + `godot/modtools` + `godot/probes` — non-increasing, banked in the baseline; a decrease means code moved to its engine home or died); any witness cite under `godot/game/mcp` (`mcp_boundary_cites`, an absolute zero floor) | CI, fail-on-increase against a committed baseline (`scripts/lint/maturity_baseline.json`); the mcp floor is absolute, no baseline key |
| `scripts/lint/cite_census.py` | a witness address (`@0xADDR`) silently disappearing from the code trees during a structural move; a wire-frozen name or a docs citation/ledger id lost in a rename (`--audit-range`, the local diff guard) | CI, hard-fail against `scripts/lint/cite_census_baseline.json`; a deliberate deletion is banked with `--write-baseline` and named in the commit |
| `scripts/lint/link_graph_check.py` | forbidden lib edges (ADR 0019/0020 seams) | CI, hard-fail |
| `scripts/lint/include_graph_check.py` | the ADR 0020 terrain seam dissolving with the ADR 0029 target collapse — net/wac/mission/world may include only terrain_query's six seam headers (`coords.h`, `height_field.h`, `surface_type_map.h`, `terrain_field_store.h` — ADR 0042 d4's engine field builder — `terrain_raycast.h`, `terrain_scorch_record.h`, under the group-qualified `runtime/terrain_query/` prefix), never the terrain-format stack; Dear ImGui includes escaping `engine/runtime/devtools` + `tests/devtools` (ADR 0042 d6) | CI, hard-fail |
| `scripts/lint/orphan_header_check.py` | an `engine/` header wired to nothing: policy code that no engine, binding or app source includes (a header included only by its own test is exactly that shape), left behind by a move or landed ahead of its caller | CI, hard-fail; two deliberate escapes, `scripts/lint/orphan_header_allowlist.json` (a reasoned-exception list, burned down to empty on 2026-08-27) and a `STAGED, NOT WIRED` marker in the header naming the owner that will consume it |
| `scripts/lint/maturity_lint.py` | dict-contract drift and new magic numbers in changed `.gd` ranges | CI, hard-fail (advisory legs stay advisory) |
| `scripts/lint/env_lint.py` | a new environment read outside the two documented roots (`OPENNOVA_JO_DIR`, `OPENNOVA_JO_ASSETS`), `GODOT_BIN` and the service family (ADR 0041; [dev-env-vars.md](dev-env-vars.md)) | CI, hard-fail; the allowlist is `scripts/lint/env_allowlist.json` |
| `scripts/lint/fixture_lint.py` | a file under `fixtures/` that is not MINTED, AUTHORED or KEEP, an oversize fixture, or one that missed LFS (`fixtures/README.md`, ADR 0041) | CI, hard-fail with `--require-pulled`; the KEEP list is `scripts/lint/fixture_allowlist.json` |
| `scripts/lint/conventions_lint.py` | a `Nova`/`nova_` prefix returning after ADR 0040 | CI, hard-fail (`nova-macro` stays advisory) |
| [asset-gated-tests.md](asset-gated-tests.md) | believing a green run exercised retail data when the env vars were unset | read it before trusting a parity green |

## What this page is not

It is not a priority order and not a commitment. Which slice runs next is the
maintainer's call; this page only makes sure that once that call is made, the
next step is already written down somewhere tracked.
