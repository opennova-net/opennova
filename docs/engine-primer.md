# Engine primer — the original engine and its conventions

Start here before any engine work. This page is a synthesis and an index: every claim
is drawn from a tracked doc and linked; nothing here is new research, and the `[orig:]`
citations are reused from the records that own them. The deep dives stay where they
are — [runtime-architecture.md](runtime-architecture.md) for the mission loop,
[correspondence.md](correspondence.md) for the symbol↔address parity matrix, the
per-domain RE records for layouts, divergence catalogs, and verdict rationale.

## 1. What the original engine is

NovaLogic's titles share one engine: at a high level each game is a skin of the
previous one with upgraded engine features and different data formats
([GOALS.md](../GOALS.md), "Target games"). The engine is data driven by convention —
weapons, items, terrain, missions, and menus are described by data files and wired
together by convention rather than hardcoded ([GOALS.md](../GOALS.md), "Data driven,
by convention"). OpenNova reimplements it for parity, not reinterpretation: original
content loads as-is, behavior is recovered by reverse engineering the original
binaries, and where we deviate it is a tracked decision, not an accident. Joint
Operations (JO) is the first title being brought up end to end.

## 2. Binaries and IDBs

The canonical reference binary, from the [correspondence.md](correspondence.md) header:

> retail `Jointops.exe` (Joint Operations: Combined Arms), imagebase `0x400000`,
> IDB `Jointops.exe.kong.i64`. All addresses are absolute in that image.

**Rule:** every `[orig: Name @ 0xADDR]` citation means retail `Jointops.exe` unless
the doc says otherwise ([docs/README.md](README.md) conventions). Binaries that do
appear in tracked docs:

| Binary | What it is | Where it is used |
|---|---|---|
| `Jointops.exe` | the shipped JO: Combined Arms executable ("retail", as distinct from the demo) | the default for every citation — [correspondence.md](correspondence.md) |
| `dfx2med.exe` | the DFX2 Mission EDitor; same engine lineage as JO, shared `.bms` format | editor-side cross-checks: [bms-event-runtime-re.md](mission/bms-event-runtime-re.md), [world-wac-ai-re.md](world/world-wac-ai-re.md) |
| `ModSuperOed.exe` | NovaLogic's original mod-tools OED, the 3DI exporter (32-bit PE) | the 3DI3 wire format and its writers: [3di-gp-format-re.md](threedi/3di-gp-format-re.md); also the ground-truth comparator (§5 below) |
| `dfvas.exe` | Delta Force: Black Hawk Down affiliate build | GP-era runtime `.3di` loaders: [3di-gp-format-re.md](threedi/3di-gp-format-re.md) |
| `jodemo.exe` | the JO demo | historical citations only — the env grill re-anchored every jodemo-era address to retail ([env-tod-re.md](env/env-tod-re.md), atmosphere-parity appendix) |

**Cross-title portability: none, unless grilled.** An address is valid only in the
binary it was witnessed in — the env appendix exists because jodemo addresses
mislabeled as Jointops had to be re-anchored one by one. Behavioral correspondence
across binaries is likewise stated only where verified, never assumed from "same
engine".

## 3. Engine-wide conventions

### Fixed point

The engine's recurring scalar encoding is 16.16 fixed point (65536 = 1.0); floats
appear only where a record says so. Known scales, each owned by its record:

| Value | Encoding | Owner |
|---|---|---|
| Time of day | 16.16 **hours** (not HHMM); a day = `0x180000` | [env-tod-re.md](env/env-tod-re.md) |
| `.env` keyword scales | `water_height << 15`, `sky_height << 16`, `fog_level << 16`, `sky_speed << 10`, `curtime = time << 8` (8.24 hours) | [env-tod-re.md](env/env-tod-re.md) |
| World positions / ground | 16.16 world units (e.g. ground clamp `pos[2] = ground + 0x50000` = +5.0) | [world-wac-ai-re.md](world/world-wac-ai-re.md) |
| Infantry root motion | forward step/tick in 16.16; scale `32768 = 65536/2` bakes the ~2-sim-ticks-per-30fps-anim-frame ratio | [world-wac-ai-re.md](world/world-wac-ai-re.md) |
| Angles | BAM: `deg * 2^32/360` (`kBamPerDegree = 11930464`); engine heading = `90 - yaw` | [world-wac-ai-re.md §7](world/world-wac-ai-re.md) |
| 3DI collision geometry | positions/normals packed as i16 16.16 (CVRT/CNRM); plane/bbox distances fp16.16 (CFAC) | [3di-gp-format-re.md](threedi/3di-gp-format-re.md) |
| LOD thresholds | `lod_dist_threshold_q16` (GP) / `lod_threshold_fp16` (3DI3 RMDL) — Q16.16 view distances | [3di-gp-format-re.md](threedi/3di-gp-format-re.md) |
| Skeletal bind matrices | BadBone fp16.16; the decode is `* 1/65536` | [ADR 0007](adr/0007-skeletal-runtime-and-entity-visual.md) |
| PANM part-anim phase | control value 0..65535 = the 16.16 phase; `ANIMTIME` is raw 16.16 seconds | [world-wac-ai-re.md §8](world/world-wac-ai-re.md) |

### Coordinates

- The mission/world frame is **Z-up**; the Godot side is **Y-up**
  ([world-wac-ai-re.md §10](world/world-wac-ai-re.md)). Grid convention:
  "(x,y) = plane, z = up, y inverted"
  `[orig: Mission_LoadBMSAndExtractSpawnPoints @ 0x40d650]`
  ([correspondence.md §3](correspondence.md)).
- The conversion is **single-sourced** in `MissionObjectPlacer.bms_to_godot_basis` /
  `bms_to_godot_position` (`godot/engine/mission/mission_object_placer.gd` statics),
  citing `[orig: Entity_SpawnFromBMSRecord @ 0x40eb66]` +
  `[orig: Math_BuildFixedPointMatrixFromEulerAngles @ 0x613f40]`
  ([runtime-architecture.md](runtime-architecture.md)). **Never re-derive it** — a
  C++ re-derivation was reverted over a godot-cpp `Basis(axis, angle)` sign
  divergence on negative-component axes ([world-wac-ai-re.md §10](world/world-wac-ai-re.md)).
- Heading lives in the ENGINE frame everywhere (`90 - yaw`, BAM-scaled) and converts
  back to mission yaw exactly once, at the present boundary
  ([world-wac-ai-re.md §7](world/world-wac-ai-re.md)).

### Timing

- The engine logic tick is **62 Hz**: `[orig: Game_ProcessMainFrame @ 0x5263f0]`
  increments `current_tick @ 0x24c1968` once per call
  ([bms-event-runtime-re.md §1.6](mission/bms-event-runtime-re.md)).
- Dividers are **per system**, inside each system: the WAC VM executes once per
  **62 ticks** (`[orig: WacScript_AdvanceTick @ 0x4f81a0]`, the 0x3E divider — ours is
  `WacSystem::kTicksPerExecution`, `libs/wac/include/wac/wac_system.h`); normal BMS
  events run a 16-tick gate over a quarter-list cursor (each event evaluated about
  every 64 ticks); the AI/entity motor runs every tick with its own 2/8/16-tick
  stagger (same doc).
- Authoritative per-tick order: **WAC → BMS events → AI**
  ([bms-event-runtime-re.md §1.6](mission/bms-event-runtime-re.md)). How OpenNova's
  hosts drive that loop is [runtime-architecture.md](runtime-architecture.md).

### The entity model, in brief

- Entities live in indexed pools; spawn order is file order: items (pool 1) →
  buildings (pool 2) → markers (pool 3) → organics (pool 0)
  ([bms-event-runtime-re.md §1.7](mission/bms-event-runtime-re.md)).
- The **SSN** (net id) that every trigger/action references is authored in the BMS
  record; lookup matches its low 16 bits across pools 0..3
  (`[orig: EntityPool_FindByNetId @ 0x4f0a20]`, same doc).
- Behavior is **class-keyed**: two tables scanned by the 8-byte items.def class tag —
  an event-callback table (`@ 0x813000`) and a per-frame physics/motor table
  (`@ 0x82abc8`: `org1` = the infantry motor `Entity_UpdateInfantryAI @ 0x4b9910`,
  `CHel`/`cpln` = aircraft, the `cveh`/`ctank` vehicle family)
  ([world-wac-ai-re.md §1](world/world-wac-ai-re.md)).
- AI units carry an 812-byte brain driven by a 24-row state machine (`@ 0x815238`);
  the 0→16 (GROUND_FOLLOWWP) transition fires at the first AI tick, never at spawn
  ([world-wac-ai-re.md](world/world-wac-ai-re.md)).

### Data-driven wiring

Assets reference each other **by name, by convention**: a `.3di` points at its
textures, a `.def` points at a `.3di` and its `.bad` animations, a mission points at
definitions ([GOALS.md](../GOALS.md)). Honoring those conventions instead of
hardcoding is the project's second pillar; modeling them as an asset dependency
graph is the editor's long-term aim (same doc).

### Wire format / network compatibility

The network byte stream is a parity surface like any other. Encoders and decoders are
witnessed against retail and cited inline (`[orig: Name @ 0xADDR]`), exactly like a file
format — the goal is **wire compatibility**: our clients can join original servers, our
servers can serve original clients, and opennova↔opennova works the same way. opennova↔
opennova requires encoder/decoder self-consistency; retail interop requires byte-parity.

The original runs single-player **through** an in-process listen server (socketless
transport, byte loopback), so SP / co-op / MP all share **one** replication path — only
the transport mode differs, never the codec
([ADR 0011](adr/0011-single-player-in-process-listen-server.md);
[ADR 0009](adr/0009-in-match-net-seam.md) for the in-match seam,
[ADR 0012](adr/0012-player-is-host-side-server-entity.md) for the player entity). The
protocol RE record is [net/novaworld-net-re.md](net/novaworld-net-re.md).

## 4. Subsystem index

Verdicts below are a snapshot for orientation; [correspondence.md §1](correspondence.md)
is authoritative, and each record owns the rationale and the stable `D-…` divergence ids.

| Subsystem | Our code | Record | Verdict |
|---|---|---|---|
| Menus (MNU/MNS UI) | `libs/mnu` (incl. the mnu_xml reader), `libs/mns`, `godot/engine/mnu` | [mnu/menu-re.md](mnu/menu-re.md) + [menu-wiring.md](mnu/menu-wiring.md) | matching (D-MNU-1..3) |
| Sound banks + dialog | `libs/lwf`, `libs/dbf`, `libs/audio` | [audio/lwf-dbf-sound-re.md](audio/lwf-dbf-sound-re.md) | matching (D-SND-1..3) |
| Music (MUS/SBF/SCR) | `libs/mus`, `libs/sbf`, `libs/scr` | [audio/mus-sbf-re.md](audio/mus-sbf-re.md) | matching per component |
| Environment / time-of-day | `libs/env`, `env_render`, the `NovaEnvironment` family | [env/env-tod-re.md](env/env-tod-re.md) | mixed per subsystem (see record) |
| String tables (RTXT) | `libs/rtxt`, NovaStrings | [interface/rtxt-strings-re.md](interface/rtxt-strings-re.md) | matching at byte level (98/98) |
| Mission loader (`.bms`) | `libs/mission` | [correspondence.md §3](correspondence.md) | per function |
| BMS event runtime + promotion | `libs/mission` | [mission/bms-event-runtime-re.md](mission/bms-event-runtime-re.md) | matching (D-EVT-1..4) |
| World / WAC VM / AI / gameplay systems | `libs/world`, `libs/wac` | [world/world-wac-ai-re.md](world/world-wac-ai-re.md) | matching core, largest live surface: §14–§27 carry the infantry motor, collision + blink boxes, ground-AI combat, fire/death presentation, the round-outcome loop, player-body physics, the vehicle pass, item destruction, tracers, allegiance/mounted weapons, and throwables. Catalogs D-INF / D-COL / D-AI / D-ITEM / D-WPN / D-THROW; open rows in the [ledger](divergence-ledger.md) |
| Items (items.def entity defs) | `libs/def`, `NovaItemDatabase` | [world/itemdef-re.md](world/itemdef-re.md) | landed |
| HUD + interface overlays | `godot/engine/ui/hud_*.gd`, `NovaHudPos`, `game_hud.gd` | [interface/hud-re.md](interface/hud-re.md) | ported (weapon-coupled elements, waypoint track, heat bar, objectives panel, attach labels; D-HUD-1..18) |
| Mission loading screen | `godot/engine/ui/nova_loading_screen.gd` | [interface/loading-screen-re.md](interface/loading-screen-re.md) | matching for sidecar rule / MP text / bar (D-LOADSCR-1..6) |
| Player info / avatars | `libs/avatars`, `NovaAvatarDatabase`, `player_info_menu_companion.gd` | [playerinfo/avatars-re.md](playerinfo/avatars-re.md) | matching for parser + screen orchestration (D-PLAYERINFO-1..12; loadout combos open) |
| Render — materials / state | `libs/oed` tag registry, `libs/renderer`, `NovaObjectShaderCache` | [render/render-material-re.md](render/render-material-re.md) | matching (REN-2; D-RMAT catalog) |
| Render — draw order | `libs/renderer` render_order + the engine priority ladder | [render/render-order-re.md](render/render-order-re.md) | matching for the ported ladder (REN-3; D-RORD catalog) |
| Render — lighting | `libs/renderer/light_runtime`, `libs/env::ModulatorChain` | [render/render-lighting-re.md](render/render-lighting-re.md) | matching for the ported chain (REN-5; D-RLIT catalog) |
| Render — occlusion / blink boxes | `libs/world/occlusion`, `GameWorld` frame gates | [render/render-occlusion-re.md](render/render-occlusion-re.md) | landed 2026-07-16 (sound occlusion + indoor gates ported; section-mask/portal engine is the follow-up slice; D-OCC-1..8) |
| Skeletal animation (`.bad`/`.adm`) | `libs/anim`, NovaSkeletalAnim | [ADR 0007](adr/0007-skeletal-runtime-and-entity-visual.md) | implemented (deferrals listed there) |
| Models (`.3di`: 3DI3 + GP) | `libs/threedi` | [threedi/3di-gp-format-re.md](threedi/3di-gp-format-re.md) | landed format record |
| Models (Land Warrior `.3di`) | — | [threedi/3di-lw-format-re.md](threedi/3di-lw-format-re.md) | unlanded (PR #45 closed) |
| Particles (`.ptl`) | `libs/particle`, `libs/renderer` particle path, `godot/engine/particle` (NovaEffectScene / NovaParticleFile) | [particles/ptl-format-re.md](particles/ptl-format-re.md) | landed (#237); D-PTL catalog in the ledger |
| NovaWorld networking | `libs/npwire`, `libs/novaworld`, `libs/napi`, `libs/novacrypto`, `libs/netsim`, `apps/novaworld_server`, `godot/engine/network` | [net/novaworld-net-re.md](net/novaworld-net-re.md) | landed + maturing (backend + SP listen server; in-match decode byte-witnessed, encode in progress) |
| Boot-required resources | `libs/gameprofile` `required_resources` manifest (ENG-6), consumed via `NovaResourceRoot.list_missing_boot_resources` | [required-resources.md](required-resources.md) | witnessed (R8) + manifest landed: the fatal set, per-resource failure behavior, boot order, D-BOOT catalog |
| VFS / PFF mount stack | `libs/vfs`, `libs/pff`, `NovaResourceRoot` | [vfs/vfs-pff-mount-re.md](vfs/vfs-pff-mount-re.md) | witnessed (PAR-R7): mount, precedence, /d gate; D-VFS-1..9 |
| Terrain (TRN + runtime queries) | `libs/terrain`, `libs/terrain_query` | [terrain/terrain-re.md](terrain/terrain-re.md) | partial record (PAR-R1; runtime queries ported, ENG-3; rendering parity re-grilled on #245) |
| Foliage | `libs/foliage`, `NovaFoliageDispatcher` | [foliage/foliage-re.md](foliage/foliage-re.md) | matching incl. the model tier (runtime rebuilt on #245; D-FOLIAGE-7/9/10 open) |
| Tiles (`.til` overlay) | `libs/til` | [tiles/til-re.md](tiles/til-re.md) | landed (PAR-R3; D-TIL-1) |
| Fonts (`.fnt`) | `libs/fnt` | [fonts/fnt-re.md](fonts/fnt-re.md) | landed (PAR-R4; D-FNT-1..4) |
| Credits (CBIN) | `libs/cbin` | [credits/cbin-re.md](credits/cbin-re.md) | partial (PAR-R5: codec matching; markup NEEDS-RE) |
| Importer pipeline | `apps/importer`, `pyopennova` | [importer/importer-audit.md](importer/importer-audit.md) | tracked-by-composition (PAR-R6) |

Every subsystem now has a dedicated RE record (full or partial) or a
tracked-by-composition audit — the PAR-R1..R7 sweep (2026-07-05) landed the last seven
(terrain, foliage, tiles, fonts, credits, the importer, and the VFS/PFF mount stack; the
PFF write side is [ADR 0008](adr/0008-pff-writer-policy.md)). The full per-record status
table is [docs/README.md](README.md).

## 5. Researching the engine

Order of operations when you need an engine truth:

1. **[correspondence.md](correspondence.md) first.** The master join of reimpl symbol ↔
   original address with per-system verdicts; `addr` is the join key (names drift
   across IDB passes, addresses don't). If the system has a row, the answer probably
   already exists.
2. **The domain RE record** ([docs/README.md](README.md) table) — format layouts,
   witness maps, the divergence catalog, and verdict rationale.
3. **Live IDA via `ida-pro-mcp`** — an MCP server bridging live IDA instances
   (decompile/disasm, xrefs/callgraphs, byte/regex search, write-backs behind
   confidence gates). Optional local tooling: configured at user level on the
   maintainer's machine and not guaranteed for contributors — which is why findings
   must land in the tracked docs. Two skills drive it:
   - **How does the original do X?** → the `engine-research` skill
     (`.claude/skills/engine-research/`): hunts the function from strings/data/
     callgraph, witnesses the behavior, and lands durable findings via `re-doc`.
   - **Does our reimplementation match?** → the `grill-ida` skill
     (`.claude/skills/grill-ida/`): interrogates reimpl vs binary axis by axis and
     ends in a per-system verdict, landed via the `re-doc` skill.
4. **Ground truth without IDA:**
   - ModSuperOed comparator — drive the retail exporter headlessly and byte-compare
     its `.3di` output with ours: `tools/modsuperoed` (32-bit hook DLL + injector) +
     `apps/modsuperoed.py`, fixture pack in `third_party/modsuperoed`, gated on
     `OPENNOVA_MODSUPEROED_DIR`.
   - Byte-exact fixture roundtrips in ctest — e.g. `tests/rtxt/real_parity_test.cpp`
     (98/98 retail bins), `tests/terrain/dvd4_parity_test.cpp`, the `.bad`/3DI
     roundtrips under `tests/<domain>/`.
   - Retail-install sweeps and corpus tests, gated on env vars (`OPENNOVA_JO_DIR`,
     `OPENNOVA_MISSION_CORPUS`, `OPENNOVA_JO_ASSETS` — see the root CLAUDE.md).
5. **Runtime introspection:**
   - The debug overlay (F3 in the standalone game): Entities/Sim/Vars/Perf
     tabs, sim transport (play/pause/step), and mission-variable writes —
     `godot/engine/debug/nova_debug_overlay.gd`. ONED launches that same game
     with F5, or its current saved loose mission with F6; it has no embedded
     mission preview or debug overlay.
   - `NovaSimulation` introspection: `get_present_snapshot()`,
     `get_entity_debug(index)`, `get_fired_events_snapshot()`, `get_wac_state()`,
     and the mission/global/music variable snapshots.
   - PerfTimeline ring (`godot/engine/util/perf_timeline.gd`), rendered in the
     overlay's Perf pane; recorded baselines in `docs/perf/`.
   - Headless probes `godot/tests/*_probe.gd` (menu_shell, mission_load_perf,
     mission_reground_perf, runtime_scene, trn_project_roundtrip).

## 6. Evidence and landing rules

Research flows one way into the tree ([docs/README.md](README.md) conventions):

- Implementing "our own version" of engine behavior is never allowed — every system
  is a faithful port of the witnessed original unless a tracked decision (ADR or an
  RE-record divergence entry) says otherwise. "Implement X" therefore always means
  witness-then-port: establish how the original did X — its behavior and its look —
  through the research path in §5 before writing any of it. The exclusion is
  CRT/OS/platform primitives (`strcpy`/`sprintf`/`memcpy`, D3D, file I/O): those map
  to standard-library or platform equivalents rather than being ported.
- There is no scratch directory: `docs/` is kept pristine — the best current
  understanding of the original engine — and durable findings land there the same
  session they are witnessed; ephemeral working state stays in the session.
- Ports cite their witness inline at the port site: `[orig: Name @ 0xADDR]`.
- Durable findings land in the domain RE record — the `re-doc` skill authors
  the house format (verdict table, witness map, stable `D-<DOMAIN>-n` divergence
  catalog) — or in an ADR when the finding is a decision.
- No raw decompiled code is ever committed; behavior is summarized and cited.
- Divergence from the original is a tracked decision, never an accident
  ([GOALS.md](../GOALS.md)).
