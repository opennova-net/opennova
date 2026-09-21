# Where the project is, and what is next

PR #663 also fixes 03TR mounted aiming: the shared PANM evaluator now
retains authored animation frames ([D-3DI-3](threedi/3di-gp-format-re.md#retail-panm-animation-frames-2026-09-21)),
and mounted bodies/guns retain fractional carrier attitude through camera and
presentation ([D-INF-27](world/world-wac-ai-re.md#37-mounted-pose-precision-2026-09-21)).
The objective minigun now publishes its witnessed barrel-spin phase while
firing and coasting down ([D-3DI-4](threedi/3di-gp-format-re.md#retail-emplaced-barrel-spin-2026-09-21)).

The [2026-09-18 JO-C validation and fixes](jo-c-validation-2026-09-18.md) cover
guided round lifecycles/motors, shared loaded ammo, blast/indoor collision state,
parachute descent, writable night, reverb selection, specialized textures,
AI callback/controller ownership, player steering/brake transitions, and
unseated helicopter carry with authored NPC boarding/stop orders
([world section 35](world/world-wac-ai-re.md#35-unseated-helicopter-riders-and-reserved-route-orders-2026-09-18)).
Mission saves are excluded; the report distinguishes remaining wider gaps.

The one-page router for "what phase are we in, and where is the next step written
down". It holds no facts of its own: every number and every next step below is a
pointer into the tracked instrument that owns it. When they disagree, the
instrument wins and this page is stale.

The [NPC AI and mission scripting completion work](world/npc-mission-completion.md)
tracks the current shared-engine implementation and normal mission playthrough
gates. Its focused tests and remaining gaps are separate from mission acceptance.
The first playthrough acceptance landed 2026-09-12: `00TRa.bms` played through its
authored sequence on the real input path by the `mission_playthrough` runtime probe
([docs/mcp.md](mcp.md)), the ten gates and the dated verdict in that record. The two
divergences the run surfaced are fixed in the same slice (the HUD declutter level
persisting a death's blank across missions, [interface/hud-re.md](interface/hud-re.md);
the script tick running on through the SP lose epilog,
[world/world-wac-ai-re.md §20.8a](world/world-wac-ai-re.md)).

The #645 follow-ups are implemented in the active parity slice: [class events and squib](world/world-wac-ai-re.md#243b-squib-and-shared-class-effects-2026-09-11), [channel timing](anim/adm-bad-format-re.md#playback-clock-follow-up-2026-09-11), [ordered network effects](net/novaworld-net-re.md#item-explosion-and-state-receive-order-2026-09-11), and [texture conversion](render/render-material-re.md#texture-preprocessing-follow-up-2026-09-11). Their records distinguish the ported scope from pre-existing wider domain gaps.

The [2026-09-13 jo-c audit](jo-c-parity-audit-2026-09-13.md) covers the full active
backlog and adds the untabled material-loader, reverb, particle and savegame gaps.
It orders implementation by state ownership and runtime consumers; the
[ledger](divergence-ledger.md) retains current dispositions.

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
fixtures), landing as one PR of committed-as-we-go slices.
[ADR 0044](adr/0044-shared-native-assets.md) (2026-09-18, PR #653) is its first
follow-on record: one shared `assets::AssetStore` per mounted resource source
for simulation and presentation alike, `runtime/simassets` dissolved into its
owning systems. What led to ADR 0043,
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
rows, the sun feed, the scar gate, the joiner pump; A3 and the rig-twinned half
landed with ADR 0042), the Train C tail, Train D, and the E-rows added by the
2026-09-20 push-down census (a Claude three-agent census reconciled with a
Codex read-only review: the `Simulation` builders, the record bindings only
GUT reads, the loading-screen layout, the HUD latches, the kit model, the
compiled-menu interaction runtime, the remote-body machine, the terrain and
audio lifecycles, the joiner-side client facts, the presentation clocks). The
counter (read it from the baseline, not here) is not yet at a seam-contract
floor; the campaign is finished when it holds only documented seam contracts.
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

Counts come from the [ledger's generated scoreboard](divergence-ledger.md#count-to-zero-scoreboard),
maintained by `scripts/lint/ledger_check.py --check`. The
[dated audit inventory](jo-c-parity-audit-2026-09-13.json) assigns the IDs active at
its master baseline to implementation packages; it is a snapshot, and the ledger
carries every status change since (the PR that landed it already closed part of it).

Each domain's next step is named in its own record, not centrally:

| Domain | Open rows live in | The record that names the next step |
|---|---|---|
| World / AI + gameplay | ledger § World | [world/world-wac-ai-re.md](world/world-wac-ai-re.md) (§14–§33); the #403 client mover/prediction record is [world/vehicle-client-movers-re.md](world/vehicle-client-movers-re.md) (ADR 0026 topology); 2026-09-05: numbered seat keys share the mounted panel list and send confirmed joiner requests (world-wac-ai-re section 23.1, D-AI-11 d); live retail LAN ATV drive/seat changes and restored vehicle panel verified; panel/rider HP lookups translate authored definition IDs (D-NET-157, 2026-09-05); 2026-09-07, PR #640: the full vehicle attempt adds selector-zero and full family motors, amphibious routing, model contact/traction/chassis, mounted and renderer controls, aircraft AI/countermeasures, death/respawn, dedicated sound, wreck bone banks, W1 through W4 trails, rotor wash, foliage sway and water rings. See vehicle-client-movers-re sections 11 through 37; D-ITEM-15 and D-AI-11 are FIXED; the 2026-09-08 review re-grilled the traction, lean, selector-zero, aircraft-brain, sound and dispatcher legs and left D-NET-161 and D-SND-17 OPEN but narrowed to witnessed residuals (pool-3 deck-marker localization, flare off28 target, selector-zero boat part spin, ground-height tap ray kinds, `entity+684` mirror, emplacement brain dispatch; tank fold extra-effect argument, sound-ready gate); D-VEH-2 is the bounded full-bank retirement guard proposed in PR #640, pending maintainer ratification at merge. PR #645: blast-seat semantics, once-per-tick turret slew, parent aircraft weapon limits and non-drivable hover validated. Placement admission and loadout sanitization are now covered by bms-event-runtime-re sections 6.3a/6.4a; the malformed-tail boundary D-EVT-7 is a proposed class-D permanent entry (PR #646), pending maintainer ratification at merge. The bike off-contact launch vector, D-NET-161's former residual (a), is ported (vehicle record §38). 2026-09-11 follow-up: model-aware infantry slopes, global stance sounds and player/NPC burn selection implemented; world record sections 3.5, 17.3b and 17.4b. Vehicle record §38 covers #645 crash-height, bike axle, wheelie and launch-direction corrections. 2026-09-12: the first SP playthrough acceptance (00TRa, ten gates on the real input path) is recorded in [world/npc-mission-completion.md](world/npc-mission-completion.md); the retail in-game RESTART stays unported under the SP epilog/respawn flow (D-AI-10 / D-LOADSCR-8). 2026-09-13: D-WPN-36..39, D-WAC-7..10, D-EVT-8/9 and D-AI-13 FIXED (scoped drift, ADS bias/rotation/optical clock, the shared WAC clock, auto DWORD, V#/M# operands, the post-PreMission variable reset, the signed action count, avoidance quantization); D-WPN-40, the pending-slot pose binding, open; [2026-09-18 movement parity](jo-c-validation-2026-09-18.md#ai-and-player-movement-follow-through): D-AI-14 / D-VEH-3 fixed in PR #652; PR #652 [animation timing audit](world/world-wac-ai-re.md#36-animation-and-relative-motion-timing-audit-2026-09-18): D-INF-26 fixed; current-tick local head and spectator preparation order aligned; visual trails await user comparison; 2026-09-21 scar owner-visibility re-grill and native predicate: world record section 24.9 |
| Item class events | none open (D-ITEM-7 / D-DOOR-2 FIXED 2026-09-11, ledger closure lines) | [World §24.3a](world/world-wac-ai-re.md#243a-class-clocks-regional-shots-barrels-buildings-flags-and-targets) and §24.3b record all #645 class follow-ups; invalid target/model/section limits remain explicit |
| Net (in-match + matchmaking) | ledger § Net (`PAR-NET`) | [net/novaworld-net-re.md](net/novaworld-net-re.md) §8; the build record behind it is `engine/runtime/inmatch/ROADMAP.md`; §5.10 covers joiner vehicle confirmation and shared seat/overlay occupancy; 2026-09-13: numeric self-identity and side-password admission ported; squad challenge remains D-NET-167; [2026-09-19 tank training switch fix](world/special-weapons-parity.md#tank-training-right-click-follow-up) in PR #655; 2026-09-20: the cross-checked netcode parity slice (wire codec/hello, lobby session, service, host registration, in-match cadence/ordering/producers) landed — the corrected rows are D-NET-1/16/17/18/23/26/28/31/38/120/127/139/165/173/182/202/218 in net-re §8; 2026-09-21: [remote-body arbitration re-grill](net/novaworld-net-re.md#remote-body-arbitration-re-grill-2026-09-21) fixes the model-side same-state pending cancellation through a shared native rule |
| UI (HUD, menus, sound, player info) | ledger § UI | [interface/hud-re.md](interface/hud-re.md), [interface/loading-screen-re.md](interface/loading-screen-re.md), [mnu/menu-re.md](mnu/menu-re.md), [playerinfo/avatars-re.md](playerinfo/avatars-re.md), [audio/lwf-dbf-sound-re.md](audio/lwf-dbf-sound-re.md) PR #645: native-aspect sights-card correction ported; live zero/rangefinder feeds and scope camera/readouts are ported (D-HUD-27, PR #650 review). Host rule readback/sentinels and explicit aspect selection are ported; see the host dialog section in menu-re. One-shot layers follow the live listener view before selection and pitch draws (D-SND-19, 2026-09-13). |
| Render (materials, order, lighting, occlusion) | ledger § Render — materials/state, draw order and occlusion (lighting holds no open rows) | [render/README.md](render/README.md); D-RORD-12, D-RLIT-11 and D-OCC-16 minted and closed 2026-09-13 |
| Terrain / foliage / tiles | ledger § Terrain, Foliage (§ Tiles holds no open rows: D-TIL-1..4 FIXED) | [terrain/terrain-re.md](terrain/terrain-re.md), [foliage/foliage-re.md](foliage/foliage-re.md); D-TERRAIN-12 (the empty-sector flat fallback) and D-FOLIAGE-14 (collector capacity) FIXED 2026-09-13 |
| Environment | ledger § Environment | [env/env-tod-re.md](env/env-tod-re.md), [env/env-honored-matrix.md](env/env-honored-matrix.md) PR #650 review: BG_COLOR conversion fixes the 00TRa/00TRg dome seam; see the sky-dome device bridge and windowed regression. |
| Formats (`.mis`, `.ptl`, LW `.3di`, CBIN, fonts, VFS) | ledger, per format | the matching record in [README.md](README.md) |
| Mission savegames | ledger § Mission savegames | [mission/savegame-re.md](mission/savegame-re.md); schema, runtime state restoration and slot lifecycle, D-SAVE-1 |

Work that is **not** a parity divergence — OpenNova Launcher UX, project health, code
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

HUD follow-ups from #645 now include runtime flag/SSKB keys, flag-event audio, the retained centre kill banner, and both death-screen instruction statics. See the [HUD #645 follow-up](interface/hud-re.md#645-follow-ups-flag-feed-announcement-banner-and-death-instructions-2026-09-11) for remaining HUD work.
