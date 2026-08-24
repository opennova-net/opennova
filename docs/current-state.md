# Where the project is, and what is next

The one-page router for "what phase are we in, and where is the next step written
down". It holds no facts of its own: every number and every next step below is a
pointer into the tracked instrument that owns it. When they disagree, the
instrument wins and this page is stale.

## The phase

| Period | What it was | State |
|---|---|---|
| through 2026-07-12 | **The maturity program** — the pre-reimplementation rearchitecture: seven tracks, five waves, the boundary-conformance checklist, the enforcement ratchets | **CLOSED** 2026-07-12, freeze lifted ([maturity-program.md](maturity-program.md)). The standing rules (ADRs 0015–0018, 0022–0024, plus 0028's engine/shell split since 2026-08) and the enforcement instruments survive it |
| since 2026-07-12 | **Retail-fidelity slices** — one system at a time, witnessed in IDA and ported | **current**. There is no separate program doc: the [divergence ledger](divergence-ledger.md) *is* the plan, and each slice's witness lands in its RE record |

The distinction matters for scoping a task. The maturity program was
structural (move code, draw boundaries, add gates) and had a schedule. The
current phase is behavioral (make it act like retail) and is driven by the
ledger's open rows rather than by a wave calendar.

THE structural program is the **rearchitecture**
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
`engine/net/inmatch`, made `world::Match` the shared all-retail-mode/co-op gameplay
owner, and demoted `npruntime`/`netsim` from public layers to implementation
directories. Flag-family gameplay now consumes the collision owner's exact
MoveCB/non-Powerup contact stream; the former gameplay radius scan is deleted
(`Entity_MovementCollisionResolver @0x4B2F90..0x4B2FF5`). The same typed host
request now carries every mode's rule inputs
(including flag limit/return time, KOTH delta, and two/four-side selection),
with an explicit Flag Me route for retail's otherwise-unreachable task-12
launch branch. `godot/game/world/game_frame_pipeline.gd` remains the
device-order owner, superseding ADR 0033's R1 callback-bus design; PR #468 moved local-player camera placement plus
the ObjectModel material loop onto pipeline legs. The P1 rewrite queue that
followed (env/weather, placer,
present facades, composition, avatar, menus), the task-12
no-magic-in-the-godot-layer sweep, and the engine layout flatten (ADR
0024's amendment) are complete; R3 (the render frame) CLOSED not taken at its
2026-08-10 spike — the one-scene screenshot diff vs retail showed no
ordering-attributable delta (ADR 0033 §R3 spike result); R4 (a second
backend) waits on R3's reopen condition. It subsumes the structural slot earlier programs
held: the **2026-07 quality campaign** closed at W4 (#310–#375; its W5
residue stays tracked in [`TODO.md`](../TODO.md) § "Cleanup & verification
backlog"), and the 2026-08-08 adapter-shape round (#451–#456) was this
program's census and ground-clearing.

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
5. **Green bar.** Full ctest, the GUT suite, and the lint gates
   (`scripts/lint/`). See root `CLAUDE.md` for the commands and the
   asset-gated-test caveat.

## Where the open work sits

Counts come from the ledger's generated scoreboard — read them there, not here,
because `scripts/lint/ledger_check.py --check` keeps that table honest and
nothing keeps this sentence honest. As of the 2026-08-15 regeneration the shape was:
**World/AI** carries the largest share (63 of 154 domain-open), **UI** (40 — swollen
by the 2026-08-04 D-SND/D-MNU/D-LOADSCR catalog tabling; most of those rows are
small or permanent-register candidates) and **Net** (24) the next largest, and every
other domain is in single digits.

Each domain's next step is named in its own record, not centrally:

| Domain | Open rows live in | The record that names the next step |
|---|---|---|
| World / AI + gameplay | ledger § World | [world/world-wac-ai-re.md](world/world-wac-ai-re.md) (§14–§30); the #403 client mover/prediction record is [world/vehicle-client-movers-re.md](world/vehicle-client-movers-re.md) (ADR 0026 topology) |
| Net (in-match + matchmaking) | ledger § Net (`PAR-NET`) | [net/novaworld-net-re.md](net/novaworld-net-re.md) §8; the build record behind it is `engine/net/npruntime/ROADMAP.md` |
| UI (HUD, menus, sound, player info) | ledger § UI | [interface/hud-re.md](interface/hud-re.md), [interface/loading-screen-re.md](interface/loading-screen-re.md), [mnu/menu-re.md](mnu/menu-re.md), [playerinfo/avatars-re.md](playerinfo/avatars-re.md), [audio/lwf-dbf-sound-re.md](audio/lwf-dbf-sound-re.md) |
| Render (materials, order, lighting, occlusion) | ledger § Render ×3 | [render/README.md](render/README.md) |
| Terrain / foliage / tiles | ledger § Terrain, Foliage, Tiles | [terrain/terrain-re.md](terrain/terrain-re.md), [foliage/foliage-re.md](foliage/foliage-re.md) |
| Environment | ledger § Environment | [env/env-tod-re.md](env/env-tod-re.md), [env/env-honored-matrix.md](env/env-honored-matrix.md) |
| Formats (`.mis`, `.ptl`, LW `.3di`, CBIN, fonts, VFS) | ledger, per format | the matching record in [README.md](README.md) |
| Editor depth (ONED workspaces) | TODO.md (editor UX rows) + the workspace matrix | [oned/workspace-maturity-program.md](oned/workspace-maturity-program.md) — the editor's standing roadmap (survived the umbrella close) |

Work that is **not** a parity divergence — editor UX, project health, code
hardening — lives in [`TODO.md`](../TODO.md) at the repo root instead; it is
the ONE non-parity backlog. Completed-effort records are not plans: `plan/`
(the NovaWorld-integration era), `engine/net/npruntime/ROADMAP.md`,
[oned/editor-layer-program.md](oned/editor-layer-program.md), and
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

Two rows across the ledger are explicitly tagged **`research starter`** in their
Slice column — one `NEEDS-RE` row (D-THROW-6) and one `OPEN` row waiting on a
specific witness (D-HUD-6). D-PLAYERINFO-1 closed on 2026-08-15 after the packed
character-id, world/first-person submit, and per-part `TEX_CAMO` paths were
witnessed and ported. D-HUD-7 closed with the 2026-07-31 recoil/spread grill;
D-NET-49 closed pre-2026-08-01 (its stale ledger row was reconciled in #403).
The remaining two are scoped small enough to be somebody's first grill.

## The instruments that keep this honest

| Instrument | What it prevents | How it runs |
|---|---|---|
| [divergence-ledger.md](divergence-ledger.md) + `scripts/lint/ledger_check.py` | a divergence being known but untracked, or the scoreboard drifting from its own tables | CI, hard-fail; `--write` regenerates |
| `scripts/lint/ratchet_counts.py` | new uncited `engine/` source files; new tests poking privates | CI, fail-on-increase against a committed baseline |
| `scripts/lint/link_graph_check.py` | forbidden lib edges (ADR 0019/0020 seams) | CI, hard-fail |
| `scripts/lint/include_graph_check.py` | the ADR 0020 terrain seam dissolving with the ADR 0029 target collapse — net/wac/mission may include only terrain_query's four `terrain_query/` headers, never the terrain-format stack | CI, hard-fail |
| `scripts/lint/host_lint.py` | "host" regressing to any non-game-host sense (the terminology campaign's teeth) | CI, hard-fail on code suffixes; Markdown gets a non-failing advisory |
| `scripts/lint/maturity_lint.py` | dict-contract drift and new magic numbers in changed `.gd` ranges | CI, hard-fail (advisory legs stay advisory) |
| `abi_export_identity` ctest | an accidental change to the flat C ABI | default ctest; a baseline bump is same-commit and logged in [maturity-program.md](maturity-program.md) |
| [asset-gated-tests.md](asset-gated-tests.md) | believing a green run exercised retail data when the env vars were unset | read it before trusting a parity green |

## What this page is not

It is not a priority order and not a commitment. Which slice runs next is the
maintainer's call; this page only makes sure that once that call is made, the
next step is already written down somewhere tracked.
