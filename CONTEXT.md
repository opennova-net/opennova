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
agent drives the editor's typed request seam: the state, requests, documents,
problems, the asset graph, the menu preview, the menu tools (`editor_menu`: a menu's
tree, a batch by label, a list replaced, a menu's findings), Build and Play (ADR 0046
S6d, S9m, `docs/mcp.md`). The game a Play starts is then driven through its own runtime
MCP.
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
One of the two applications exported from the `godot/` project: the OpenNova game
runtime (`opennova.exe`, ADR 0045 and ADR 0048; its Play export adds the runtime MCP
for the editor) and the OpenNova Editor (`opennova-editor.exe`, ADR 0046). Backend
services and development tools are outside this taxonomy (ADR 0015).
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
authored from scratch. Today it is only the placeholder main menu that says the
OpenNova game is coming and offers **PLAY RETAIL**. The game mounts it when no
`--resource-dir` is given (ADR 0048).
_Avoid_: fixtures (test-only data), retail data

**Serve mode**:
`opennova.exe` hosting a match without being a player: the host screen's retail Serve Only
server type (`SERVERTYPE` = 1), runnable windowed or `--headless`. A mode of the game product, never a separate binary,
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
game data), never redistributed; Play in the game install runs a build there.
_Avoid_: retail directory, retail root, retail files (the names before ADR 0046 S13 A4), resource
dir (the runtime's `--resource-dir`, which may be a build)

**Logical name**:
The flat, case-insensitive name the engine resolves an asset by (`main.mnu`,
`items.def`), at most 16 bytes as a PFF entry. A project asset's identity; its path in
the tree is organization only. Uniqueness and length are checked on output names.
_Avoid_: path (when the engine-facing identity is meant), resource name

**Import / sidecar**:
Bringing a non-native source (an image, a GLB scene, a wave) into the project the Godot
way: a committed `<file>.import` sidecar records the importer, its version, options,
output logical names and the source's content hash, and nothing a checkout changes; the
outputs are regenerated into `.opennova/imported/`, in a directory named by a hash of the
source's path, and packed like native assets. The **import cache**
(`.opennova/import_cache.json`, machine-local), keyed by the source's project-relative
path, keeps each source's size and last-write time and the record its outputs were made
from, so only a real change imports again. A file is an import source only while its
record is there: importing it writes the record, and a `.png` with none is a texture the
build packs as it is.
_Avoid_: convert (the runtime never converts), asset pipeline (the retired Python route)

**Base layer**:
What a read-only dependency mount (a game install a project builds on) gives the project's
asset graph: its files and the names they define, read once and never edited. A lookup by name
tries the project first, then the base layer, whose file of a name the project also has is
hidden with the names it defines (project assets win); the base layer makes no reference and no
finding of its own.
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
scans for beside the `.bms` files.
_Avoid_: mission (the `.bms` the game loads), map pack

**Request**:
What a window, the Shell or the editor MCP asks of the editor's session: a kind, one row of the
request table (`session/request_kinds`: its token, who serves it, the fields it takes, what it
reads and writes, what the unsaved-changes prompt guards of it), and the fields that kind takes,
each meaning one thing whatever the kind (`dir`, `path`, `locator`, `edits`, `new_name`...).
_Avoid_: command (the command line's verbs), message, action (a menu's ACTION is a record)

**Request outcome**:
What one editor request came to: done, or not (refused, did not finish, or waiting on
the unsaved-changes prompt), with the findings it reported. The editor MCP reads it;
the request's `ok` only says it parsed.
_Avoid_: status (the one-line text the editor shows), result

**Workspace**:
The editor's windows (Files, Document, Preview, Inspector, Problems and Output, the menu bar
and the modals) and their seam to the session: the view every window reads, the typed
requests it raises, and the devices its previews draw through. The Shell (`EditorApp`) drains
the requests into the session and hands the devices in (`ui/workspace.h`).
_Avoid_: host, editor host (host is the game host alone), UI (too broad), project (the data)

**Record / owner**:
A row of a document or anything nested in one, at any depth; the record that holds a
record is its owner (a menu window's owner is its parent window, a root window's is
the screen). A record's **locator** is its place (the row's index, then each
collection's kind token and index, `0/window:0/window:2`), which a reload of the same
file finds it again by; its path is every name from the row down
(`STARTUP/MAIN/EXIT`), which is what findings and graph edges show.
_Avoid_: node (the core's type for a row), child (an identity field, not a relation)

**Menu preview**:
The Preview window's menu pane: the editor's render of the previewed screen (the last
menu screen selected) through the runtime's own menu frame, reading the project's files
the way the game reads its mounted files, the open documents standing in, so it shows
what the game would draw were the menu saved now; when it cannot, its status says why
(no project, no menu, no screen, a menu the game could not read, a screen missing from
it). Headless, it answers as JSON.
Its primary window carries eight drawn handles: a drag moves or resizes the window (one
gesture), writing the fewest POSITION edges that make the game's own layout land it where
it was dropped; a drag of any selected window moves every selected one.
_Avoid_: play (a running game), render check (the headless validator's notes)

**Model preview**:
The Preview window's model pane: the editor's render of the previewed model (the last
model, clip or animation table document made active) through the runtime's own object
renderer: a model document as it would save, a clip or a table played on its rig's model
(the graphic an item pairs with the table, or one the author picks) at the preview's
clip clock. It draws the level the
game would pick at the camera's distance (Auto) or one held, holds CTRL registers at a
value, and marks what the model's records place (user points, lights, part pivots) where
the game puts them on the posed model; a click selects a marker's record and a drag of
the selected one moves it or turns its axis. Headless, it answers as JSON.
_Avoid_: viewer (it edits), avatar preview (the game's player-info portrait)

**Rig**:
What an animation plays on: an animation table (its reset clip the bind) or a lone clip
(its own bind) over a model's bone table (its parts' pivots and parents), loaded through
the game's own loader.
_Avoid_: skeleton (the rig's bones alone), armature (Blender's)

**Selection / primary**:
The records selected in the active document, all inside one row. The primary is the last
one selected: the inspector's form, the preview's handles and the place a new or pasted
record goes follow it, and an arrange aligns the others to it. Several records of one kind
share one form in the inspector, where a change sets every one of them in one undo step.
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
The editor's headless render of every menu screen of the project with each validation,
the way the game draws it: its compiler notes that are a consequence the author may not
mean become Problems rows on their records, never a build's gate and never a finding
the asset graph already makes (a name the project lacks is the graph's).
_Avoid_: preview (the one screen shown), validation (the document types' own checks)

**Canvas**:
Where the editor shows a device's picture and takes the pointer and the keys over it: it tells
a click from a drag (one gesture at a time), zooms and pans the picture, and draws over it the
shapes its kind makes (outlines, handles, markers, the marquee's box) with the cursor they ask
for. The kind (the menu's, the model's) says what a press takes, what a drag writes and what is
drawn; the canvas is the same for every kind. A viewport is a canvas backed by a device; what
the device renders is its picture.
_Avoid_: overlay (one shape drawn over the picture), view (a document's view in the Document
window), picture (what the device renders, which the canvas shows)

**Gesture**:
The edits one continuous action on a canvas makes (a drag of a handle, an arrow key held): they
carry one token and fold into one undo step on their row until it ends (let go, or the canvas
stops drawing it: one end, for the document it began in), and the Problems wait for that end.
_Avoid_: transaction (the rename's), group (a coalesced typing burst of one field)

**Batch**:
Several edits on one row applied as one undo step, each against the row as the ones before
it left it, nothing committed when any is refused; a later edit may name a record an
earlier one made (in the editor MCP, by the label its edit gave it), so one step adds a
window and fills it in. A new name is its record's edit alone: what names the record keeps
the old name until Rename everywhere rewrites it.
_Avoid_: transaction (the rename's), gesture (edits folding one after another until an end),
follow (the same-file rename S13 D5 removed)

**Build**:
The one operation behind Play and Export: validate the project, route every asset into
the canonical archives (`language.pff`, `localres.pff`, `resource.pff`) and the
mandatory loose files, write through the streamed PFF writer, verify through the VFS,
and publish an immutable `.opennova/build/play/<build-id>/` directory. Incremental by
per-archive input hash.
_Avoid_: pack (a step inside a build), export (a build copied to a chosen directory),
stage (the retired retail-staging vocabulary)

**Operation**:
A long job of the editor's session (a build; opening a project, a refresh, an import's plan and
its write, a rename's rewrite to follow), run one at a time a step at a time, each step within
the frame's budget, so the editor keeps drawing while it runs. Its progress shows while it runs;
nothing it makes reaches the view until it finishes, and a Cancel stops it between two steps, its
work discarded. It declares what it reads and writes (a build reads the project's files), as each
request kind does, and a request that writes what it reads or writes, or reads what it writes,
meets the busy gate, the two rows saying what happens: it is refused (a save while a build packs),
joins the operation (a Build or a Play onto a build), takes its place (a new import plan over a
running one) or cancels it as it commits (a project switch, Quit). A request that conflicts with
nothing it holds (an edit, an open) goes on.
_Avoid_: task, job, background work (nothing runs on another thread)

**Play**:
Build, then launch the game runtime (`opennova.exe -- --resource-dir <build>
--mcp-port <n>`) as the editor's one managed child through a `PlaySession`; Stop ends
it. The editor tails the session log until the runtime MCP answers, then drives it there.
_Avoid_: run (ONED's vocabulary), preview (an in-editor render, not a running game),
"see in game"

## Runtime presentation

**HUD**:
The in-game heads-up display, laid out by `hudpos.def`. RE record:
`docs/interface/hud-re.md`.
_Avoid_: overlay, UI (too broad)

**Dev tools**:
The engine-owned Dear ImGui tool windows behind F3 (`engine/runtime/devtools`,
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
