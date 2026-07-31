# Where the project is, and what is next

The one-page router for "what phase are we in, and where is the next step written
down". It holds no facts of its own: every number and every next step below is a
pointer into the tracked instrument that owns it. When they disagree, the
instrument wins and this page is stale.

## The phase

| Period | What it was | State |
|---|---|---|
| through 2026-07-12 | **The maturity program** — the pre-reimplementation rearchitecture: seven tracks, five waves, the boundary-conformance checklist, the enforcement ratchets | **CLOSED** 2026-07-12, freeze lifted ([maturity-program.md](maturity-program.md)). The standing rules (ADRs 0015–0018, 0022–0024) and the enforcement instruments survive it |
| since 2026-07-12 | **Retail-fidelity slices** — one system at a time, witnessed in IDA and ported | **current**. There is no separate program doc: the [divergence ledger](divergence-ledger.md) *is* the plan, and each slice's witness lands in its RE record |

The distinction matters for scoping a task. The maturity program was
structural (move code, draw boundaries, add gates) and had a schedule. The
current phase is behavioral (make it act like retail) and is driven by the
ledger's open rows rather than by a wave calendar.

One structural workstream runs in parallel with the fidelity slices: the
**2026-07 quality campaign** (approved 2026-07-28) — small independent PRs
straight to master for dead-code removal, duplication collapse, and god-file
splits. Waves 1–3 landed as #310–#356 (Wave 3 closed with the #355/#356
header-width pass, which landed the `oversize_cpp_files` ratchet); Wave 4
landed as #357–#375 (closed by #375's W4-6 modtools splits and the
`oversize_gd_files` ratchet at its residual floor of 7 — `game_world.gd`
stays whole by design as the load path); the remaining W5 slices are
tracked in [`TODO.md`](../TODO.md) § "Quality campaign — remaining slices".

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
nothing keeps this sentence honest. As of the last regeneration the shape was:
**World/AI** carries roughly half the open rows, **UI** and **Net** the next
largest shares, and every other domain is in single digits.

Each domain's next step is named in its own record, not centrally:

| Domain | Open rows live in | The record that names the next step |
|---|---|---|
| World / AI + gameplay | ledger § World | [world/world-wac-ai-re.md](world/world-wac-ai-re.md) (§14–§27) |
| Net (in-match + matchmaking) | ledger § Net (`PAR-NET`) | [net/novaworld-net-re.md](net/novaworld-net-re.md) §8; the build record behind it is `libs/npruntime/ROADMAP.md` |
| UI (HUD, menus, sound, player info) | ledger § UI | [interface/hud-re.md](interface/hud-re.md), [mnu/menu-re.md](mnu/menu-re.md), [playerinfo/avatars-re.md](playerinfo/avatars-re.md), [audio/lwf-dbf-sound-re.md](audio/lwf-dbf-sound-re.md) |
| Render (materials, order, lighting, occlusion) | ledger § Render ×3 | [render/README.md](render/README.md) |
| Terrain / foliage / tiles | ledger § Terrain, Foliage, Tiles | [terrain/terrain-re.md](terrain/terrain-re.md), [foliage/foliage-re.md](foliage/foliage-re.md) |
| Environment | ledger § Environment | [env/env-tod-re.md](env/env-tod-re.md), [env/env-honored-matrix.md](env/env-honored-matrix.md) |
| Formats (`.mis`, `.ptl`, LW `.3di`, CBIN, fonts, VFS) | ledger, per format | the matching record in [README.md](README.md) |

Work that is **not** a parity divergence — editor UX, project health, code
hardening — lives in [`TODO.md`](../TODO.md) at the repo root instead.

## The research queue

`NEEDS-RE` is the ledger's own name for "we cannot port this yet because nobody
has witnessed it". Those rows are the highest-signal starting points for an
`engine-research` session, because each one is a question the project has already
decided it needs answered. Get the current list straight from the ledger:

```bash
grep -n 'NEEDS-RE' docs/divergence-ledger.md
```

Four rows across the ledger are explicitly tagged **`research starter`** in their
Slice column — three `NEEDS-RE` (D-NET-49, D-THROW-6, D-PLAYERINFO-1) and one
`OPEN` row waiting on a specific witness (D-HUD-6). D-HUD-7 closed with the
2026-07-31 recoil/spread grill. The remaining four are scoped small enough to be
somebody's first grill.

## The instruments that keep this honest

| Instrument | What it prevents | How it runs |
|---|---|---|
| [divergence-ledger.md](divergence-ledger.md) + `scripts/lint/ledger_check.py` | a divergence being known but untracked, or the scoreboard drifting from its own tables | CI, hard-fail; `--write` regenerates |
| `scripts/lint/ratchet_counts.py` | new uncited `libs/` source files; new tests poking privates | CI, fail-on-increase against a committed baseline |
| `scripts/lint/link_graph_check.py` | forbidden lib edges (ADR 0019/0020 seams) | CI, hard-fail |
| `abi_export_identity` ctest | an accidental change to the flat C ABI | default ctest; a baseline bump is same-commit and logged in [maturity-program.md](maturity-program.md) |
| [asset-gated-tests.md](asset-gated-tests.md) | believing a green run exercised retail data when the env vars were unset | read it before trusting a parity green |

## What this page is not

It is not a priority order and not a commitment. Which slice runs next is the
maintainer's call; this page only makes sure that once that call is made, the
next step is already written down somewhere tracked.
