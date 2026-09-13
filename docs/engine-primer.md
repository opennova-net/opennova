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
| `TrnGen.exe` | NovaLogic's terrain build tool (heightmap → `.trn`/`.tml`/`.tms`) | historical bake research retained in [terrain-re.md](terrain/terrain-re.md); the editor-love hard cut removed OpenNova's legacy terrain builder |
| `ParticleEdit_v1_1.exe` | NovaLogic's `.ptl` particle editor | `.ptl` semantics: [ptl-format-re.md](particles/ptl-format-re.md) (`engine/formats/particle`) |
| `Dflw.exe` | the Delta Force: Land Warrior executable | LW-era `.3di` loaders: [3di-lw-format-re.md](threedi/3di-lw-format-re.md) |
| `misldr.dll` | the JO mission loader DLL | the `.mis` editor-side format: [mis-format-re.md](mission/mis-format-re.md) |
| `binkw32.dll` | the Bink video decoder the game ships | the intro/menu video decode law: [menu-re.md](mnu/menu-re.md) |

Addresses cited from `TrnGen.exe`, `ParticleEdit_v1_1.exe`, and `Dflw.exe` are
labeled at their citation sites; an unlabeled address is retail `Jointops.exe`.

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
| LOD thresholds | `lod_dist_threshold_q16` (GP) is a Q16.16 view-distance field; `lod_threshold_fp16` (3DI3 RMDL) is instead compared with the model sphere's projected screen radius after the frame's resolution/detail normalization | [3di-gp-format-re.md](threedi/3di-gp-format-re.md), [render-order-re.md](render/render-order-re.md) |
| Skeletal bind matrices | BadBone fp16.16; the decode is `* 1/65536` | [ADR 0007](adr/0007-skeletal-runtime-and-entity-visual.md) |
| PANM part-anim phase | control value 0..65535 = the 16.16 phase; `ANIMTIME` is raw 16.16 seconds | [world-wac-ai-re.md §8](world/world-wac-ai-re.md) |

### Coordinates

- The mission/world frame is **Z-up**; the Godot side is **Y-up**
  ([world-wac-ai-re.md §10](world/world-wac-ai-re.md)). Grid convention:
  "(x,y) = plane, z = up, y inverted"
  `[orig: Mission_LoadBMSAndExtractSpawnPoints @ 0x40d650]`
  ([correspondence.md §3](correspondence.md)).
- The conversion is **single-sourced** in `MissionObjectPlacer.bms_to_godot_basis` /
  `bms_to_godot_position` (`godot/src/mission/mission_object_placer.cpp` statics),
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
  `WacSystem::kTicksPerExecution`, `engine/runtime/wac/wac_system.h`); normal BMS
  events run a 16-tick gate over a quarter-list cursor (each event evaluated about
  every 64 ticks); the AI/entity motor runs every tick with its own 2/8/16-tick
  stagger (same doc).
- Authoritative per-tick order: **WAC → BMS events → AI**
  ([bms-event-runtime-re.md §1.6](mission/bms-event-runtime-re.md)). How OpenNova's
  hosts drive that loop is [runtime-architecture.md](runtime-architecture.md).

The active [NPC AI and mission scripting completion work](world/npc-mission-completion.md)
uses this shared tick and entity model. Script dispatch coverage and focused
tests do not by themselves establish a completed mission playthrough.

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
hardcoding is the project's second pillar. Format tools write the canonical files
directly, and the runtime resolves their names without an editor-owned project or
asset database (same doc; [ADR 0037](adr/0037-oned-runs-game-data.md)).

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
| Menus (MNU/MNS UI) | `engine/formats/mnu` (incl. the mnu_xml reader), `engine/formats/mns`, `godot/src/mnu` | [mnu/menu-re.md](mnu/menu-re.md) + [menu-wiring.md](mnu/menu-wiring.md) | record complete (D-MNU-1..12; open rows tabled in the ledger); host dialog readback and explicit aspect selection verified 2026-09-11 |
| Sound banks + dialog | `engine/formats/lwf`, `engine/formats/dbf`, `engine/runtime/audio` | [audio/lwf-dbf-sound-re.md](audio/lwf-dbf-sound-re.md) | record scoped to ported sound behavior (D-SND-1..19; one-shot listener-view gate verified; reverb selector/DSP remains OPEN + NEEDS-RE) |
| Music (MUS/SBF/SCR) | `engine/formats/mus`, `engine/formats/sbf`, `engine/formats/scr` | [audio/mus-sbf-re.md](audio/mus-sbf-re.md) | matching per component |
| Environment / time-of-day / weather | `engine/formats/env` (the parse, TOD and weather math), `engine/runtime/world/weather_state` (the ONE weather home the WAC handlers, the sim tick, the wire and the F3 window share), `engine/runtime/environment` (the render owner, the seed, the precipitation pool), `engine/runtime/renderer/precipitation_frame`, the `MissionEnvironment`/`Weather`/`Precipitation` nodes | [env/env-tod-re.md](env/env-tod-re.md) | matching (the weather port closed the deferred rows 2026-08-30; residuals per subsystem in the record) |
| String tables (RTXT) | `engine/formats/rtxt`, Strings | [interface/rtxt-strings-re.md](interface/rtxt-strings-re.md) | matching at byte level (98/98) |
| Mission loader (`.bms`) | `engine/formats/mission` | [correspondence.md §3](correspondence.md) | per function |
| Mission savegames | Runtime snapshot/restore owner unported | [mission/savegame-re.md](mission/savegame-re.md) | D-SAVE-1 NEEDS-RE; recovered routines do not establish full live save/load |
| BMS event runtime + promotion | `engine/runtime/mission` | [mission/bms-event-runtime-re.md](mission/bms-event-runtime-re.md) | matching core (§10 structural action closure 2026-08-30; open D-EVT-1/-3 in the ledger); placement admission and loadout sanitizer verified 2026-09-11; D-EVT-7 records malformed inputs  post-PreMission numbered-variable reset verified (D-EVT-8, 2026-09-13) |
| World / WAC VM / AI / gameplay systems | `engine/runtime/world`, `engine/runtime/wac`, `engine/formats/grm` (GRM facial rigs, §33.30) | [world/world-wac-ai-re.md](world/world-wac-ai-re.md) | matching core, largest live surface: §14–§33 carry the infantry motor, collision + blink boxes, ground-AI combat, fire/death presentation, the round-outcome loop, player-body physics, the vehicle pass, item destruction, tracers, allegiance/mounted weapons, throwables, the water/deck-ride channel, and the ladder climb state machine. Catalogs D-INF / D-COL / D-AI / D-ITEM / D-WPN / D-THROW / D-VEH; open rows in the [ledger](divergence-ledger.md); 2026-08-21: §24.9 impact scars, §14.6 the mounted camera, §25.9 the round-effect lifecycle, the spring leg and the part-animation registers PORTED; 2026-08-30: §32 WAC rows + the queued ChangeAI command path PORTED; §27.6a (2026-09-04) retail mission minefields `lndm` ported and validated on `00TRd`/`CP09`, D-THROW-6 closed; 2026-09-05: numbered seat keys share the mounted panel list and send confirmed joiner requests (world-wac-ai-re section 23.1, D-AI-11 d); 2026-09-07, PR #640: the full vehicle attempt adds selector-zero and full family motors, amphibious routing, model contact/traction/chassis, mounted and renderer controls, aircraft AI/countermeasures, death/respawn, dedicated sound, wreck bone banks, W1 through W4 trails, rotor wash, foliage sway and water rings. See vehicle-client-movers-re sections 11 through 36; D-ITEM-15 and D-AI-11 are FIXED; D-NET-161 and D-SND-17 are OPEN, narrowed at the 2026-09-08 review to witnessed residuals (ledger rows); D-VEH-2 is the bounded full-bank retirement guard proposed in PR #640, pending maintainer ratification at merge. PR #645 validates blast-seat semantics and turret cadence, and closes the aircraft parent-clamp and hover follow-ups. 2026-09-11 follow-up: model-aware infantry slopes, global stance sounds and player/NPC burn selection implemented; world record sections 3.5, 17.3b and 17.4b. Vehicle record §38 records the #645 crash-height and bike wheelie/axle follow-up. |
| Item event callbacks | `engine/runtime/world/item_events.cpp`, `simassets/item_traits.cpp` | [World §24.3a](world/world-wac-ai-re.md#243a-class-clocks-regional-shots-barrels-buildings-flags-and-targets): pool-specific countdowns, regional shots, target sections, flag return, crane/building collapse and emitter group lifetime |
| Items (items.def entity defs) | `engine/formats/def`, `ItemDatabase` | [world/itemdef-re.md](world/itemdef-re.md) | landed |
| HUD + interface overlays | `engine/runtime/hud` (`HudFrameCompiler` + `hud_math`; `hud_minimap` — `HudMinimapCompiler`/`HudMapControl`; the `hud_declutter` module), `engine/runtime/world` `minimap_overlay`/`minimap_footprint`, `engine/runtime/replication` `client_replica_minimap`, `godot/src/hud` (`HudOverlay`/`HudPos`) | [interface/hud-re.md](interface/hud-re.md) | ported (weapon-coupled elements, waypoint track, heat bar, objectives panel, attach labels, friendly tags, the gameplay spinmap + M-map, the HUDDECLUT declutter system; D-HUD-1..25 — open rows in the ledger); 2026-08-21: the mounted-vehicle panel, the Recent Messages window + the chat ring, the AAS zone status panel and the map medic marker ported; 2026-08-24: the MP end-of-round presentation (D-HUD-25) closed, the S2C 0x1E message feed (D-HUD-23) and the Tab scoreboard (D-HUD-24) partial; 2026-09-05: mounted panel and remote rider HP definition-ID handoffs repaired, with live retail LAN panel/seat/drive checks (D-NET-157) PR #645 adds shared native-aspect sights-card scaling with pixel rounding. |
| Mission loading screen | `godot/game/ui/loading_screen.gd` | [interface/loading-screen-re.md](interface/loading-screen-re.md) | matching for sidecar rule / MP text / bar / SP splash (D-LOADSCR-1..8; re-verified 2026-08-15) |
| Player info / avatars | `engine/formats/avatars`, `AvatarDatabase`, `player_info_menu_companion.gd`, `MissionObjectPlacer` | [playerinfo/avatars-re.md](playerinfo/avatars-re.md) | matching for parser, screen orchestration, active-slot per-side persistence/network identity, selected world/first-person composition, raw per-part `TEX_CAMO` controls, and packed-id head-sex projection into player sound profiles (D-PLAYERINFO-1 and D-SND-12 FIXED 2026-08-15; -12's per-team memory ported; open: -12's five-slot selector, -9's edited kit pages + `player.sav` options) |
| Render — materials / state | `engine/runtime/renderer` material registry, `ObjectShaderCache` | [render/render-material-re.md](render/render-material-re.md) | matching for the ported material/state slice; dedicated texture loaders remain OPEN + NEEDS-RE (D-RMAT-12) |
| Render — draw order | `engine/runtime/renderer` render_order + the engine priority ladder | [render/render-order-re.md](render/render-order-re.md) | matching for the ported ladder (REN-3; D-RORD catalog) |
| Render — lighting | `engine/runtime/renderer/light_runtime`, `engine/formats/env::ModulatorChain` | [render/render-lighting-re.md](render/render-lighting-re.md) | matching for the ported chain (REN-5; D-RLIT catalog); 2026-08-21: the per-light terrain projected pass ported; D-RLIT-4 closed 2026-08-23 after the max-quality foliage selector was proved inert |
| Render — occlusion / blink boxes | `engine/runtime/world/occlusion`, `GameWorld` frame gates | [render/render-occlusion-re.md](render/render-occlusion-re.md) | landed 2026-07-16 (sound occlusion + indoor gates ported; section-mask/portal engine ported 2026-07-17 — init, mask build, traversal, occluder culling (engine/runtime/world/occlusion.cpp); D-OCC-1..15 — 1..8 record-only witness details, 9..15 tabled/registered 2026-08-30) |
| Skeletal animation (`.bad`/`.adm`) | `engine/runtime/anim`, SkeletalAnim | [ADR 0007](adr/0007-skeletal-runtime-and-entity-visual.md) | implemented (deferrals listed there) |
| Models (`.3di`: 3DI3, consumed directly; the GP era is documented only, ADR 0027) | `engine/formats/threedi` | [threedi/3di-gp-format-re.md](threedi/3di-gp-format-re.md) | landed format record |
| Models (Land Warrior `.3di`) | — | [threedi/3di-lw-format-re.md](threedi/3di-lw-format-re.md) | unlanded (PR #45 closed) |
| Particles (`.ptl`, plus the `.ptu`/`.ptg` gore sets #563 made first-class) | `engine/runtime/particle`, `engine/runtime/renderer` particle path, `godot/src/particle` (EffectScene / ParticleFile) | [particles/ptl-format-re.md](particles/ptl-format-re.md) | landed (#237); ownership/child/ORBIT/subframe/collision residuals D-PTL-26..30 tabled in the ledger |
| NovaWorld networking | `engine/net/npwire`, `engine/net/novaworld`, `engine/net/napi`, `engine/net/novacrypto`, `engine/runtime/replication`, `apps/novaworld_server`, `godot/src/network` | [net/novaworld-net-re.md](net/novaworld-net-re.md) | landed + maturing (backend + SP listen server; in-match replication exercised in both directions against captures and live retail sessions (retail clients join and play on our hosts); remaining gaps ledgered; §5.10 records the 2026-09-04/05 joiner vehicle/seat-query pass); 2026-09-13: numeric self-identity and side-password admission ported; squad challenge remains D-NET-167 |
| Boot-required resources | `engine/base/gameprofile` `required_resources` manifest (ENG-6), consumed via `ResourceRoot.list_missing_boot_resources` | [required-resources.md](required-resources.md) | witnessed (R8) + manifest landed: the fatal set, per-resource failure behavior, boot order, D-BOOT catalog |
| VFS / PFF mount stack | `engine/base/vfs`, `engine/formats/pff`, `ResourceRoot` | [vfs/vfs-pff-mount-re.md](vfs/vfs-pff-mount-re.md) | witnessed (PAR-R7): mount, precedence, /d gate; D-VFS-1..11 |
| Terrain (TRN + runtime queries) | `engine/runtime/terrain`, `engine/runtime/terrain_query` | [terrain/terrain-re.md](terrain/terrain-re.md) | partial record (PAR-R1; runtime queries ported, ENG-3; rendering parity re-grilled on #245) |
| Foliage | `engine/formats/foliage`, `FoliageDispatcher` | [foliage/foliage-re.md](foliage/foliage-re.md) | matching incl. the model tier (runtime rebuilt on #245; D-FOLIAGE-7/9/10 open) |
| Tiles (`.til` overlay) | `engine/formats/til` | [tiles/til-re.md](tiles/til-re.md) | landed (PAR-R3; D-TIL-1..4 all FIXED, the last 2026-08-20) |
| Fonts (`.fnt`) | `engine/formats/fnt` | [fonts/fnt-re.md](fonts/fnt-re.md) | landed (PAR-R4; D-FNT-1..4) |
| Credits (CBIN) | `engine/formats/cbin` | [credits/cbin-re.md](credits/cbin-re.md) | partial (PAR-R5: codec matching; markup NEEDS-RE) |

Every subsystem now has a dedicated RE record (full or partial) — the PAR sweep
(2026-07-05) landed terrain, foliage, tiles, fonts, credits, and the VFS/PFF mount stack; the
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
   - **Has the three-way link drifted?** → `scripts/ida/cite_sweep.py` joins every
     `[orig:` marker (the one citation form, ADR 0042 d7) in code and docs against the live IDB by address and
     prints the disagreements (stale names, auto-names, undefined or other-image
     addresses, stale `reimpl:` back-links); run it after a merge train and before a
     release (grill-ida `LIFECYCLE.md` §4).
   - Byte-exact fixture roundtrips in ctest — e.g. `tests/rtxt/real_parity_test.cpp`
     (98/98 retail bins), `tests/terrain/cdep_roundtrip_test.cpp` and
     `tests/terrain/trn_config_roundtrip_test.cpp`, the `.bad`/3DI
     roundtrips under `tests/<domain>/`.
   - Retail-install sweeps and corpus tests, gated on the two documented roots (`OPENNOVA_JO_DIR`,
     `OPENNOVA_JO_ASSETS` — see docs/asset-gated-tests.md).
5. **Runtime introspection:**
   - The dev tools (F3 in the standalone game, ADR 0039): engine-owned Dear ImGui
     windows under `engine/runtime/devtools/` — the Stats window over the
     frame-stats board and the Entities window over
     `world::inspect::entity_directory` (records in, typed requests out, ADR
     0042 d6), further inspection/control windows as they are wanted (`engine/runtime/devtools/README.md` is the recipe). Debug builds
     only. ONED's Run OpenNova loose action launches that same standalone game
     against the selected data directory; ONED has no embedded preview or dev
     tools (its own run surface is an engine ImGui window on the same pass).
   - `Simulation` introspection: `get_present_snapshot()`, the typed
     `entity_directory()` / `entity_card(handle)` records (`world::inspect`,
     ADR 0042 d5), `get_fired_events_snapshot()`,
     and the mission/global/music variable snapshots.
   - LoadTimeline ring (`godot/src/world/load_timeline.h`; its Perf page went
     with the overlay — a dev-tools window is the re-home); recorded baselines
     in `docs/perf/`.
   - Runtime probes: `game_probe` tools under `godot/probes/`, driven through the
     game MCP (`docs/mcp.md`, ADR 0041); an assertion over the portable engine is a
     ctest.

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

The [HUD #645 follow-up](interface/hud-re.md#645-follow-ups-flag-feed-announcement-banner-and-death-instructions-2026-09-11) refutes two IDB names: the progress-bar helper is positional audio, and the scoreboard “dead row” count counts living nonspectators.

### PR #645 follow-up records (2026-09-11)

The active parity slice covers the deferred gameplay, wire, controls, sound, and rendering findings. Detailed new witnesses are in [item classes and squib](world/world-wac-ai-re.md#243b-squib-and-shared-class-effects-2026-09-11), [animation timing](anim/adm-bad-format-re.md#playback-clock-follow-up-2026-09-11), [item effect replication](net/novaworld-net-re.md#item-explosion-and-state-receive-order-2026-09-11), [normal maps](render/render-material-re.md#texture-preprocessing-follow-up-2026-09-11), and [projection/thermal waves](render/render-order-re.md#projection-and-thermal-wave-follow-up-2026-09-11). Vehicle, HUD, mission, controls, and audio findings remain in their domain records; the divergence ledger retains unrelated open work.

The [2026-09-13 jo-c sweep](jo-c-parity-audit-2026-09-13.md) reconciles the active
backlog, identifies additional source discrepancies, and orders the next fixes.
Its source/oracle confidence levels are separate from the earlier IDA grills and
runtime acceptance results linked above.

Scoped-aim parity (2026-09-13): the local body pass applies the stance-scaled, fixed-point scope/binocular oscillator and carries its phase across mission-kernel replacement; [world §14.9](world/world-wac-ai-re.md#149-scoped-weapon-stability-and-persistent-aim-drift-2026-09-13), D-WPN-36.

Empty terrain sectors (2026-09-13): ordinary views traverse quadrant 1 at zero height, retain source tracked bounds, collapse primary/blend coordinates, and share the original LOD-0 cache identity. See [terrain fallback record](terrain/terrain-re.md#empty-sector-flat-fallback-2026-09-13-d-terrain-12), D-TERRAIN-12.

WAC/BMS timing (2026-09-13): both schedulers use the mutable WAC time word and one admission decision per logic tick. A terminator-only program still executes at startup. See [mission event runtime](mission/bms-event-runtime-re.md), D-WAC-7.

2026-09-13, D-WPN-37: Airborne players suppress authored first-person ADS position bias while scope interpolation continues; reload, NoCardSwitch and ForceScoped keep their existing optical policy. See [world §14.10](world/world-wac-ai-re.md). Native `local_player_view` regressions pass.

2026-09-13, D-WAC-8: The Player/Item/auto slot retains all 32 bits; cache and group refreshes replace only its low-word entity handle. [World §33.15](world/world-wac-ai-re.md) records the original word stores and passing `wac_state` regressions.

PR #649 adversarial correction (2026-09-13, D-WPN-36): local heading/leg/pitch clamps precede scoped drift. The body-tick limit and PRNG regressions are recorded in [world §14.9](world/world-wac-ai-re.md).

2026-09-13, D-WAC-9: Numbered V operands accept decimal prefixes while declared names retain precedence and indices clamp at 255. See [D-WAC-9 evidence](world/world-wac-ai-re.md). Native `wac_behavior` prefix, boundary and shadowing regressions pass.

2026-09-13, D-EVT-9: BMS action counts use the original signed byte at execution; counts 128–255 skip actions while preserving event bookkeeping and linked spawns. See [D-EVT-9 evidence](mission/bms-event-runtime-re.md). Native `event_runtime_bms` immediate/delayed, truncated-slice and unsigned-trigger regressions pass.

2026-09-13, D-FOLIAGE-13: Foliage collection survives saturation of the 224-entry main terrain list while retaining its own visibility, distance and 128-cell limits. See [D-FOLIAGE-13 evidence](foliage/foliage-re.md). Native `terrain_frame_compiler` and `terrain_foliage_detail_collector` regressions pass, alongside eight original-executable capacity combinations.

2026-09-13, D-AI-13: Vehicle avoidance uses the original quantized footprint cosine and truncating bearing, preserving ordered neighbor braking and carrier exclusions. See [D-AI-13 evidence](world/world-wac-ai-re.md). Native `vehicle_mount` regressions pass for ground and boat callers, half-bin boundaries and compounded braking.

2026-09-13, D-WAC-10: WAC M# operands bind the live music context at compilation and share its actual byte globals; missing contexts alias scratch. Game music opens before initial WAC compilation. See [D-WAC-10 evidence](world/world-wac-ai-re.md) and [music globals](audio/mus-sbf-re.md). Native MUS/WAC and mission-kernel tests pass; Godot music-director integration passes 8/8, including startup, reload and weak-provider lifetime.

PR #649 integration review (2026-09-13, D-WAC-7): standalone event fixtures explicitly admit script ticks; a new empty-startup gate regression and all 97 Simulation GUT tests pass with reference assets. See the [mission runtime record](mission/bms-event-runtime-re.md).
