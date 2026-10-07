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

**File source**:
Whole files by flat logical name with a stamp that moves whenever a read may answer
differently (`engine/base/vfs/file_source.h`): the game's mounted root, or the editor's
project files with its open documents standing in. A menu's inputs are loaded through
one and kept by name and stamp.
_Avoid_: mount (a resource source's layering), cache key (the stamp is the source's)

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

**Root window**:
A Window directly under a screen. A screen holds any number of them, in document order:
retail draws them in that order, hit-tests the last first, and takes the screen's text
table and default cursor from them (a SCREEN itself reads only its NAME, MUSICVAR and
windows).
_Avoid_: screen window, main window (`MAIN` is only the usual name)

**Part**:
A Window an owner parses from one of its own elements with an embedded widget: a
combo's LIST_BOX, a spin list's SPINUP and SPINDOWN, a list's, table's or multi-line
edit's SCROLLBAR. An owner holds one of each at most; the owner's field of the same name
leaves it out of the file or writes it again, its content kept. Retail attaches a part to
its owner as a child but names it itself when it creates it (`LISTBOX_WND`,
`SPINLISTWND_UP` / `_DOWN`, `LISTWND_SCROLL`, `TABLEWND_SCROLL`, `MEDITWND_SCROLL`), so a
lookup by the part's authored NAME never reaches it; the windows a part holds keep theirs.
_Avoid_: sub-window, child (a part is not in the children list)

**Element path**:
The name a menu field or list goes by in the format's property table
(`formats/mnu/mnu_schema`), the editor, the MCP and the findings: the format's element
and attribute names from the record down, lower-cased and dotted (`position.left`,
`string.value`, `font.default_fg`, `items.item`, `column.header`).
_Avoid_: field alias (the flat `image_default`, `action_file`, `text` names are gone)

**Parse note**:
What the menu reader leaves out because retail's reader does not read it (an attribute
retail ignores, a window retail never creates), with its line and path. The editor
lists it as a warning; a save writes the menu without it, which retail reads the same.
A fatal parse note names input retail crashes or hangs on (an empty value it
tokenizes, the text ending inside a tag): the editor blocks that menu and the build
until the file is corrected.
_Avoid_: parse error (the file still loads)

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
the behavior is authored in the Menu or bound externally by name. A widget's Actions
run before its Commands, as retail runs its callbacks after the ACTION rows.
_Avoid_: action (reserve that strictly for the `<ACTION>` element)

**Hotkey**:
A key a screen's table binds to a Window: an authored `<HOTKEY>` (`VK_RETURN`,
`VK_ESCAPE`, `VK_SPACE`, or a character) or a label's `{hot}` letter. Pressing it
clicks the first Window of the table bound to it that is visible and enabled, unless
some Window has the keyboard focus.
_Avoid_: shortcut, accelerator (except when quoting retail names)

**Popup**:
The one shown MODAL Window that has the input: while it is open only it and its
descendants take the mouse and the hotkeys. A combo's open list is a dropdown, not a
popup.
_Avoid_: dialog (for the input sense), modal (the attribute)

**Menu Shell**:
The runtime front-end that loads a menu set, drives a live interactive menu, plays its audio, and supplies Commands by control name. The menu counterpart to the world runtime.
_Avoid_: menu host (retired 2026-07), menu manager, controller

**Tab**:
A Window shown or hidden by a sibling button's `window` Action (e.g. the Options panels). Not a widget type, just an authored convention: one button per panel, each `<ACTION type="window">` hiding the siblings and showing its own.
_Avoid_: page, panel (when you mean the toggling mechanism)

**Stylesheet / Style variable**:
A `.mns` file of `NAME value` lines whose names the menus use as `%NAME%` (a font, a colour, an image). The game reads two, the **shell's stylesheets**: `menu_style.mns`, then `brand.mns` onto the same list, a later definition winning; a `.mns` by any other name is never read. A **style variable** is one name in that list; the one the game reads is the last definition of the last sheet that has it.
_Avoid_: style file, theme, macro (except when quoting the shipped header)

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
_Avoid_: editor MCP (the editor's own server, below)

**Editor MCP**:
The `opennova-editor` Model Context Protocol server the OpenNova Editor embeds
(`--mcp-port`, 8977 in `.mcp.json`; `scripts/mcp/editor_mcp.py`), through which an
agent drives the editor's session by its two seams: the state by section, a request by
kind (`editor_request`), a query by name (`editor_query`), Build and Play, and a document's
viewport (`editor_viewport`: the `viewport` query's reads, and `set_viewport` and
`edit_in_viewport` for its writes) (ADR 0046 S6d, S13 A5, S13 V7, `docs/mcp.md`). Its tools and
their schemas are made from the `catalog` query when it starts. The game a Play starts is then
driven through its own runtime MCP.
_Avoid_: runtime MCP (the game's), game MCP (when the editor is meant)

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
The line-based text form of one `.3di` model (`docs/threedi/o3d-scene-format.md`),
carrying no native limit: meshes of any size, influences of any number, names of any
length (ADR 0052). `opennova-3di build` mints a `.3di` from it through the engine's
reader, its lowering to the retail target, the construction API and parity writer;
`opennova-3di scene` writes any `.3di`, retail ones included, back out as it (build's
exact inverse).
_Avoid_: scene file (a Godot `.tscn` is a scene), ASE/OED (the retired authoring
formats)

**Lowering** (of a `.o3d` model to a **target**):
The step that holds a scene text model to what a target game holds
(`engine/formats/threedi/threedi_o3d_lower.h`): it splits a mesh into the strips a
3DI3 stores, keeps the influences the game blends, and checks every name, count and
fixed-point extent against the target's limits (`ThreediTargetLimits`; the retail
target, `threedi_retail_limits`, is the only one), refusing what the target cannot
hold and noting what it loads all the same.
_Avoid_: validation (the reader validates the text; the lowering is the target's),
export (the add-on exports; the CLI lowers)

**`.o3a` clip-set text**:
The text form of one rig's clip set: its `.adm` table and every `.bad` clip the table
names (`docs/anim/o3a-scene-format.md`). `opennova-3di anim build` mints the set from
it; `opennova-3di anim scene` writes a set, retail ones included, back out as it.
_Avoid_: animation file, clip file (a `.bad` is one clip)

**`opennova-3di`**:
The command-line tool (`apps/3di`) over the engine's one `.3di` reader and
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
One of the two applications exported from the `godot/` project: the OpenNova game
runtime (`opennova.exe`, ADR 0045 and ADR 0048; its Play export adds the runtime MCP
for the editor) and the OpenNova Editor (`opennova-editor.exe`, ADR 0046). Backend
services and development tools are outside this taxonomy (ADR 0015); so is
`opennova-serve` (ADR 0051).
_Avoid_: product (when the Godot boundary matters), app (ambiguous), the runtime
(as a product name)

**OpenNova Editor**:
The project-based data editor, the second Godot product (ADR 0046): it owns a Project,
enforces the engine's Required resources as a checklist, edits assets through the
engine's own format libraries, and packs a Build for Play or Export. Its portable core
is `engine/editor/` (`opennova_editor`), its bindings `godot/src/authoring/`, its
panels Dear ImGui windows (ADR 0039). "The editor" in prose means this product; the
Godot editor is always "the Godot editor".
_Avoid_: ONED (the retired product), mod tools, modtools, terrain editor

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
Hosting a match without being a player: retail's Serve Only server type (`SERVERTYPE`
= 1, saved as `game.cfg` `dedicated`), a host with no player of its own
(`RoleKind::DedicatedHost`, HostOnly). The game runs it from the host screen;
`opennova-serve` runs it with no shell (ADR 0051). One implementation below
the config, riding the one in-match seam (ADR 0043).
_Avoid_: dedicated server product, server exe, opennova-server (the binary is
`opennova-serve`)

**`opennova-serve`**:
The headless game server (`apps/serve`, ADR 0051): the game's Serve Only host with no
Godot, configured by a host file. A product, not a Godot product.
_Avoid_: dedicated server (for the binary), server exe, opennova-server

**Host file**:
Retail's dedicated-server config, the file `/HOST <file>` names: `<Key> <value>` lines of
`ServerConfig_ApplyHostSetting`'s keys (a text form of the host screen) and
`Mission <file> <launch option>` lines that seed the map rotation. Retail never reaches
its reader; `opennova-serve` reads it (`engine/runtime/inmatch/host_file.h`).
_Avoid_: server config, server.cfg, dedicated.cfg

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

## Authoring (the OpenNova Editor)

The vocabulary of ADR 0046's project model.

**Project**:
A directory the editor owns: `project.opennova` (versioned JSON: title, target game,
feature toggles, export settings), the loose source files anywhere beneath it, and a
disposable `.opennova/` cache. The project IS the source tree; nothing lives in a
database.
_Avoid_: workspace (the editor's windows, not the data), mod (an expansion-type project is a
project kind), game directory (the runtime's mounted root)

**Game install**:
The folder of an installed NovaLogic game (Joint Operations) that a project imports from and
plays in, mounted as a stock launch mounts it (the boot archives, no `/d`). Each project names
its own in `.opennova/local.json` (`game_install`); the editor keeps the one last chosen, where
a project naming none starts. Its files are copied into a project on request (Import from the
game data: the files chosen, their **import closure**, or the whole install in one action), never
redistributed: its archives' files, and the loose files the game ships beside them and reads from
there (its music banks, its videos, the country code, the NovaWorld table and its screens; never
a save, a configuration or a score). Play in the game install runs a build there.
_Avoid_: retail directory, retail root, retail files (the names before ADR 0046 S13 A4), resource
dir (the runtime's `--resource-dir`, which may be a build)

**Install check**:
What a folder holds as a game install, read as an import mounts one (the boot archives, no `/d`):
whether they mount, how many files they serve, the expansions, whether the game's program is beside
them; said in a line under every field that names an install (New project, Project settings, the
welcome page), so a wrong folder is said at once (`assets/install_check.h`, the `check_install`
request).
_Avoid_: install validation (nothing is validated: the folder is read)

**Welcome page**:
The editor with no project open: the Document window across the whole workspace (the other windows
stand aside, as the Preview does for a document it has nothing of), with the recent projects (each by
its title, its game and its expansion), Open, and the New project form with its game install.
_Avoid_: start screen, home, launcher (ADR 0048 retired the launcher)

**File card**:
Files' page for one project file: what a file of its kind is to the game, where a build puts it, where
it came from, what it names and who names it, a wave's sound with Play; what a double click opens for a
file the editor has no document of (`session/file_card.h`, the `file_card` query).
_Avoid_: properties, details, info panel, References (the card's two lists replaced it)

**Project expansion**:
What a project builds as and on (ADR 0046 S16, `project.opennova`'s `expansion`): its **expansion
name**, the folder `expansion\<name>\` its build is, played with `/exp <name>`, and the installed
expansion it **builds on**, whose files it imports as `/exp` mounts them (none: the base game). A
project with no expansion name is standalone; one that builds on an installed expansion builds as one
of its own, since the game reads an expansion's files only under `/exp`. The files the game reads by an
expansion's name (`<name>.bin`, `version.txt`, `M<name>.*`, `G<name>.*`, `<name>L.lwf`, `<name>.lwf`)
are its **expansion files**.
_Avoid_: mod (a project builds as an expansion; "mod" is the Mods list's word for one), base (for
the expansion a project builds on: the base game is the install with none)

**Logical name**:
The flat, case-insensitive name the engine resolves an asset by (`main.mnu`,
`items.def`), at most 16 bytes as a PFF entry. A project asset's identity; its path in
the tree is organization only. Uniqueness and length are checked on output names.
_Avoid_: path (when the engine-facing identity is meant), resource name

**Font set**:
A bitmap font made from a picture by the editor's font importer: `<name>.fntset`, a short text naming
its **glyph sheet**, a PNG of 16 x 14 equal cells holding the bytes 0x20..0xFF in reading order, each
glyph its cell's alpha. The sheet is the import's input; the import record's options are the font's
metrics (`advance`: `ink`, `left` or `cell`; `tracking`; `space`; `spacing`; `design_width`) and
`color` (`white`, a mask the text's colour tints, or `sheet`, the sheet's own colour kept). The
import makes `<name>.fnt` through the FNT writer, every glyph the cell's height and as wide as its
advance (the format has no advance table). An import of the set from the disk brings its sheet beside
it into `fonts/`.
_Avoid_: font project, atlas (the `.fnt`'s pages are the packer's layout, not the sheet's)

**Project layout / placement / move**:
Where a project keeps its files: **flat** (most at the top level, as the base game does) or **by
kind** (each in its kind's folder, `AssetKindRow::folder`). A file the editor makes (Create
missing, New file, an import) is **placed** beside the project's files of its kind, else as the
layout says (`assets/project_layout.h`, DI-03), so a flat project stays flat. A **move** puts a
file in another folder under its own name and rewrites no reference, since the game finds it by
its logical name.
_Avoid_: default folder (a kind's folder holds new files only in a project laid out by kind)

**Import / sidecar**:
Bringing a non-native source (an image, a terrain set; later a sound bank's manifest, a font)
into the project the Godot way: a committed `<file>.import` sidecar records the
importer, its version, options, output logical names, the source's content hash and the
**inputs**, every other file the importer read through its **import context**
(`ImportContext`), by path (taken from the source's folder, never outside the project), and
nothing a checkout or an edit of an input changes; the outputs are regenerated into
`.opennova/imported/`, in a directory named by a hash of the source's path, and packed like
native assets. One import is one source with as many inputs as its importer reads, and it runs
again when the source or any input changes. The **import cache**
(`.opennova/import_cache.json`, machine-local), keyed by project-relative path, keeps each
source's and each input's size, last-write time and content hash (a file written within two
seconds of the pass that read it is read again next time), and the record and the inputs'
hashes each source's outputs were made from, so only a real change imports again and an
untouched file is not read. A file is an **import source** (its own kind, whatever its name:
`import_source`) only while its record is there: importing it writes the record, and a `.png`
with none is a texture the build packs as it is. A file a record lists among its inputs is an
**import input** (`import_input`, whatever its name: a terrain set's heightmap is no texture) while the
record lists it. The build never packs an import source or an import input, only the outputs. The
files the import dialog offers are **import choices** (`ImportChoice`).
_Avoid_: convert (the runtime never converts), asset pipeline (the retired Python route),
image source (the kind's name before S13 A8)

**Terrain set**:
A terrain made from ordinary images (ADR 0046 S20): `<name>.tset`, a short text in `art/terrain/`
naming its heightmap (a 1024 x 1024 PNG at any depth, or TrnGen's own `.raw`), its colour map
(1024 x 1024) and, if wanted, a detail, a tile set and a surface map, each an import input; its import
record's options are the terrain's numbers in world units (the heightmap white's height, the water
level, the layout). The terrain importer bakes it with TrnGen.exe's own bake (the port in
`engine/editor/terrain`) into the files the game reads for a terrain, each named after the set:
`<name>.trn`, `.cpt`, `.til`, the colour, detail, blend and tile-set textures, and `<name>_m.pcx`, the
**surface map** (the `.trn`'s char map: each texel's index the surface class the game reads there,
painted as indices or in the char map legend's colours, `formats/trn/charmap_legend.h`), and
`<name>_f.pcx`, the **foliage map** (the `.trn`'s `polytrn_foliagemap`: each texel's index a foliage code,
which the set's `foliage` blocks, the `.trn`'s own, turn into the models that grow there; ADR 0046 DI-29).
Files' New > Terrain from images..., the `new_terrain` request and `opennova-project new-terrain` make one;
a change to an image imports it again.
_Avoid_: terrain project (TrnGen's `.tpj`), heightfield document

**Ground overlay**:
What the mission view tints its terrain with (ADR 0046 DI-29, Show > Over the terrain): a picture laid
over the terrain from above, each texel what the game's own sampler reads at its middle, the surface
class (in the char map legend's colours) or the foliage definitions the foliage map grows there, with
its legend over the picture's corner.
_Avoid_: overlay alone (one shape a canvas draws over the picture), debug mode (the terrain shader's
diagnostic colourings)

**Import closure**:
What an import "with the files these need" brings beside the files chosen (ADR 0046 S14): every
file they reference, and those files' in turn; the file that defines each name they use (an
item, a weapon, a particle effect, a string id, a screen, a style variable), where no file of
the project or the import does; and for a mission its **mission sidecars** and every file the
game opens by a fixed name at boot, at the menu, at mission start (the required-resources
manifest) and while a mission runs. It is planned before anything is written, a file a step (the **import plan**: each
file, where it comes from, what wanted it and its size, the files by kind, what is found
nowhere, the names no place defines), then written a file a step: every file checked and staged
before any is published, so a file that cannot be read or converted writes nothing (a failure while
publishing can leave the files published before it). The player's own files (a save, a
configuration, the scores, the stored credentials) are never taken. A
project is its own files: nothing is mounted under it, so a mission plays, previews and builds
from what its project holds, and a shipped JO mission's closure is some 7,300 files (650 MB:
most of the game, its sounds included). Importing the whole game install is the one action that
leaves nothing out.
_Avoid_: dependency mount, base, parent project (a project builds on nothing but what it holds),
bundle

**Import option**:
One setting of how an import makes its outputs, held in its import record by a key (ADR 0046 S18):
each a row of its importer's, with the values it takes, what it means where the record leaves it out
and the option it applies under (the image importer's format, file name, alpha, size, palette, DDS
compression, mip levels, green and normal map). What a texture's uses ask of its import (a model row's
`.tga` the DXT5 `.dds` its loader reads first, a colour map a 24-bit 1024 x 1024 TGA, a loading screen an
800 x 600 PCX, a foliage map from an 8-bit PCX its indices kept) is said beside them, with why; a
texture's tab sets them and its uses' in one click. A PNG an author imports is an import source; a TGA or
a PCX is one only where its record makes it one (Replace, Edit externally), else the texture the game
reads as it is.
_Avoid_: import setting, sidecar field, conversion (an option is how the outputs are made, never a
change to the source)

**Mission sidecar**:
A file the game finds by a mission's name rather than by a reference in it, each skipped when
absent: `<mission>.bin` (its text, else `medmssn.bin`), `.wac` (its script), `.pcx` (its loading
image, else `loadscrn.pcx`), `.til` (its tiles), `.dbf` (its dialog bank) and that bank's sounds
(`.lwf`, else `.pwf`, read only when the `.dbf` exists), each by the mission's name to its first
dot. One witnessed table (`mission::sidecars`), which the mission text's loader and the editor's
Play mission read (the other loaders come to on touch), and of which the editor's mission document
makes the mission's own edges (its file set), which the graph, Rename and the import follow.
_Avoid_: companion (the members of a mission set a rename takes with the mission), attachment,
import sidecar (a source's `.import` record)

**Mission set**:
A mission and its sidecars as the editor holds them (ADR 0046 S14): each sidecar a reference the
mission's file makes (its string table, script, loading image, tile placement, dialog bank and the
bank's sounds), those the game runs without optional (no finding when the project lacks one), so the
graph, References, the import and Problems see the set; a mission renamed takes the members the
project has with it, its **companions**.
_Avoid_: bundle, package (a set is the files a name finds, not a container)

**Mission text**:
A `.mis`: the original mission editor's interchange text (`dfx2med.exe`), which the game never reads.
Its own kind, packed nowhere; never a mission.
_Avoid_: mission (the `.bms` the game loads), mission file

**Display name**:
What a value a document holds reads as to a modder where it stands for something with a name (ADR
0046 S15): an item id by its catalog's name, an SSN by its entity's item and SSN ("Ranger #12"), a
zone, a path with its stops, an event by its sentence, a text key by its string; one naming nothing
says so in words (it is **dangling**). The raw value stays beside it, secondary (muted, in a tooltip,
on the wire). One service makes it for every window and the wire
(`engine/editor/graph/display_names.h`; a type's own words in its `record_label` and `value_label`
hooks, the mission's `documents/mission_labels.h`), reading the project's names through a **name
source** (the asset graph's). A record's display name is its **title**; its **name** stays what the
graph keys it by (an entity's SSN). Where a column cannot hold the title, its **brief** words show
what tells the record apart first (an event's first trigger's subject and verb, an entity's SSN).
_Avoid_: label (a field's own name), caption, alias

**Game's own data**:
What the game install ships, as the game is served it, what an import of the install copies (ADR 0046
S15, `engine/editor/session/original_files.h`). A finding is the game's own when the game install, as
it ships and validated as a whole, makes the same finding (its code, its record as itself, its field,
what it names) in the file of the same name: whatever the modder's edits or the project's other files
brought is the modder's (a texture the install has that the project lacks among it), however shipped the
file it is in. Problems shows them apart, under "In the game's own data (also in the original)", and
counts the modder's findings first; a finding that blocks the build is never the game's own there.
_Avoid_: stock files, vanilla, unmodified (as Problems words)

**Event sentence**:
A mission's event in words, as the game runs it (ADR 0046 S15): "When <trigger>, and/or <trigger>
..., then <action>; <action>.", the triggers folded left to right with the joins their flags make
(bracketed where the join changes), a negated one in its type's negative words, the start, end and
repeat flags and the two times in seconds, each parameter by its display name
(`engine/editor/documents/mission_sentence.h`). An event's title; a trigger's and an action's are
their part of it. Its type's name in the Add trigger and Add action pickers is its **title**, under
its **group**.
_Avoid_: description, summary, event label

**Base layer**:
What a read-only dependency mount (a game install a project builds on) gives the project's
asset graph: its files and the names they define, read once and never edited. A lookup by name
tries the project first, then the base layer, whose file of a name the project also has is
hidden with the names it defines (project assets win); the base layer makes no reference and no
finding of its own. No project mounts one (ADR 0046 S14: a project imports what it needs).
_Avoid_: second graph (one graph, the base under it), import (an import copies files in; the
base layer only answers names)

**Requirement**:
One row of the editor's checklist: a file the engine demands by name (a Required
resources manifest row with its witnessed severity and failure text, keyed by a stable
role token) or a project feature's own need, Present, Missing or Wrong kind. One the
project lacks is a Problems row whose fixes Create it, Import it from the game data or,
for a required one, Use another file of its kind as it (Assign, a rename). The engine's
names stay fixed; the checklist enforces them.
_Avoid_: dependency (that is a reference between assets), contract (the deferred
runtime-read deployment contract)

**Free-form file**:
A new project file of a kind that no requirement names (New > Menu... or String table...
in Files): it comes from that kind's one free-form blank factory, never from a
requirement's (a new menu is one screen named after the file, not a copy of STARTUP).
_Avoid_: template (the blanks are authored from scratch, not copied)

**Sound bank / music bank**:
A sound bank is a `.lwf`: the sound sets the game plays by name, each naming the waves it picks
from (the game's `.lwf` reader is its sound bank's). A music bank is a `.sbf`: the music a music
script streams by path, copied loose beside the archives.
_Avoid_: wave bank (a `.lwf` holds no waves, it names them), sound bank for a `.sbf`

**Wave**:
A `.wav` the game loads from its archives by name, as a sound bank's sets name it (retail packs
thousands, its localized voice lines in language.pff): a native file the build packs as it is.
_Avoid_: wave source (it is no import source), sample, sound (a sound is a bank's set)

**Face animation**:
A character's facial animation file (`.grm`): the texture meshes the face deforms, its gestures'
offsets and its eyes, which the game loads beside an item's model by the model's name.
_Avoid_: face (a model's bullet face is a record kind of that name), grm (the format's name), face
mesh (the meshes are one part of it)

**Score table**:
`score.ini`: the scoring values per game type, which retail reads from the install folder.
_Avoid_: configuration (its kind before it had its own), score config

**Map project**:
A `.npj` or `.npz`: the mission editor's project for a mission, which the game's mission list
scans for beside the `.bms` files and loads through an XML loader not yet read (D-MIS-7): packed as
it is, opened by no editor yet.
_Avoid_: mission (the `.bms` the game loads), map pack

**Material chunk**:
A file a model's chunk material row names, which the game reads as a chunk container (NQ8B,
HRZ8 or AOC8) whatever its name: the editor types one by its chunk headers when no rule types its
name (`material_chunk`), and the build packs it with the art. A file of no kind the game knows,
which the build leaves out, serves no row.
_Avoid_: texture (a texture row's file, read by its extension), chunk file

**Surface**:
What a round that hits a model's bullet face plays: its ammo's effects row of that name (Metal,
Glass, Wood; a face's byte b plays row b + 4 of the game's tag table). The file keeps one on each
bullet face; a modder sets it on a material, every face made from that material's triangles taking
it in one step (ADR 0046 S17), and a material whose faces disagree is mixed. The Blender add-on keeps
it on the material too, a face that differs keeping its own on its polygon.
_Avoid_: impact material, poly type (the format's words), collision surface

**Volume**:
A model's collision solid (BVOL): the convex solid of its planes within its stored box, what the game
tests contact, ladders, zones and lines of sight against, its type saying how (solid, a ladder, a blink
box, a zone, damage). Rounds never meet volumes: they hit bullet faces (or a person's hit spheres). The
model viewport draws them by their type's colour (ADR 0046 S17).
_Avoid_: collision box (the collision block's bounds), hull, BVOL alone

**Hit sphere**:
A person's bone section (a skinned model's COBJ with no faces and no volumes): the sphere a round meets
the bone at, 45 percent of its stored radius plus 1/20 m (the head 65 percent).
_Avoid_: hitbox, bone sphere

**Part name**:
How the editor and the Blender add-on name a model's part: `PN01` (a rig's bone, `BN01`), two digits
and 1-based for the 0-based index the file stores; the file keeps no names of its own.
_Avoid_: Part 0, subobject (the format's word), bone index

**Request**:
What a window, the Shell or the editor MCP asks of the editor's session: a kind, one row of the
request table (`session/request_kinds`: its token, who serves it, the fields it takes, what it
reads and writes, what the unsaved-changes prompt guards of it), and the fields that kind takes,
each meaning one thing whatever the kind (`dir`, `path`, `locator`, `edits`, `new_name`...).
_Avoid_: command (the command line's verbs), message, action (a menu's ACTION is a record)

**Request outcome**:
What one editor request came to: done, or not (refused, did not finish, or waiting on
the unsaved-changes prompt), with the findings it reported and the records its edits made.
The editor MCP reads it; the request's `ok` only says it parsed.
_Avoid_: status (the one-line text the editor shows), result

**Query**:
What the editor MCP, the Shell, the command line or a test asks of the editor's session without
changing it: a name, one row of the query table (`session/editor_queries`: the params it takes,
the list it pages, the view concern whose revision its answer carries), and those params. The
state is one, by section and since a revision; every list a query serves comes a page at a time
(an offset, or a cursor for the output lines and the events, a limit, and the whole count); the
`catalog` query lists every request kind, query, section and concern from the tables themselves.
_Avoid_: request (a request changes the session), view (what the windows draw from), getter

**Verb**:
What `opennova-project` is run to do (`new`, `status`, `validate`, `create-missing`, `import`,
`reimport`, `new-terrain`, `build`, `export`, `request`, `query`): a row of the command line's verb table
(`apps/project/cli_verbs`) naming the requests it sends the editor's session, run headless for
that one run, and the query whose answer it prints, as text or with `--json` as the Shell's
`query_json` gives it. The command line orchestrates nothing of its own (ADR 0046 S13 A7).
_Avoid_: command (a request, a menu's COMMAND, a script's command), subcommand

**Workspace**:
The editor's windows (Files, Document, Preview, Inspector, Problems and Output, the menu bar
and the modals) and their seam to the session: the view every window reads, the typed
requests it raises, and the devices its viewports' canvases draw through (the Shell's, one per
document and kind; none in a headless run or a test). The Shell (`EditorApp`) drains the requests
into the session and hands the devices in (`ui/workspace.h`). What the windows show of their own
(a card or a dialog open, a form's fields, a list's filter, a confirmation waiting) is the session's
too: the view's workspace (`session/view/workspace_view.h`), its parts changed by `set_workspace`
alone (`session/workspace_parts`), so the editor MCP sets and reads it as a person's controls do; a
window keeps only how it shows (scroll, folds, column widths, the docking).
_Avoid_: host, editor host (host is the game host alone), UI (too broad), project (the data)

**Document**:
A file open in the editor: read as the game's loader reads it, changed through its own undo
history, and written back over the file only while the file still holds what it was read from.
That lifecycle is every document's (the base); what a document holds is its kind's. A record
document holds rows of records, each edit naming a record and one of its fields (the def
catalogs, string tables, menus, stylesheets, models, clips and animation tables); a text document
holds a text whose spans its edits replace (a script, a music script, a credits file, a shader, a
configuration); a document of another kind (a terrain's raster) will hold its own content and take
the changes its type makes (an Apply edit's payload).
_Avoid_: file (what is on disk: an open document stands in for it until it is saved), asset (a
project file by its logical name)

**Document view**:
What a document's tab in the Document window shows of it: its type's row of the editor's view
table, one view per open document, which keeps its filter, its order and what it has open for as
long as the document is open. Most types show their records as an outline (a tree of the rows and
what they hold, a list of the rows, or master and detail: the rows beside the selected row's
records as a table edited in place); a type may have a view of its own (a stylesheet's lines, a
menu's screens and windows), or a Main-role viewport filling the tab: a text's script device, or a
mission's 3D view with its outline beside it (a tree with a chip per kind of row) and the Inspector's
form. The selected record's fields are the Inspector's, whatever the type.
_Avoid_: editor (the application), panel, preview (the Preview window's picture), inspector (the
generic form beside it)

**Viewport**:
One document's picture as the game would draw it, of one kind (a menu's screen, a model, a text in
its script device, a mission's 3D view, a texture, a particle file's effect, a HUD layout's HUD, a definition table's record), kept by the session while the document is
open: one per document and kind (a texture's also while Files selects the file, open or not). Its role is Preview (shown by the Preview window while its document
is the last of its kind made active) or Main (the Document tab's view: a text's script device, or a
picture with the outline and the Inspector beside it: a mission's, for which the Preview window steps
aside while that document is active and it has nothing to show, so the picture has the centre);
a document type is shown by one Main kind at most and fed by one Preview kind at most (a mission has
its 3D view alone; its top-down look is a camera command of that view, `top`, not a map). Its state (the size its device draws at where no
canvas sizes the picture, the kind's options and camera) changes by a SetViewport request, every
change a person or a client makes; three changes alone are derived by its follow instead (a menu's
held window following the selection, a model framed when another model first shows, the preview
clock sought when the clip the selection plays changes or a clip event is selected), and nothing
else moves it between two follows. It follows its document (what its change set says changed), the
files its picture read and its state into what its device does next (make the picture again, apply
the state again, drop it, or nothing), a picture the game could not read kept so until the document
changes, and, for a kind that holds for a gesture (the model's scene), a picture made again held
while a gesture is open in its document over one the device holds. Its device is the Shell's (an
offscreen Godot viewport for one document and kind, or a Godot control placed over the tab, the
script device; four kept, the least recently used not drawn since the last pump given up and its
viewport keeping its state), drawn only by the viewport's canvas, which owns the pointer and the keys
and sizes the picture it draws (a control owns them itself in the rect the canvas reserves). A device
may make a picture over several of the Shell's frames, a little each frame (a model: its textures,
its meshes, then its scene and its pose; a menu's screen shown the first time: its textures, then the
screen; a mission: its environment, its terrain's files and tiles, its sky and water, its item
table, a model per graphic, its placement and its pose); the viewport is loading meanwhile, its canvas drawing the last picture the device made,
never a half-made one, and a change that asks for the picture again begins it anew. Headless, it
answers as its envelope (JSON, the `viewport` query), and a client drags and commands through it as
its canvas would (`edit_in_viewport`: a drag one batch under one gesture over the selected records it
moves, a command one request).
_Avoid_: pane (the Preview window's two, which it replaced), device (the Shell's renderer behind
it), preview (the Preview window, or the role)

**Mark**:
A record of a mission as its viewport's overlays draw it and its canvas picks it: an entity's glyph
by its pool, ringed in its team's colour, or an area's footprint on the ground, at its anchor (an
entity's stored position, an area's middle) as the viewport's camera projects it. A mark shows within
the mark range while its kind's marks are on; a click picks the front-most mark within a few pixels
of its anchor (by its anchor, never the model's shape), a marquee every shown mark inside it, and the
primary selected mark carries the handles a drag edits it by (move, height, yaw; an area's edges).
_Avoid_: gizmo (the handles alone), icon, marker (an entity pool of a mission), pin

**Lifted**:
An entity a mission viewport's device draws apart from the placement it last made: one added (an Add,
a Duplicate, a Paste, a drop) or given another item, group or attributes since, built as the
placement builds an entity's own model (its lighting, its mirror flag from its attributes, its
terrain shadow) until the next placement takes it in, a static item's too where the placement would
batch it (past 256 lifted rows the device places the whole mission again). Given back what was
placed (an undo), the placed one shows again. An entity removed is hidden with its terrain shadow,
and shown where it stands when the removal is undone; a moved one is moved in place. None of these
places the mission again.
_Avoid_: spawned (what the game does at run time), dynamic, unplaced

**Palette**:
The mission viewport's list of what its Place tool places: every item the project's catalogs define,
by its name, in a group of what items.def says it is (People, Vehicles, Objects, Buildings,
Decoration and foliage, Markers, Effects), each with the pool a placed one lands in and the model it
draws, searchable, the recently placed first. One is picked, then each click on the picture places
one of it; or one is dragged onto the picture. The viewport's tool (Select, Place, Path stops, Area),
its picked item and its picked path are its options, set by its toolbar and by the wire alike.
_Avoid_: item list, catalog (the items.def file), library, browser (NovaWorld's)

**Script device**:
A text document's Main view (a script, a music script, a credits file, a shader, a configuration, a
particle file):
a Godot code editor placed over its Document tab, which owns the pointer and the keys there and
shows the document's text as it stands (the text is the document's: an undo, a reload or another
client's edit comes back into the control). What is typed goes to the document as spans replaced,
each run of typing one gesture and one undo step, Undo and Redo the editor's and never the
control's; the findings are marks in its gutter, their messages on hover and the worst one's first
sentence after its line's text; a script's words the WAC compiler knows (its keywords, commands and
operands) are coloured, nothing else; as a script is typed it offers what may complete the word (the
commands and keywords, or the names the command's parameter takes), says what a word is on hover, and
a Ctrl+click goes where the word is defined (ADR 0046 S15, `session/script_assist`); a Go to or a
Problems row selects its place. A file its text form cannot carry shows read only, and so does every
text while an operation holds the documents. Where no window draws it (headless) or something is
drawn over the tab (a menu, a dialog), the document's lines show instead, read only.
_Avoid_: script editor (the whole editor), code view, text view (the lines shown read only where no
device draws), CodeEdit (the Godot control behind it)

**Environment document**:
A mission's environment (a .env: its sky, light, fog and water) open in the editor (the deep-integration
plan's DI-19a), read and written through the engine's own reader and writer: one record, the
environment, whose fields are the keywords the game reads in the units the file writes them (a time as
HHMM, a fog distance in whole metres, a water height in half metres, a colour as its three bytes), and
its time-of-day keyframes, at most 16, each its time and its twelve colours. The cloud layers name
textures and the sun, moon, glare and star models, each a Go to; the Inspector heads it with the missions
that run on it, each with the terrain it pairs it with, what its header sets over it (fog, water) and
where its water plane comes from. A line the game reads otherwise than written is a finding of its line;
Save writes the file in the editor's layout, the game reading the same environment.
_Avoid_: env file (the file alone), weather (the runtime's state), sky (one part of it)

**Texture document**:
A texture file of the project (a .tga, .mdt, .pcx, .dds or .png) open in the editor, read as the
game reads it: by the reader its name picks (a .tga or an .mdt the game's TGA reader, which takes every
file's rows bottom up whatever its header says; a .pcx the PCX reader, every colour opaque; a .dds
D3DX's loader, which reads the bytes by their content; a .png the menus' reader), never by what the
bytes would say elsewhere. It holds the file's texels (each level a DDS stores, an indexed file's
palette and indices) and what the texture is in a modder's words (its format, size, texels,
compression, alpha, mip levels, palette, whether the game loads it and why not); a file the game
cannot load still opens and says why. Its Document tab is its texture viewport beside those facts,
its palette and what uses it; the Preview window shows the texture Files selects before it is opened.
Its edits are whole-image ones (a texture operation: a resize, an alpha, its stored form, an upside-down
TGA's rows, an 8-bit PCX's palette indices), each one undo step that makes the file anew through the
editor's writers, which Save writes; no paint program. A file an import makes is made by its import's
options, never edited in place (ADR 0046 S18).
_Avoid_: image (the decoded texels alone), bitmap, sprite, asset (a project file by its logical name)

**Texture role**:
One way the game uses a texture file (ADR 0046 S18): a model's diffuse or normal map, a terrain's
colour map or foliage map, a sky's cloud layer, a particle's graphic, a HUD's alpha-only art, a menu's
image, a mission's loading screen, and so on, 46 in all. Each names the loader that picks the file for
a name and the reader that decodes it (a model row's: the `.dds` beside the name first; the HUD's: the
`.FULL` and `.ALPHA` suffixes, a PCX made white with its alpha from blue), the formats that work, the
size the game needs, what the alpha means there, and what the game does with a wrong or missing file,
each with its witness. A file has as many roles as uses; the role, not the file, decides what is
right.
_Avoid_: texture type (a model texture row's type field), usage, slot (a model row's slot field)

**Texture use**:
One way a texture file is used (ADR 0046 S18): a reference to it from another file (a model's material
row, a terrain's key, a sky's cloud layer, a particle's graphic, an item's HUD image, a menu's image) with
the role that reference gives it and what the referrer says of it (the material's shader and cut-out),
or a name the game opens itself (the HUD's art, the weather, the scars). A use whose loader opens another
file of the name (a `.tga` beside the `.dds` a model row loads) does not read this one. A texture's tab
lists its uses as Used as.
_Avoid_: usage (the graph's references to any file), referrer (the file that makes the use)

**Texture thumbnail**:
A texture file as a small picture (ADR 0046 S18): read by the reader its name picks, what a use's
loader makes of its texels applied (the HUD's alpha alone, a sky map's PCX alpha from its palette),
shrunk to 128 pixels a side, with its size and format in words. The session keeps them by the file's
stamp and makes them off the frame; the editor shows one wherever a field names a texture (the file the
reference's loader opens, a `.tga`'s `.dds` where that is what the game loads), in a texture field's
picker, and as the tooltip of a texture in Files, Problems and the outline.
_Avoid_: icon, preview (the Preview window), image

**Texture viewport**:
A texture's picture (the Main view of a texture document, and the Preview window's for a texture
Files selects, read from its file while it is not open): its texels at a zoom (fitted, or a scale about
a middle texel, which the wheel steps about the pointer and a drag pans), through its colour, one
channel or its alpha as grey, or its colour over a checkerboard by its alpha, at a mip level, as the
file holds it or as the game draws it for one of its uses (the use's loader's texels, a cut-out's test,
a tile atlas's cells, a model row's alpha drawn as the game reads it: opaque where it is a specular brightness),
at the chain the game builds of it and at an object texture detail, or a normal map lit, or beside the DXT
texture its `.dds` would hold (a **compare**: split, the DXT texture alone or their difference, with each
level's error; a `.dds` an import makes beside its source); each a SetViewport. A point of it names the texel
under it (its column and row in the level shown, its value,
its palette entry), never a record. Its device draws the texels the portable decode made, texel for
texel where a texel covers a pixel or more.
_Avoid_: image viewer, preview (the Preview window, or the role)

**Texture budget**:
What a model texture costs the game (ADR 0046 S18): the device texture its row's loader makes of the file it
opens (its sides after the game's halvings, its levels, its format, and every level's bytes, which the game keeps
in its own memory) at each of the four **object texture detail** levels (game.cfg's `object_texdetail`, 0 the
lowest to 3 full: one or two halvings of a diffuse or detail texture), and what the `.dds` its loader reads first
would cost instead. Said under each use in the texture's tab and on the wire; past 16 MB it is a warning. The
project's budget is every texture the game makes for the model rows (one a name written), costliest first, with
its totals: the `texture_budget` query, Files' By cost.
_Avoid_: footprint, VRAM (the game keeps every level in its own process too), file size (what the disk holds)

**HUD viewport**:
A HUD layout's picture (hudpos.def's Preview, beside its text): the game's own HUD drawn over the layout
as Save would write it now (the runtime's HudOverlay through the engine's layout fill and frame
compiler, and the game's view effects over it), at a screen size, for a player whose state its options
choose (the stance, a weapon of weapon.def with its clip and reserve, the health, the binoculars' or the
goggles' view, a hit's damage vignette, the HUD detail level, the crosshair style); each a SetViewport.
A point of it names the HUD element under it (the HUD's walk records what each of its elements drew),
the hudpos.def lines that place it and the textures it draws, never a record; a click picks the
element, whose lines and textures are each a Go to.
_Avoid_: HUD editor (nothing it does edits the layout), preview (the Preview window, or the role)

**Preview clock**:
The one clock every viewport reads: a model's part animations, flipbooks and colour generators by
its milliseconds, a clip by its game ticks, and a menu's frame clock by its milliseconds, which a
focused edit box's caret reads (it blinks as the clock plays, the frame drawn again as the caret's
half of the blink changes and never configured again), and a particle effect by its game ticks (the
effect preview's: its spawn stepped a tick at a time); an environment's time of day is to read it once
it has a viewport. It runs while it plays, at its rate, as the Shell's
frames pass; a SetViewport plays, pauses, sets its rate or seeks it, and a viewport seeks it as it
follows (a clip newly chosen starts at tick 0, a clip event selected holds the clock on the tick the
clip first samples it; an effect newly shown starts at tick 0).
_Avoid_: clip clock (the model preview's own, which it replaced), game clock (a running match's),
tick (the game's 62 Hz step, which it counts)

**View event**:
A one-shot ask a request makes of one of the editor's windows, which the session's view keeps
until the window it is for has had it: show a record's field (a Problems row, a Go to), show a
file in Files, ask a name's new name (Rename everywhere), say how the settings' Apply came out,
take a new import plan's checks. Each ask is an event of its own, with its place in the view's
sequence (the last 64 kept), so the same ask made twice is two asks; the workspace hands each
to the window it is for, which holds it until it draws (64 at most) and takes it once, passing
over an ask a newer one or the selection has overtaken since. The editor MCP pages them by their
place.
_Avoid_: serial (the per-ask counters the events replaced), reveal state (the view keeps none),
notification (the OS's), signal (Godot's)

**Navigation history / place**:
Where the person has been in the editor, which Back and Forward take them to again, as a browser's
pages (`session/navigation_history.h`, the session's `NavigationController`). A place is the pane that
shows it and the file: a document's tab with the record selected there (by its locator once the
document is read again), a text's line a Go to showed, a file's page (with the record a Go to marked
there: a Go to always lands, on its page where the editor has no editor for the file), or Files on a
file. A step is a
move the request table marks as one (`navigates`: a document switched to, a Go to, a Problems row, a
find's hit, another of a menu's screens, Show in Files), whoever raised it; a record picked within what
shows, an edit or a camera move is none, and a run of quick steps is one. The open project's alone.
_Avoid_: undo (a document's own history of edits), breadcrumb (the outline's path to a record),
selection history (a selection within what shows is no step)

**Record / owner**:
A row of a document or anything nested in one, at any depth; the record that holds a
record is its owner (a menu window's owner is its parent window, a root window's is
the screen). A record's **locator** is its place (the row's index, then each
collection's kind token and index, `0/window:0/window:2`), which a reload of the same
file finds it again by; its path is every name from the row down
(`STARTUP/MAIN/EXIT`), which is what findings and graph edges show.
_Avoid_: node (the core's type for a row), child (an identity field, not a relation)

**Record reference / record set**:
A field naming a record of its own file by its index among the file's records of one kind, in
the file's order: a model's generator, track or light naming one of its CTRL registers, a part
animation's frame byte naming a rotation frame, a mission's stop naming a marker or a parameter an
event (a mission's entity and area are named by their SSN and zone id instead, which no edit
renumbers). Those records are the file's record set of that kind, each found by its index: the
picker offers them by index and name, and each shows what names it (Referenced by). An edit that
adds, removes or moves one of them changes, in the same undo step, every index that named a
record it moved (an index past the set staying past it), and is refused while a field the game
reads still names a record it removes. An index past the set is the file's own finding (what the
game makes of it), never a missing reference.
_Avoid_: id (an id stays with its record; an index is its place), link, pointer

**Table shape**:
How a type of records tells the editor's core what its records are, in one form for every format: a
table with a row per kind of record, each kind its labelled fields (what the editor shows of a field,
how it reads and writes the record in the units its file writes, the values it takes by name, what the
record and the records it lies in decide of it where they do) and the lists it holds (a list's records
of one kind or of several in one order, how one goes in, comes out and is copied). The core answers
the outline, the Inspector, every read and write and every add, copy, removal and move from the table,
keeping each record's identity beside it; the type keeps its parse, its writer and the rules no row
can say (a screen keeps a root window). A record owns what it holds: what one record names of another
by its index is a Record reference, never a list reaching past its owner. A new kind of record, a new
field or a new family of a catalog (a def table the editor did not open) is rows of tables, not code of
a document.
_Avoid_: schema (one kind's fields: the part of the table the Inspector reads), property table (the
per-format tables the shape replaced), plugin (there are none: the tables are compiled in)

**Band**:
The run of a document's rows of one kind where its file fixes their order (a mission's: the mission
row, then its items, buildings, markers and organics, its 128 waypoint paths, its area triggers and
its events, the order its writer writes them). A row added, duplicated, pasted or moved lands inside
its kind's band, never outside it; a band the file holds fixed (the mission row, the paths) takes no
row and gives none up.
_Avoid_: section (a part of the file's bytes), group (a mission's AI group), pool (the runtime's
entity storage an entity's kind spawns into)

**Menu preview**:
The menu's viewport in the Preview window: the editor's render of the previewed screen (the last
menu screen selected) through the runtime's own menu frame, reading the project's files
the way the game reads its mounted files, the open documents standing in, so it shows
what the game would draw were the menu saved now; when it cannot, its status says why
(no project, no menu, no screen, a menu the game could not read, a screen missing from
it). Headless, it answers as its viewport's envelope.
Its primary window carries eight drawn handles: a drag moves or resizes the window (one
gesture), writing the fewest POSITION edges that make the game's own layout land it where
it was dropped; a drag of any selected window moves every selected one.
_Avoid_: play (a running game), render check (the headless validator's notes)

**Model preview**:
The model's viewport in the Preview window: the editor's render of the previewed model (the last
model, clip or animation table document made active) through the runtime's own object
renderer: a model document as it would save, a clip or a table played on its rig's model
(the graphic an item pairs with the table, or one the author picks) at the preview
clock's ticks. It draws the level the
game would pick at the camera's distance (Auto) or one held, holds CTRL registers at a
value, and marks what the model's records place (user points, lights, part pivots) where
the game puts them on the posed model; a click selects a marker's record and a drag of
the selected one moves it or turns its axis (every selected marker moving as far). Headless, it
answers as its viewport's envelope.
_Avoid_: viewer (it edits), avatar preview (the game's player-info portrait)

**Effect preview**:
A particle file's viewport in the Preview window (ADR 0046 DI-14): the effect it defines that its
options name played by the engine's own effect scene as the game spawns one alone (at the spawn point
with no orientation, every EMITVECTOR member emitting around +Y), over the catalog the game would load
were the project saved now (every particle file in the effect system's order, the open documents
standing in), so it shows what the game spawns for the name: the first definition registered (another
file's where that one comes first, said, with a Go to it), its members all or nothing, its tables. It
plays on the preview clock's ticks, spawned again as it dies while it loops (the editor's aid) and
pre-aged on a seek; an edit of any particle file shows at once, the effect at the age it had; a Go to of
an effect's name opens the file at its id and the preview shows it. Its device draws through the game's
particle renderer, single-sampled as the game's view is. Headless, it answers as its envelope: what
the name resolves to and the playing cycle.
_Avoid_: particle editor (ParticleEdit's: nothing here edits but the text), effect viewer, emitter
preview (one effect spawns several emitters)

**Definition preview**:
A definition table's viewport in the Preview window (ADR 0046 DI-21): the record the selection lands in
drawn as the game draws the thing it defines. An item in its State (alive as its mission starts;
destroying, its death as the game runs it from the clock's tick 0; its husk, the wreck standing; its final
husk, the model its death pieces are cut from), its graphic or, seen by its enemies, its enemy graphic,
the husk swapped in with the pieces' sections gone and the destroy fade on its registers, its particle
slot and its death's Dead, Fire and Other banks spawned where the game spawns them through the engine's
effect scene, its death sound heard as the clock runs from the death, a person posed as its spawn poses
it; a weapon's third- or first-person model; an ammo's round as the item its tracer id names. The record
as it stands, what it names as the game would read it were the project saved now. A record picked is no
step of the navigation history (a menu's screen is). Headless, it answers as its envelope: what it draws,
the death in order, the effects it spawned, the sounds it fired.
_Avoid_: item viewer, def preview (a powerup row or a carry limit draws nothing), model preview (the
model's own document's picture)

**Weapon range**:
Where a weapon record fires in the Definition preview (ADR 0046 DI-22): a world of the editor's own
holding the game's local player with the weapon in hand, before a target wall whose face is the surface
picked, run tick by tick through the game's own legs (the weapon pump, the rounds' flight, the impacts,
the fire-sound queue) from the gestures (Fire, Hold fire and its Release, Reload, Scope, Switch) the
editor stamps on the preview clock. The run is a function of its gestures: the clock stepped back runs it
again. What it hands the game's presenter is drawn where the presenter draws it: in first person the
action legs at the gun's points, in third person a shot as another sees it (a soldier's, through the
ammo; another player's, through the weapon's FIRE and RECOIL rows).
_Avoid_: shooting range (the game has none), firing test, gun sim

**Rig**:
What an animation plays on: an animation table (its reset clip the bind) or a lone clip
(its own bind) over a model's bone table (its parts' pivots and parents), loaded through
the game's own loader.
_Avoid_: skeleton (the rig's bones alone), armature (Blender's)

**Selection / primary**:
The records selected in the active document, of any of its rows (a marquee over a mission's
entities, windows of several screens). The primary is one of them, the one selected last or
the one a marquee names: the inspector's form, the preview's handles and the place a new or
pasted record goes follow it, and an arrange aligns the others to it. Several records of one
kind share one form in the inspector, where a change sets every one of them in one undo step.
Each open document keeps its own while another is active; every change of one, and every one put
back, takes a serial no selection had before. A record the document does not hold is never
selected, and the copies a Duplicate makes are selected with the primary's copy the primary.
_Avoid_: focus (the keyboard's), active (the active document, not a record)

**Arrange**:
The selected menu windows aligned by an edge or their centres to the primary's, spread
with equal gaps between them, or moved in the drawing order among their siblings (the game
draws a window's children in the file's order, a later one over an earlier one), one
undo step, the moves written by the same rules as a drag.
_Avoid_: layout (the runtime's POSITION solve), z-index (the format has none: the order is
the file's)

**Compiler note**:
What the menu frame compiler makes of a screen where the picture may not be what the
author meant (a colour that reads transparent, a label cut short, a window with no area,
a row the game ignores, a file that did not load), as a code on the window, list record
and field that cause it, with its basis: witnessed (the game does this), port policy
(OpenNova's own choice), deferred (a known gap, a divergence-ledger row). The engine
keeps codes; the editor words them. Notes observe: the picture is the same with or
without them.
_Avoid_: warning (a Problems severity; a note is one only through the render check),
parse note (what the reader leaves out of a file)

**Render check**:
The menu type's project check: the editor's headless render of every menu screen of the
project with each validation, the way the game draws it: its compiler notes that are a
consequence the author may not mean become Problems rows on their records, never a build's gate
and never a finding the asset graph already makes (a name the project lacks is the graph's).
_Avoid_: preview (the one screen shown), validation (the document types' own checks)

**Use check**:
A finding about a project file that reads what other files make of it: a variable of the
stylesheets the game reads that no menu uses, or that a menu uses as a colour it is not. The editor
makes it from the asset graph after every file's own findings, which its document type makes from
the file alone.
_Avoid_: cross-file validation (the use checks are one table, by the kind of file), validation (a
file's own findings)

**Project check**:
A document type's own check across the project's files, run with every validation after each
file's own findings: it reads the files itself, keeps what it made from one validation to the
next and makes again only what moved, and says whether its findings moved (the menu type's render
check renders a menu again only when it, a file its screens read or a variable it names changed).
Its findings are Problems rows after the build's gate, never in it, and never a finding a use check
or the asset graph makes. A use check is the other cross-file finding, one whose rows are in the
gate: a function of the asset graph, keeping nothing. Which one a finding is, is chosen by whether
it may block a build (and in the gate an error blocks only when its code's row says so: a missing
reference's does not, ADR 0046 S14).
_Avoid_: use check (in the gate, stateless, one per kind of file), validation (a file's own
findings)

**Finding code**:
A finding's stable dotted token (`reference.missing`, `style.line_ending`) and the row it is made
from, which the finding keeps: what Problems offers for it (its fixes; a Rewrite, with what writing
the file again does), whether it says the file does not serialize (its Save refused, no Rewrite
offered), where Problems takes it (the record and field in the file's document, or the file itself
in Files), the group it shows under (its family), where it comes from (its own part, the asset
graph, the render check), for a compiler note, whether it is a Problems row at all, and whether an
error of it refuses a build (ADR 0046 S14: every code's does but a missing reference's,
`reference.missing`, which is listed and fixable and blocks nothing: the shipped game carries such
names and runs; what the game cannot start without is the manifest's rows). The
editor's own codes are one table and each document type declares its own, a family of its name
(`menu.`, `style.`); every finding is made from a row, so a code no table declares cannot be made.
The wire's `code` is the token.
_Avoid_: error code (a finding may be a warning or a note), message id, diagnostic code (Diagnostic
is the record's type, the code its row's token; a format reader's own codes, the stylesheet
reader's `mns::DiagnosticCode`, are what its type keys rows by)

**Canvas**:
Where the editor shows a device's picture and takes the pointer and the keys over it: it tells
a click from a drag (one gesture at a time), zooms and pans the picture, and draws over it the
shapes its kind makes (outlines, handles, markers, the marquee's box) with the cursor they ask
for. The kind (the menu's, the model's) says what a press takes, what a drag writes and what is
drawn; the canvas is the same for every kind. A viewport's canvas draws its device's picture:
the canvas owns the input, the viewport its state, and what the device renders is the picture.
_Avoid_: overlay (one shape drawn over the picture), view (a document's view in the Document
window), picture (what the device renders, which the canvas shows)

**Gesture**:
The edits one continuous action on a canvas makes (a drag of a handle, an arrow key held, a gizmo
over several entities): they carry one token and fold into one undo step over every row they
change (each row's version before the gesture and its latest after) until it ends (let go, or the
canvas stops drawing it: one end, for the document it began in); a batch of it that adds, removes
or moves a row is a step of its own and ends it. The view keeps the gestures open, one per document
(its token, and for a client's drag over the wire when its last sample came): one also ends with an
undo, a redo or a save of its document, its document closed or read again, every edit group ended
(Build, Play) and another gesture's batch in its document; a canvas ends its own, while one of the
wire's also ends with any other request on its document and with ten seconds with no sample (a
client that went away). The Problems wait for every one to end. A viewport whose kind holds for a
gesture (the model's scene, costly to make) keeps the picture its
device holds while one is open in its document, its markers following each edit, and makes the
picture once when it ends; a change only the state shows (a model's user point moved) applies
meanwhile; a menu's picture is made again each edit.
_Avoid_: transaction (the rename's), group (a coalesced typing burst of one field)

**Batch**:
Edits on any rows of one document applied as one undo step, each against the rows as the ones
before it left them (each row an edit touches copied once), nothing committed when any is refused
or the document's type refuses the step: records' fields, rows added, duplicated, removed, moved
and pasted, and file-wide values together. A later edit may name a row or a record an earlier one
made (in the editor MCP, by the label its edit gave it), so one step adds a window and fills it
in; one naming a row an earlier edit removed is refused. A new name is its record's edit alone:
what names the record keeps the old name until Rename everywhere rewrites it.
_Avoid_: transaction (the rename's), gesture (edits folding one after another until an end),
follow (the same-file rename S13 D5 removed)

**Change set**:
What changed in a document between a state a window or a preview last read and the state it is
in, in the words of its kind: a record document's rows added, removed and changed, whether rows
moved among one another and whether the file-wide state changed; a text document's spans (each run
of text that changed, a removal a span of no length); a raster's regions later. When the document
cannot say (it was read again, or its history no longer holds that state), everything changed.
What keeps something made of a document follows it: a viewport makes its picture again only for a
change of what the picture reads (a menu's screen and the screens before it in the file, whose first
load of a texture fixes the band the screen draws, not a screen after it; a model's drawn tables, not
its user points, which its markers alone show), an outline makes again only a changed row's lines and
re-orders the rest, and the selection asks only about the rows that changed.
_Avoid_: diff (of files on disk), delta, dirty (unsaved edits, against the saved file)

**Text document**:
A document of text (a script, a music script, a credits file, a shader, a configuration or a text),
held as the game reads it, in its code page (one byte a character, which is what a column counts),
as lines, its places `line:column`. Its one change is a span replaced. Its type reads and writes
the form its file is stored in (a music script's bytecode, a credits file's CBIN form and a shader's
SCR form are held as their text and written back in the form, byte for byte while the text is left
as it is), checks it through the game's own reader where the editor has a port of it (the WAC
compiler, the ConfigFile text reader) or else the toolchain that writes the form (the MUS compiler,
which is ours, not the game's), writes each line end as its game reader ends a line (CR LF for a
script and a credits text), and names the references its text makes (a script's operands), each at
its span. A file its text cannot carry as it is (a music script's message handler) opens read only.
_Avoid_: source (an import's input), code (the bytecode a compiler makes), script (one kind of text
document)

**Span**:
A run of a text document's text: the line and the column it starts at (both from 1) and how many
characters it covers (a line end counting its own). An edit replaces a span; a reference a text
makes is at a span, which Rename everywhere rewrites and a Go to opens the document at; what changed
in a text is its spans.
_Avoid_: range, selection (the records a document's selection holds), offset (the byte a span's
place maps to)

**Build**:
The one operation behind Play and Export: validate the project, route every asset into
the canonical archives (`language.pff`, `localres.pff`, `resource.pff`) and the
mandatory loose files, write through the streamed PFF writer, verify through the VFS,
and publish an immutable `.opennova/build/play/<build-id>/` directory, which nothing writes
once published (Play runs in a run directory). Incremental by per-archive input hash (with the
archive writer's version): each file's content hash comes from the **build cache**
(`.opennova/build_cache.json`, machine-local) while its size and last write hold (a file written
within two seconds of the build is read by every build until it settles; a `rehash` build reads
every file), so a build reads only the files that changed, and an archive whose content is
unchanged is linked from the last good build (copied where the file system cannot link, never
through a name already there). An import source and a file the editor does not know are left
out.
_Avoid_: pack (a step inside a build), export (a build copied to a chosen directory),
stage (the retired retail-staging vocabulary)

**Operation**:
A long job of the editor's session: opening a project (the game install's names, the import pass,
the scan, the requirements), a refresh (a Rescan, a Reimport), an import's plan and its write, a
rename's commit (a file at a time, then the one step that writes) and a build. One runs at a time,
a step at a time, each step within the frame's budget, so the editor keeps drawing while it runs;
the request that starts one returns at once, naming it. Its progress shows while it runs; nothing
it makes reaches the view until it finishes (a project being opened is not the open one yet), and
a Cancel stops it between two steps with nothing of it in the view (an import once it wrote, a
rename once it committed, run to their end). On disk a cancel leaves only what an Open's or a
refresh's import pass had written by then: the outputs of the sources it reached, and its cache
once the pass had ended. It declares what it reads and writes (a build reads the project's
files), as each request kind does, and a request that writes what it reads or writes, or reads
what it writes, meets the busy gate, the two rows saying what happens: it is refused (a save while
a build packs, an edit while a project opens), joins the operation (a Build or a Play onto a
build), takes its place (a new import plan over a running one) or cancels it as it commits (a
project switch, Quit). A request that starts an operation of its own waits for the one that runs;
one that conflicts with nothing it holds (an edit beside a build, a selection, a query) goes on.
The validation that follows one steps a file at a time too, but is no operation: it holds nothing,
an edit starts it again, and an operation that reads the graph (an import's plan, a rename) runs
its remaining steps first.
_Avoid_: task, job, background work (nothing runs on another thread)

**Play**:
Build, then launch the game runtime (`opennova.exe -- --resource-dir <build>
--mcp-port <n> [--mission <name.bms>]`) as the editor's one managed child through a
`PlaySession`, in a run directory; Stop ends it. The editor tails the session log until the
runtime MCP answers, then drives it there. Play starts the game at its menu; **Play mission**
(Ctrl+F5) starts it in the active document's mission, a `.bms` of the project (the document
itself, or the mission whose sidecar it is). A mission the project does not hold is refused
before anything is built; one that does not load is a Problems row until the next Play. Play in
the game install starts at its menu whatever is asked (the stock game takes no mission). Play
saves every file with unsaved edits first (the editor's `save_before_play`, on by default; off,
it asks as Build does).
_Avoid_: run (ONED's vocabulary), preview (an in-editor render, not a running game),
"see in game"

**Play from here**:
Play of a mission with the player starting at a point of it: the ground under the mission view's
camera (its button, Build > Play from here, Alt+F5) or under a point of its picture (the picture's
right-click menu), facing the way the camera looks. Neither game takes a place on its command
line, so the start is a **start marker**: the build's copy of the mission in the run directory
has the markers its single player deploys at moved there (one added where it has none), never the
project's file nor the build, and the game places its player there by its own spawn selection,
OpenNova and the game install alike (the game install starts at its menu, where the mission is
chosen).
_Avoid_: teleport, spawn point (a deploy zone's), warp

**Play log row**:
A Problems row of what a Play's game looked for and did not find, on the file of the project that
names it and the record and field naming it, with that reference's or required file's fixes and a
Go to there. OpenNova says each miss once on its log (`resource missing: <kind> "<name>"`); the game
install names nothing it lacks, so its rows are read from what its file log shows it opened (the
missing references of a file it read; the boot's text tables it did not open) and from its graphics
log (`ghw.txt`: a mission begun and never finished), once its game exited. Each mode's rows stay
until the next Play of that mode, or until the project closes.
_Avoid_: crash log, error list, log scrape

**Run directory**:
Where Play runs the game: `.opennova/run/<mode>/<n>/` (the Play's mode, `runtime`, `install` or
`strict`; n from 1), the game's working directory, the log Play tails (`session.log`; the game
install's own, `_filelog.txt`, read once its game has exited, never while it runs) and the saves
the game keeps beside itself (`weapon.sav`), so the build it runs from stays as the build wrote
it. Play in the game install puts there what the install's game needs beside it: the build's
files (linked; one the game may write, a `.cfg`, `.sav`, `.coo` or `.txt`, copied), the
install's executable and Bink DLL, a `game.cfg` (the project's own, else the install's) and the
install's `player.sav` and `weapon.sav` where the project has none; Strict Play, the build's
files and the executable and Bink DLL alone. It records its game (pid and creation time) while
the game may run; each Play takes the first free one of its mode, passing one whose game may
still run, and keeps what the game wrote there in that mode's runs before (its `game.cfg`, its
saves; Play fresh empties it). A Play never touches another mode's run directories.
_Avoid_: build directory (what the build publishes, never written after), working copy, stage

**Strict Play**:
Play in the game install as a player's drop-in: the run directory holds the build's files and the
game install's executable and Bink DLL, nothing else of the install (no configuration, save or
score), and the game is launched without `/d`, so it reads its archives first and writes its own
configuration (`play_in_install_strict`, an editor preference beside Play in the game install). A
first run that wrote its own `game.cfg` and quit soon after it started is started once more; an
expansion is refused until a project can name its base game's build. Play in the game install
without it is the day-to-day run.
_Avoid_: retail mode, clean run, vanilla run

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
