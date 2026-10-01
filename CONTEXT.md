# OpenNova

Glossary of the project's domain language. Definitions only: what a term *is*, not how it's implemented. Pick one canonical word per concept; alternatives go under _Avoid_. The runtime architecture map lives in [docs/runtime-architecture.md](docs/runtime-architecture.md); the documentation index is [docs/README.md](docs/README.md).

## Layers

The vocabulary for the codebase's own layering (ADR 0028/0034; the boundary rule is
ADR 0042's). Dependencies point one way: the Godot layer (godot/src bindings +
godot/game game runtime) → engine. Shells are the front-end composition inside the
Godot layer's game runtime, not a third dependency tier.

**Engine**:
The portable, Godot-free C++ core under `engine/` (`base/`, `formats/`, `runtime/`,
`net/`) — the reimplemented NovaLogic engine: format codecs, the world simulation, the
net stack. "The original engine" always means NovaLogic's binary; our engine contrasted
with retail is *the reimpl*. Godot is never "the engine".
_Avoid_: libs (the pre-2026-08 path), core, framework

**The Godot layer (first-class, ADR 0034)**:
`godot/src/` (pure C++ GDExtension bindings) plus `godot/game/` (the game-level
GDScript runtime) — the layer that wires Godot nodes to engine facts. The game
shell is composed inside its game runtime.
_Avoid_: godot/engine (the pre-2026-08 path), engine layer (that word is the
engine's), glue, bindings (only half of it)

**Shell**:
An application front-end composed inside the Godot layer's game runtime: the game
shell (in `godot/game/`). Shells own UI and application
flow, never engine behavior.
_Avoid_: frontend, app (in project prose), host (reserved for the game host)

**Simulation**:
The deterministic in-match world state advanced at the 62 Hz tick by the engine's world
systems (WAC VM, BMS events, AI). `Simulation` is the Godot-layer binding that owns it
shell-side; present passes project it onto scene nodes and never mutate it.
_Avoid_: game logic, GameWorld (that is the scene, below)

**Engine fact / Device fact**:
ADR 0042's placement test — a fact a headless ctest can reproduce without Godot is an
engine fact reached through one engine function; anything that exists only because a
Godot node/server/viewport/window/socket/clock/input event/audio player exists is a
device fact, written as typed Godot code beside its owner.

## Resources and assets

**Resource source**:
A mounted collection of files, with one lookup policy deciding which loose or
archived file supplies a name.

**Asset**:
Reusable model or animation content shared by the entities that use it.
An entity's position, animation playhead and damage belong to that entity,
not to its asset.
_Avoid_: simulation asset, render asset (when both consume the same content)

## Menu UI (MNU)

The vocabulary for NovaLogic's `.mnu` menu system and OpenNova's runtime support for it.

**Menu**:
A single `.mnu` document: one screen or a set of related screens authored together (e.g. main, options, loadout).
_Avoid_: dialog, page, form

**Screen**:
A top-level, full-canvas layout inside a menu. Only one screen is visible at a time; navigation moves between them.
_Avoid_: page, view (when you mean the whole canvas)

**Window**:
Any node in a screen's widget tree, container or leaf (the format element is `<WINDOW>`). A Window is either a grouping container or an interactive widget.
_Avoid_: panel, control (when you mean the tree node)

**Widget**:
A Window of a specific interactive/visual type (button, combobox, table, spinlist...). Use Widget for the typed sense, Window for the raw tree node.
_Avoid_: control, element

**Action**:
A behavior explicitly authored in a `.mnu` file via an `<ACTION>` element. The
retail vocabulary includes screen/menu navigation; showing, hiding, enabling, or
disabling a named Window; pop; URL and form submission; focus/tab operations; and
legacy browser/LAN/application-message operations. Some Actions are completed by
the Menu Shell, but they remain Actions because the Menu authored them.
_Avoid_: command, event, handler

**Command**:
Behavior supplied by the Menu Shell by matching a widget's **name**, rather than
written as an `<ACTION>` (start a mission, apply video settings, quit, commit a
loadout). An Action may also delegate work to the shell; the distinction is whether
the behavior is authored in the Menu or bound externally by name.
_Avoid_: action (reserve that strictly for the `<ACTION>` element)

**Menu Shell**:
The runtime front-end that loads a menu set, drives a live interactive menu, plays its audio, and supplies Commands by control name. The menu counterpart to the world runtime.
_Avoid_: menu host (retired 2026-07), menu manager, controller

**Tab**:
A Window shown or hidden by a sibling button's `window` Action (e.g. the Options panels). Not a widget type, just an authored convention: one button per panel, each `<ACTION type="window">` hiding the siblings and showing its own.
_Avoid_: page, panel (when you mean the toggling mechanism)

## World & NovaWorld

The vocabulary separating the in-game world from the online service. The names collided
historically; they are now distinct.

**GameWorld**:
The runtime world-sim scene (`godot/game/world/game_world.tscn`): terrain,
environment, mission runtime, and audio under one embeddable root. The standalone
game is the sole live mission runtime (ADR 0025). Formerly named `NovaWorld`.
_Avoid_: NovaWorld (that name now belongs to the service), world scene

**NovaWorld**:
NovaLogic's online matchmaking and account service, and our reimplementation of it (`apps/novaworld_server`, `engine/net/novaworld`). Always the service, never the in-game world. It is our NovaWorld server, not an emulator.
_Avoid_: emulator, lobby server

**Gate**:
The first-contact UDP service (port 7597; the `novaworld_gate` probe leg: `engine/net/novaworld/gate` for the protocol, `GateListener` in `apps/novaworld_server/gate_listener.cpp` for the listener) that bootstraps a client with the HTTP service URL and the NovaWorld UDP endpoint.
_Avoid_: lobby

**Browser**:
The in-game server list (`NovaWorldPanel` in `godot/game/novaworld_panel.gd` over the `NovaWorldClient` pump and `engine/net/novaworld/gate/gsb.cpp`) populated from the service's GSB/GLB data.
_Avoid_: lobby, server list (in code)

**Wire-compatible / wire protocol**:
Code that produces and consumes the exact byte stream the original game uses, so original
and OpenNova endpoints interoperate: our clients can join original servers, our servers can
serve original clients, and opennova↔opennova works the same way. The protocol witness
record is `docs/net/novaworld-net-re.md`.
_Avoid_: "our own protocol", custom packet format

**Host / Joiner**:
The authoritative side of an in-match session (the **host**) versus a remote peer that
came in through the join handshake (a **joiner**). Under the listen server the host runs
a local client too; "client" survives in wire-protocol prose (retail message names).
This is the ONLY meaning of "host" in this codebase. UI attachment points are
containers, presentation owners are **Presenters**, application front-ends are
**Shells**, the application embedding a portable lib is its **embedder**, and our
engine contrasted with retail is **the reimpl** — never "the host". A review
concern, not a lint (ADR 0043 retired `host_lint.py`).
_Avoid_: master/slave, owner (when you mean the host); host for anything that is not
the authoritative session side

**Listen server**:
A host that is simultaneously the authoritative server and a local client. OpenNova's
single-player runs this way — the sim serializes real entity state through the wire codec
and the present pass renders the locally-decoded result, so SP, co-op, and multiplayer
share one replication path (only the transport differs). See ADR 0011.
_Avoid_: standalone server, dedicated server (those have no local player)

**Probe**:
A registered runtime probe: a `GameProbe` script under `godot/probes/<family>/` run
inside the live game through the `game_probe` MCP tool, reading typed arguments and
returning a verdict with data and artifacts (ADR 0041, `docs/mcp.md`). An assertion
over the portable engine is a ctest, not a probe.
_Avoid_: manual probe script, env-configured probe, `*_probe.gd` under `godot/tests/`

**Runtime MCP**:
The `opennova-game` Model Context Protocol server the game runtime embeds
(`--mcp-port`), through which scripts, runbooks and agents read state, drive the
debug catalog and menu, capture frames and run probes. Retail is driven separately
through `onhook-mcp` (`opennova-int`).
_Avoid_: editor MCP

**In-match / Matchmaking**:
The two network protocol domains. **In-match** is the 62 Hz game session between a host
and its clients (the wire codec + replication runtime). **Matchmaking** is everything that
gets players INTO a match: gate, browser, accounts, NAT rendezvous — the NovaWorld
service's domain. Code and libs are named by their domain, never bare "net".
_Avoid_: unqualified "net code", lobby (for either)

**Wire codec / In-match session / Match**:
The two stable in-match boundaries and the gameplay model (ADR 0043 d3/d4, superseding ADR 0036). The **wire
codec** (`engine/net/npwire`, ADR 0019) is the retail compatibility contract and
encodes/decodes the byte stream; its message catalog is the single source of
truth (ADR 0013). The **in-match session**
(`opennova::inmatch::Session`, `engine/runtime/inmatch`) owns lifecycle, role,
fixed cadence, retained input, and tick outcomes. The authoritative **Match**
(`world::Match`) owns rules, player/team statistics, clock, winner evaluation,
and the frozen end-round result. `runtime/inmatch` (ex `npruntime`) and
`runtime/replication` (ex `netsim`; `tests/netsim` keeps the old name) are
implementation directories beneath those boundaries, not peer layers or extension seams.
_Avoid_: "the netcode" (name the wire transaction, session behavior, or match rule);
"net seam" / "net runtime layer" as public architecture

**Retail tick bank**:
The one fixed-tick accumulator (`world::TickAccumulator`,
`engine/runtime/world/tick_accumulator.h`), a port of `Game_MainLoop`'s frame-time
bank: each frame banks its elapsed time in 1/16 ms units, a 7/8 EMA smooths the bank
(one over 500 ms is clamped and skips the smoothing), and the bank drains in 4 ms
quanta with a logic tick on every fourth, so a hitch is repaid over the next frames
rather than in one burst. The in-match session banks through it and re-bases the
clock at mission start (the load and the first three frames' render time are never
banked); the same bank feeds the FR counter (`average_fps`).
_Avoid_: frame pacing, catch-up loop, a second tick clock

**Packet / Draw list**:
A **packet** is wire data — bytes on the network, and nothing else. What a frame
compiler hands a renderer/applier is a **draw list** (`*DrawList` types, "draw-list
order/bounds/diagnostics" in prose). The rearchitecture campaign renamed every render
output to this shape; new render-side "packet" vocabulary is a regression.
_Avoid_: packet for any render/compile output; "draw packet"

**Entity**:
An addressable object in a running world. A Person and a Vehicle are both Entities, but neither term implies who controls it.
_Avoid_: actor, object (when identity on the wire or in the world is meant)

**Person**:
A physical humanoid body. A Person may be controlled by the local human, a remote human, or AI; "Person" never means "NPC" by itself.
_Avoid_: infantry, player entity, soldier (when the physical body is meant)

**Player**:
A human participant in a match. A Player controls a Person and may control a separate Vehicle through a seat relationship.
_Avoid_: infantry, avatar, client (when the human participant is meant)

**Spectator**:
The third session role (#601, D-NET-217): a human admitted into a live match who
controls no Person. Signed capacity and an optional password live on
`GameConfig` (`spectator_slots` 0 disabled / −1 shared / positive dedicated);
the join decides Player vs Spectator before ClientAuth; the authority still
allocates a roster slot and a hidden, damage-disabled team-0 body while S2C
0x75 drives the client's free-fly camera, the 0x16 row rides the spectator
trailer, and the canonical bit is `replication::Connection::spectator`
(`slot+100567`). The retail deploy-hold bit covers spectators in the priority
build; the record is `docs/net/novaworld-net-re.md` §5.0e.
_Avoid_: observer, ghost, "dead player" (a spectator never deployed);
squad-mode vocabulary for the 0x2000/0x4000 BuildFlags bits (D-NET-217 refuted
the `g_squad_*` reading)

**Vehicle**:
A carrier Entity with its own physical state. Its driver, controller, gunner, or passenger remains a separate Person.
_Avoid_: player vehicle, mounted player (when the carrier Entity is meant)

**Soldier Class**:
The Player's selected loadout role, such as medic, sniper, gunner, rifleman, or engineer.
_Avoid_: player class, replication class, character identity

**Character id**:
The packed `Avatars.def` nat|div|combo|side word the wire and world/FP composition key on (`npwire/character_id.h`).
_Avoid_: avatar id, skin id, character index

**Placed device**:
A thrown or emplaced explosive converted to its own pool-1 Entity (satchel, claymore, AV mine), replicated via S2C `0x59`/`0x12`.
_Avoid_: deployable, planted explosive

**Controller Kind**:
Whether an Entity's decisions come from the local human, a remote human, AI, or no controller.
_Avoid_: local-player flag, remote flag, infantry type

**Infantry**:
The historical reverse-engineering label for the AI/organic Person compact on the in-match wire. It is a codec name, not a physical kind or controller kind; use Person in domain prose.
_Avoid_: infantry (for every on-foot person), infantry player

**Wire Compact Codec**:
The fixed record body selected while walking an S2C `0x0A` update: Player-Person, Organic-Person, or Vehicle. It says how to decode bytes, not what controls the Entity or how it moves.
_Avoid_: entity class, player class, object type

**Motion Family**:
The physical mover used by an Entity: Person, ground/light/water/air Vehicle, guided, or static. It is independent of wire codec and controller kind.
_Avoid_: net class, item type

**Storage Pool**:
The runtime allocation family encoded in a wire handle's high nibble. It is independent of BMS kind, item type, controller, and compact codec; when retail allocation is unwitnessed, say unresolved.
_Avoid_: infer the pool from the entity kind

**Presenter**:
A runtime node that owns one presentation surface and projects sim or menu state onto
it: `LocalPlayerPresenter` (FP camera/input/viewmodel — every peer runs one for its own
player, joiners included), `GameHudPresenter`, `ArmoryPresenter`,
`DeployScreenPresenter`.
_Avoid_: host, view controller

## 3DI collision authoring

The canonical author-facing names for gameplay collision-volume families in a `.3di` model.

**Generic Collision Box (CB)**:
A general-purpose collision volume.
_Avoid_: generic BVOL, ordinary box

**Ladder Collision (CL)**:
A collision volume authored for a ladder.
_Avoid_: platform volume, seat volume

**Armory Collision Box (CA)**:
A collision volume authored for an armory.
_Avoid_: armory trigger (when naming the authored volume)

**Vehicle Collision (VC)**:
A collision volume authored for vehicle collision.
_Avoid_: damage pass, damage volume

**Blink Box (BB)**:
A convex interior-containment volume authored for a room or enclosed space.
_Avoid_: blink volume, visibility box

**Door (CD)**:
A collision volume authored to activate a door or another door-like moving part.
_Avoid_: destructible-section volume

**Change Team Box (CT)**:
A collision volume whose player activation changes an object's team settings.
_Avoid_: capture-zone volume

**Flag (CF)**:
The project name for the grounded activation volume. The bundled authoring manual
describes its purpose as activating special functions such as FARPs.
_Avoid_: flag collision box

**Player Collision (CP)**:
A collision volume that physically affects players but not AI.
_Avoid_: generic player-only solid

**Damage High / Damage Medium / Damage Low (DH / DM / DL)**:
The three authored contact-damage volume grades, from high through low.
_Avoid_: numbered hurt volume, damage tier 16/17/18

## Model and animation tools (ADR 0047)

**`.o3d` scene text**:
The line-based text form of one `.3di` model (`docs/threedi/o3d-scene-format.md`).
`opennova-3di build` mints a `.3di` from it through the engine's construction API and
parity writer; `opennova-3di scene` writes any `.3di`, retail ones included, back out
as it (build's exact inverse).
_Avoid_: scene file (a Godot `.tscn` is a scene), ASE/OED (the retired authoring
formats)

**`.o3a` clip-set text**:
The text form of one rig's clip set: its `.adm` table and every `.bad` clip the table
names (`docs/anim/o3a-scene-format.md`). `opennova-3di anim build` mints the set from
it; `opennova-3di anim scene` writes a set, retail ones included, back out as it.
_Avoid_: animation file, clip file (a `.bad` is one clip)

**`opennova-3di`**:
The command-line tool (`apps/threedi_cli`) over the engine's one `.3di` reader and
writer: `build` / `scene` over the `.o3d` text, `info`, `compare` and `catalog`,
`anim build|scene|info|compare` for a rig's `.bad` clips and `.adm` table over the
`.o3a` text, and `weapon timing` / `weapon merge` (the timing request measured with the
engine's weapon FSM, and its edits file applied to a `weapon.def` copy;
`docs/anim/weapon-timing-format.md`). The Blender add-on bundles it.
_Avoid_: the exporter (the add-on is the front end; the CLI mints the files)

**Blender add-on**:
`tools/blender/opennova_3di`: exports and imports `.3di` models and their animations
by the ASE/OED object-naming convention (`docs/threedi/scene-naming-contract.md`),
through the bundled `opennova-3di` and the two texts. Import stashes no source data.
_Avoid_: plugin, DCC pipeline (the Python/DCC authoring layer ADR 0038 retired)

## Products & modes

**Godot product**:
The OpenNova game runtime built with Godot (ADR 0045, ADR 0048). Backend
services and development tools are outside this taxonomy (ADR 0015).
_Avoid_: product (when the Godot boundary matters), app (ambiguous), the runtime
(as a product name)

**Bundled assets**:
The `assets/` directory shipped beside `opennova.exe`: OpenNova's own game data,
authored from scratch. Today it is the placeholder main menu that says the
OpenNova game is coming and offers **PLAY RETAIL**, plus the first pieces of the
game's own data (the `on_ar15` carbine and arms the Blender add-on exports from
`art/`, not yet referenced). The game mounts it when no `--resource-dir` is given
(ADR 0048); the web build stages it, minus that unreferenced art, into the page's
filesystem before boot (ADR 0049).
_Avoid_: fixtures (test-only data), retail data

**Serve mode**:
`opennova.exe` hosting a match without being a player: the host screen's retail Serve Only
server type (`SERVERTYPE` = 1); retail has no command-line auto-host to port. A mode of the game product, never a separate binary,
riding the one in-match seam (ADR 0015).
_Avoid_: dedicated server product, server exe, opennova-server

**Title**:
A NovaLogic game identity (JO, DFX2, BHD...) — near-identical engine skins over different
data and configuration. OpenNova currently ships title-agnostic; per-title products are a
recorded future option, so new code must not hardcode title identity where a named
constant or config read is equally easy (ADR 0015).
_Avoid_: game (when you mean the identity, not the running program)

**Required resources**:
The resource set the engine hard-requires by name at boot (menu set, game strings, music
banks, HUD layout, defs, default world files...). The witnessed enumeration is
`docs/required-resources.md` (landed at ENG-6); the engine manifest derived from it drives
boot validation and defines the minimum game-data tree.
_Avoid_: core assets, base game files

**Promote**:
Reserved for `mission::promote_mission` — spawning a parsed mission into the live world
(entities, AI brains, nav), the IDA-cited spawn path the game uses. Other historical uses of the word (old-title format upliftment, 3DI→IR
normalization, fixture curation, code relocation) should be phrased as *migrate*,
*normalize*, *whitelist*, and *move* respectively.
_Avoid_: promote (for anything but the mission→world spawn)

## Runtime presentation

**HUD**:
The in-game heads-up display, laid out by `hudpos.def`. RE record:
`docs/interface/hud-re.md`.
_Avoid_: overlay, UI (too broad)

**Dev tools**:
The engine-owned Dear ImGui tool windows behind Insert (`engine/runtime/devtools`,
ADR 0039): the Stats window over the frame-stats board, and every inspection
or control window added later. Debug builds only; the Godot side is one
`DevTools` node plus the imgui-godot addon.
_Avoid_: debug overlay, F3 overlay (the retired GDScript surface), editor

**F3 control board**:
The read side of the one debug-control table F3 and MCP share
(`engine/runtime/devtools/control_board.h`, ADR 0043 d12): each row's definition
(label, tooltip, kind, range, enum choices) pushed once, and the live state (value,
writable, the refusal reason) of the rows the visible windows declare, pushed on a
short cadence. A window draws a row through `draw_control` and writes it through a
`ControlRequest`, so F3 shows exactly what MCP's `game_debug op=list` reports.
_Avoid_: a window-local copy of a row's range, choices or refusal text

**FrameFX**:
Retail's post-scene frame-effect system (the IDB's `FrameFX_*` family): after the
scene and before the HUD, the distortion pass (type 0), the death or damage blur
(4 / 1), the bloom (2), then the thermal (8) and monitor (9) views; the first-person
NVG view replaces the whole chain. The portable planner is
`engine/runtime/renderer/frame_fx_effects`, the device `godot/src/render/frame_fx`.
Record: `docs/render/render-order-re.md`.
_Avoid_: post-processing, screen shaders (as names for this system)

**Scene overlay stage**:
The post-particle tail of the retail scene frame (`engine/runtime/renderer/scene_overlay.h`):
after particle pass B and before FrameFX, in the witnessed order, the NVG laser beams,
precipitation, the light coronas, the water glint, the underwater murk and the sun
glare. The engine emits it as a typed draw list the device runs after its own pass-B
composite.
_Avoid_: HUD overlay (the HUD draws later), post pass

**Spinmap**:
The heading-up gameplay map element in the authored `HUDSPINMAP*` rect (retail
`HUD_DrawMapOverlay`); our code says minimap (`HudMinimapCompiler`, `minimap_overlay`,
`client_replica_minimap`) — one thing, two words.
_Avoid_: radar, compass (the compring overlay), map overlay (the windowed command map, D-HUD-19)

**HUD declutter**:
The `hud_detail` 0..3 level × hudpos `HUDDECLUT_*` masks visibility system (`huddetail`
cycles it; level 3 blanks the HUD).
_Avoid_: showhud (that cycles the FP-weapon view flags, not the detail level)

**Present pass**:
The per-frame apply step that projects simulation state onto scene nodes
(the native `EntityPresenter`). It runs once in the standalone game runtime (ADRs
0006 and 0025).
_Avoid_: render pass, sync pass

**Weather home**:
`world::WeatherState` on the simulation tick — the ONE home for weather
authority state (#597): `MissionKernel::tick_weather` runs the sim legs after
every logic tick and calls the `IWeatherRenderTick` render owner; the tick
model is `env::WeatherRuntime`, the file/core math `engine/formats/env`, and
`env::weather_seed_from_config` the one seed derivation. Standalone owners
(previews, GUT fixtures) tick `EnvironmentState::standalone_weather()` through
the same runtime. Record: `docs/env/env-tod-re.md`.
_Avoid_: a second weather clock or state copy anywhere (the pre-#597
`EnvNetworkState` / render-side accumulator shapes); "environment system" for
the weather authority (the `.env` document model is `formats/env`)
