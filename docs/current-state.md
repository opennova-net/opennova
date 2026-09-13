# Where the project is, and what is next

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

Counts come from the [ledger's generated scoreboard](divergence-ledger.md#count-to-zero-scoreboard),
maintained by `scripts/lint/ledger_check.py --check`. The
[dated audit inventory](jo-c-parity-audit-2026-09-13.json) assigns every active ID
to an implementation phase; use the ledger for subsequent status changes.

The mission startup reset now follows the complete PreMission pass (D-EVT-8);
[bms-event-runtime-re](mission/bms-event-runtime-re.md) records the ordering and regression.

One-shot sound layers now follow the live listener view before selection and pitch
randomness (D-SND-19; [sound record](audio/lwf-dbf-sound-re.md)).

Each domain's next step is named in its own record, not centrally:

| Domain | Open rows live in | The record that names the next step |
|---|---|---|
| World / AI + gameplay | ledger § World | [world/world-wac-ai-re.md](world/world-wac-ai-re.md) (§14–§33); the #403 client mover/prediction record is [world/vehicle-client-movers-re.md](world/vehicle-client-movers-re.md) (ADR 0026 topology); 2026-09-05: numbered seat keys share the mounted panel list and send confirmed joiner requests (world-wac-ai-re section 23.1, D-AI-11 d); live retail LAN ATV drive/seat changes and restored vehicle panel verified; panel/rider HP lookups translate authored definition IDs (D-NET-157, 2026-09-05); 2026-09-07, PR #640: the full vehicle attempt adds selector-zero and full family motors, amphibious routing, model contact/traction/chassis, mounted and renderer controls, aircraft AI/countermeasures, death/respawn, dedicated sound, wreck bone banks, W1 through W4 trails, rotor wash, foliage sway and water rings. See vehicle-client-movers-re sections 11 through 37; D-ITEM-15 and D-AI-11 are FIXED; the 2026-09-08 review re-grilled the traction, lean, selector-zero, aircraft-brain, sound and dispatcher legs and left D-NET-161 and D-SND-17 OPEN but narrowed to witnessed residuals (pool-3 deck-marker localization, flare off28 target, selector-zero boat part spin, ground-height tap ray kinds, `entity+684` mirror, emplacement brain dispatch; tank fold extra-effect argument, sound-ready gate); D-VEH-2 is the bounded full-bank retirement guard proposed in PR #640, pending maintainer ratification at merge. PR #645: blast-seat semantics, once-per-tick turret slew, parent aircraft weapon limits and non-drivable hover validated. Placement admission and loadout sanitization are now covered by bms-event-runtime-re sections 6.3a/6.4a; the malformed-tail boundary D-EVT-7 is a proposed class-D permanent entry (PR #646), pending maintainer ratification at merge. The bike off-contact launch vector, D-NET-161's former residual (a), is ported (vehicle record §38). 2026-09-11 follow-up: model-aware infantry slopes, global stance sounds and player/NPC burn selection implemented; world record sections 3.5, 17.3b and 17.4b. Vehicle record §38 covers #645 crash-height, bike axle, wheelie and launch-direction corrections. 2026-09-12: the first SP playthrough acceptance (00TRa, ten gates on the real input path) is recorded in [world/npc-mission-completion.md](world/npc-mission-completion.md); the retail in-game RESTART stays unported under the SP epilog/respawn flow (D-AI-10 / D-LOADSCR-8). |
| Item class events | none open (D-ITEM-7 / D-DOOR-2 FIXED 2026-09-11, ledger closure lines) | [World §24.3a](world/world-wac-ai-re.md#243a-class-clocks-regional-shots-barrels-buildings-flags-and-targets) and §24.3b record all #645 class follow-ups; invalid target/model/section limits remain explicit |
| Net (in-match + matchmaking) | ledger § Net (`PAR-NET`) | [net/novaworld-net-re.md](net/novaworld-net-re.md) §8; the build record behind it is `engine/runtime/inmatch/ROADMAP.md`; §5.10 covers joiner vehicle confirmation and shared seat/overlay occupancy; 2026-09-13: numeric self-identity and side-password admission ported; squad challenge remains D-NET-167 |
| UI (HUD, menus, sound, player info) | ledger § UI | [interface/hud-re.md](interface/hud-re.md), [interface/loading-screen-re.md](interface/loading-screen-re.md), [mnu/menu-re.md](mnu/menu-re.md), [playerinfo/avatars-re.md](playerinfo/avatars-re.md), [audio/lwf-dbf-sound-re.md](audio/lwf-dbf-sound-re.md) PR #645: native-aspect sights-card correction ported; manual zero/rangefinder remain deferred. Host rule readback/sentinels and explicit aspect selection are ported; see the host dialog section in menu-re. |
| Render (materials, order, lighting, occlusion) | ledger § Render — materials/state, draw order and occlusion (lighting holds no open rows) | [render/README.md](render/README.md) |
| Terrain / foliage / tiles | ledger § Terrain, Foliage (§ Tiles holds no open rows: D-TIL-1..4 FIXED) | [terrain/terrain-re.md](terrain/terrain-re.md), [foliage/foliage-re.md](foliage/foliage-re.md) |
| Environment | ledger § Environment | [env/env-tod-re.md](env/env-tod-re.md), [env/env-honored-matrix.md](env/env-honored-matrix.md) |
| Formats (`.mis`, `.ptl`, LW `.3di`, CBIN, fonts, VFS) | ledger, per format | the matching record in [README.md](README.md) |
| Mission savegames | ledger § Mission savegames | [mission/savegame-re.md](mission/savegame-re.md); schema, runtime state restoration and slot lifecycle, D-SAVE-1 |

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

HUD follow-ups from #645 now include runtime flag/SSKB keys, flag-event audio, the retained centre kill banner, and both death-screen instruction statics. See the [HUD #645 follow-up](interface/hud-re.md#645-follow-ups-flag-feed-announcement-banner-and-death-instructions-2026-09-11) for remaining HUD work.

2026-09-13 scoped-aim correction (D-WPN-36): DEF stability defaults and authored values now reach the local body oscillator. Camera aim and fired rounds consume the resulting persistent heading/pitch; seven original-executable stance/scaling cases plus the full body path and mission replacement carry are covered by the focused native regressions. See [world §14.9](world/world-wac-ai-re.md#149-scoped-weapon-stability-and-persistent-aim-drift-2026-09-13).

2026-09-13 empty-sector correction (D-TERRAIN-12): the native terrain frame and Godot draw/cache path now retain the original flat fallback. Focused regressions cover culling, topology, texture composition, and origin-sector cache borrowing; the shader contract checks blendmap collapse with independent detail/noise. The four asset-page golden hashes pass against the Combined Arms install.

2026-09-13 WAC clock correction (D-WAC-7): script writes, startup execution, baseline restoration, and live program replacement update the same clock used by WAC/BMS admission. The logic tick freezes its decision before either scheduler executes; diagnostic run counts no longer overwrite it. Missing/empty scripts follow the original startup epilog. Native mission, event and VM integration regressions pass.

2026-09-13, D-WPN-37: Airborne players suppress authored first-person ADS position bias while scope interpolation continues; reload, NoCardSwitch and ForceScoped keep their existing optical policy. See [world §14.10](world/world-wac-ai-re.md). Native `local_player_view` regressions pass.
