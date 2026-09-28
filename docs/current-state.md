# Where the project is, and what is next

The one-page router for "what phase are we in, and where is the next step written
down". It holds no facts of its own: every number and every next step below is a
pointer into the tracked instrument that owns it. When they disagree, the
instrument wins and this page is stale.

## The phase

| Period | What it was | State |
|---|---|---|
| through 2026-07-12 | **The maturity program** — the pre-reimplementation rearchitecture: seven tracks, five waves, the boundary-conformance checklist, the enforcement ratchets | **CLOSED** 2026-07-12, freeze lifted ([maturity-program.md](maturity-program.md)). The standing rules (ADRs 0015, 0017, 0018, 0022 and 0023; 0016 and 0024 are historical, their surviving rules carried by [ADR 0042](adr/0042-godot-permanent-shell-one-mission-kernel.md)'s boundary rule and [ADR 0029](adr/0029-engine-group-targets.md)'s group targets; 0028's engine/shell split; [ADR 0045](adr/0045-cli-game-data-runtime-only.md)'s CLI game data and runtime-only distribution, which superseded 0037's ONED hard cut and which [ADR 0048](adr/0048-bundled-placeholder-menu-and-retail-picker.md) amends; and [ADR 0038](adr/0038-native-runtime-assets-glb-editor.md)'s native-runtime-assets hard cut, as [ADR 0047](adr/0047-blender-3di-exporter.md) restates it) and the enforcement instruments survive it |
| since 2026-07-12 | **Retail-fidelity slices** — one system at a time, witnessed in IDA and ported | **current**. There is no separate program doc: the [divergence ledger](divergence-ledger.md) *is* the plan, and each slice's witness lands in its RE record |

The distinction matters for scoping a task. The maturity program was
structural (move code, draw boundaries, add gates) and had a schedule. The
current phase is behavioral (make it act like retail) and is driven by the
ledger's open rows rather than by a wave calendar.

The current architecture is [ADR 0043](adr/0043-canonical-cpp-and-godot-hard-cut.md),
the hard cut to canonical C++ and Godot (landed in PR #626): behavior on its
owning systems, one `inmatch::Session` with roles, `net/` meaning wire, the
Godot world as C++ Nodes and tests over real fixtures. The records it
supersedes (ADRs 0031 to 0033, 0035 and 0036) and
[ADR 0042](adr/0042-godot-permanent-shell-one-mission-kernel.md) (Godot the
permanent sole shell, one mission kernel) carry their own round histories;
[ADR 0044](adr/0044-shared-native-assets.md) (shared native assets) and
[ADR 0047](adr/0047-blender-3di-exporter.md) (the Blender `.3di` add-on and
`opennova-3di`) follow it. The push-down campaign in
[ADR 0040](adr/0040-the-engine-is-one-namespace.md)'s ladder is closed
(2026-09-21, PR #663): every queued row landed or was explicitly closed by the
accessor/native-home audit, and the Godot code that remains implements the
device contracts the rows name. There is no structural queue left; the
`godot_orig_cites` ratchet (read the count from the baseline, not here) now
holds only those documented seam contracts and may fall when a cite moves to
its engine home or dies, never rise.

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

Counts come from the [ledger's generated scoreboard](divergence-ledger.md#count-to-zero-scoreboard),
maintained by `scripts/lint/ledger_check.py --check`. The
[2026-09-13 jo-c audit](jo-c-parity-audit-2026-09-13.md) and its
[dated inventory](jo-c-parity-audit-2026-09-13.json) assign the IDs active at
its master baseline to implementation packages; they are a snapshot, and the ledger
carries every status change since (the PR that landed it already closed part of it).

Each domain's next step is named in its own record, not centrally:

| Domain | Open rows live in | The record that names the next step |
|---|---|---|
| World / AI + gameplay | ledger § World | [world/world-wac-ai-re.md](world/world-wac-ai-re.md); the vehicle authority and prediction record [world/vehicle-client-movers-re.md](world/vehicle-client-movers-re.md) (ADR 0026 topology); [world/tank-parity-re.md](world/tank-parity-re.md); [world/itemdef-re.md](world/itemdef-re.md); [mission/bms-event-runtime-re.md](mission/bms-event-runtime-re.md) |
| Mission acceptance (playthroughs) | ledger § World | [world/npc-mission-completion.md](world/npc-mission-completion.md): the playthrough gates, the dated acceptance verdicts and what stays unported |
| Item class events | none open (the ledger's closure lines) | [World §24.3a](world/world-wac-ai-re.md#243a-class-clocks-regional-shots-barrels-buildings-flags-and-targets) and §24.3b |
| Net (in-match + matchmaking) | ledger § Net (`PAR-NET`) | [net/novaworld-net-re.md](net/novaworld-net-re.md) §8 (the D-NET catalog), with the [dispatch-table audit](net/retail-message-dispatch-audit.md) for D-NET-218; the build record behind it is `engine/runtime/inmatch/ROADMAP.md` |
| UI (HUD, menus, sound, player info) | ledger § UI | [interface/hud-re.md](interface/hud-re.md), [interface/loading-screen-re.md](interface/loading-screen-re.md), [mnu/menu-re.md](mnu/menu-re.md), [playerinfo/avatars-re.md](playerinfo/avatars-re.md), [audio/lwf-dbf-sound-re.md](audio/lwf-dbf-sound-re.md) |
| Render (materials, order, lighting, occlusion) | ledger § Render — occlusion (D-OCC-12; materials/state, draw order and lighting hold no open rows) | [render/README.md](render/README.md) and each render record's catalog |
| Terrain / foliage / tiles | none open (the ledger's closure lines) | [terrain/terrain-re.md](terrain/terrain-re.md) (its pending list: CDEP/traversal documentation depth and the single-detail binding), [foliage/foliage-re.md](foliage/foliage-re.md), [tiles/til-re.md](tiles/til-re.md) |
| Environment | none open (the ledger's closure lines) | [env/env-tod-re.md](env/env-tod-re.md) (its "Open after the 2026-09-24 pass" list: the two water-mirror measurements), [env/env-honored-matrix.md](env/env-honored-matrix.md) |
| Formats (`.mis`, `.ptl`, LW `.3di`, CBIN, fonts, VFS) | ledger, per format | the matching record in [README.md](README.md); the particle record's open list follows its [rendering parity section](particles/ptl-format-re.md#rendering-parity-pass-2026-09-24) |
| Mission savegames | ledger § Mission savegames | [mission/savegame-re.md](mission/savegame-re.md): the schema, runtime state restoration and slot lifecycle (D-SAVE-1) |
| Performance | not a divergence (the [`TODO.md`](../TODO.md) perf rows) | [perf/03tr-frame-costs.md](perf/03tr-frame-costs.md) and the `TODO.md` perf rows |

Work that is **not** a parity divergence — project health, code hardening — lives in [`TODO.md`](../TODO.md) at the repo root instead; it is
the ONE non-parity backlog. Completed-effort records are not plans:
`engine/runtime/inmatch/ROADMAP.md` and
[maturity-program.md](maturity-program.md)'s historical body (its two log
appendices stay live). The retired ONED product's own records (the editor-layer
program, the workspace maturity track, the editor/runtime parity patterns) were
removed 2026-09-28 and live only in git history under `docs/oned/`.

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
| `scripts/lint/ratchet_counts.py` | new uncited `engine/` source files; new tests poking privates; shipping GDScript reaching into another object's privates (`gd_foreign_private_accesses`, the annex pattern, ADR 0043); witness citations growing Godot-side (`godot_orig_cites`, ONE `[orig:` count over `godot/src` + `godot/game` + `godot/probes` — non-increasing, banked in the baseline; a decrease means code moved to its engine home or died); any witness cite under `godot/game/mcp` (`mcp_boundary_cites`, an absolute zero floor) | CI, fail-on-increase against a committed baseline (`scripts/lint/maturity_baseline.json`); the mcp floor is absolute, no baseline key |
| `scripts/lint/cite_census.py` | a witness address (`@0xADDR`) silently disappearing from the code trees during a structural move; a wire-frozen name or a docs citation/ledger id lost in a rename (`--audit-range`, the local diff guard) | CI, hard-fail against `scripts/lint/cite_census_baseline.json`; a deliberate deletion is banked with `--write-baseline` and named in the commit |
| `scripts/lint/link_graph_check.py` | forbidden lib edges (ADR 0019/0020 seams) | CI, hard-fail |
| `scripts/lint/include_graph_check.py` | the ADR 0020 terrain seam dissolving with the ADR 0029 target collapse — net/wac/mission/world may include only terrain_query's six seam headers (`coords.h`, `height_field.h`, `surface_type_map.h`, `terrain_field_store.h` — ADR 0042 d4's engine field builder — `terrain_raycast.h`, `terrain_scorch_record.h`, under the group-qualified `runtime/terrain_query/` prefix), never the terrain-format stack; Dear ImGui includes escaping `engine/runtime/devtools` + `tests/devtools` (ADR 0042 d6) | CI, hard-fail |
| `scripts/lint/orphan_header_check.py` | an `engine/` header wired to nothing: policy code that no engine, binding or app source includes (a header included only by its own test is exactly that shape), left behind by a move or landed ahead of its caller | CI, hard-fail; two deliberate escapes, `scripts/lint/orphan_header_allowlist.json` (a reasoned-exception list, burned down on 2026-08-27 to its one entry-point row, `godot/src/register_types.h`) and a `STAGED, NOT WIRED` marker in the header naming the owner that will consume it |
| `scripts/lint/maturity_lint.py` | dict-contract drift and new magic numbers in changed `.gd` ranges | CI, hard-fail (advisory legs stay advisory) |
| `scripts/lint/env_lint.py` | a new environment read outside the two documented roots (`OPENNOVA_JO_DIR`, `OPENNOVA_JO_ASSETS`), `GODOT_BIN` and the service family (ADR 0041; [dev-env-vars.md](dev-env-vars.md)) | CI, hard-fail; the allowlist is `scripts/lint/env_allowlist.json` |
| `scripts/lint/fixture_lint.py` | a file under `fixtures/` that is not MINTED, AUTHORED or KEEP, an oversize fixture, or one that missed LFS (`fixtures/README.md`, ADR 0041) | CI, hard-fail with `--require-pulled`; the KEEP list is `scripts/lint/fixture_allowlist.json` |
| `scripts/lint/conventions_lint.py` | a `Nova`/`nova_` prefix returning after ADR 0040 | CI, hard-fail (`nova-macro` stays advisory) |
| [asset-gated-tests.md](asset-gated-tests.md) | believing a green run exercised retail data when the env vars were unset | read it before trusting a parity green |

## What this page is not

It is not a priority order and not a commitment. Which slice runs next is the
maintainer's call; this page only makes sure that once that call is made, the
next step is already written down somewhere tracked.
