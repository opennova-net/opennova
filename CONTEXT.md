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
_Avoid_: libs, core, framework

**The Godot layer (first-class, ADR 0034)**:
`godot/src/` (pure C++ GDExtension bindings) plus `godot/game/` (the game-level
GDScript runtime) — the layer that wires Godot nodes to engine facts. The game
shell is composed inside its game runtime.
_Avoid_: godot/engine, engine layer (that word is the engine's), glue, bindings (only
half of it)

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
_Avoid_: field alias, a flat field name

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
_Avoid_: menu host, menu manager, controller

**Tab**:
A Window shown or hidden by a sibling button's `window` Action (e.g. the Options panels). Not a widget type, just an authored convention: one button per panel, each `<ACTION type="window">` hiding the siblings and showing its own.
_Avoid_: page, panel (when you mean the toggling mechanism)

**Stylesheet / Style variable**:
A `.mns` file of `NAME value` lines whose names the menus use as `%NAME%` (a font, a colour, an image). The game reads two, the **shell's stylesheets**: `menu_style.mns`, then `brand.mns` onto the same list, a later definition winning; a `.mns` by any other name is never read. A **style variable** is one name in that list; the one the game reads is the last definition of the last sheet that has it.
_Avoid_: style file, theme, macro (except when quoting the shipped header)

## World & NovaWorld

The vocabulary separating the in-game world from the online service.

**GameWorld**:
The runtime world-sim scene (`godot/game/world/game_world.tscn`): terrain,
environment, mission runtime, and audio under one embeddable root. The standalone
game is the sole live mission runtime (ADR 0025).
_Avoid_: NovaWorld (that name now belongs to the service), world scene

**NovaWorld**:
NovaLogic's online matchmaking and account service, and our reimplementation of it (its **programs**, `apps/novaworld_*`, over `engine/net/novaworld`). Always the service, never the in-game world. It is our NovaWorld server, not an emulator.
_Avoid_: emulator, lobby server

**Program (of the NovaWorld service)**:
One of the service's binaries, each its own image and ECS service: the **gate**, the
**lobby**, **web** (the retail HTTP routes and the website's API), the **fleet program** (the
hosted servers' supervisor, one elected leader), the **worker** (its **jobs** and their
schedules) and **data** (the data sets' jobs over the game data's store). Each scales on its
own and shares nothing with the others but PostgreSQL (and web's Redis); what more than one
uses is the shared library (`apps/novaworld_common`). Each runs only its own job kinds and
schedules only those. There is no all-in-one binary. Beside them, **opennova-hostd** is a
program of the fleet cluster (`opennova-<deployment>-fleet`): an ECS daemon service, one task
per **machine**, rolled by the same releases, before the fleet program.
_Avoid_: role, service (for one program), tier, microservice, pod

**Gate**:
The program that is retail's first contact: UDP port 7597 (the protocol in
`engine/net/novaworld/gate`, the listener in `apps/novaworld_gate`), which hands a client the HTTP
service URL and the one lobby address every **lobby instance** announces, and takes the hosts'
status blobs on the same port. It holds no session; its address is the one retail's hosts file names.
_Avoid_: lobby, edge

**Lobby**:
The program that runs one **lobby instance**, NovaWorld UDP on port 64206, its sessions in
its own memory; as many run as the deployment wants, all behind one lobby address, which
each announces (`ONNET_LOBBY_ENDPOINT`: on ECS the load balancer's at the gate's address,
which spreads the clients across them). Only its own restart drops its sessions, and its
hosts reconnect to the same address and list again.
_Avoid_: edge, lobby server, frontend

**Lobby instance**:
One lobby's NovaWorld UDP lobby (`lobby_instances`: its own id, the endpoint every
instance shares, active or draining, its heartbeat). A client's lobby session lives on the
instance its flow reached through the load balancer for the session's life; listings,
session rows and **lobby commands** carry the instance's id.
_Avoid_: lobby server, shard (for one instance)

**Lobby command**:
A statement for a lobby session that another program sends through the `lobby_commands`
table (a server command or a stop to a listed server, the end of a session, forgetting an
owner); the owning **lobby instance** takes and runs it within 250 ms, rechecking the
owner. Nothing outside the lobby touches the lobby's memory.
_Avoid_: push queue (the old in-memory one), message

**Fleet program**:
The program (`apps/novaworld_fleet`, ECS service `fleet`) that runs the hosted servers'
supervisor pass with its admin links, the console drain and the machine agents' internal
listener (`fleet.<deployment>.opennova.internal:8070`), while it holds the election lock
(`fleet_leader`, a generation fencing a stale leader). It never calls the cloud and runs and
schedules no **job** itself: its pass enqueues them for the worker.
_Avoid_: controller (that word is the in-match **Controller Kind**'s), scheduler, master,
leader (alone)

**Worker**:
The program that runs the **jobs** it registers and their schedules: a pool of threads
claiming them from PostgreSQL (`ONNET_WORKER_THREADS`), and in every worker process a
scheduler of its own periodic kinds (no election: a schedule key is unique, so a bucket gets
one job). It alone holds the fleet's cloud calls (`machine.*`, `fleet.sweep`), the billing
sync and the mail.
_Avoid_: consumer, executor

**Data program**:
The program (`apps/novaworld_data`) that runs the data sets' jobs (a tree's finalize, a
release's validate and apply, the trees' prune) over the game data's store, and schedules
their prune; one task, its own disk for the files a job passes through. An admin's upload
never queues a machine launch behind it.
_Avoid_: data worker, uploader

**Job**:
One piece of background work in the `jobs` table (a machine launch, describe or terminate,
a billing sync, a mail, a prune, a data set's tree), enqueued in the same transaction as the
change that causes it, claimed with `SKIP LOCKED` under a lease by the one program that runs
its kind, retried with backoff, idempotent.
_Avoid_: task, message, event

**Realm**:
One NovaWorld: one deployment of the service with its own gate address and database.
Retail picks a NovaWorld by the address its hosts file gives `gs.novaworld.net`, so a realm
is never a column inside another realm's tables.
_Avoid_: shard, tenant (for a realm inside one database), region

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
engine contrasted with retail is **the reimpl**, never "the host". A review
concern, not a lint.
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
(`--mcp-port`, 8977 in `.mcp.json`; `scripts/mcp/editor_mcp.py`), through which an agent
drives the editor's session by its two seams, requests and queries, and Build, Play and a
document's viewport (ADR 0046, `docs/mcp.md`). Its tools are made from the `catalog` query
when it starts; the game a Play starts is driven through its own runtime MCP.
_Avoid_: runtime MCP (the game's), game MCP (when the editor is meant)

**In-match / Matchmaking**:
The two network protocol domains. **In-match** is the 62 Hz game session between a host
and its clients (the wire codec + replication runtime). **Matchmaking** is everything that
gets players INTO a match: gate, browser, accounts, NAT rendezvous — the NovaWorld
service's domain. Code and libs are named by their domain, never bare "net".
_Avoid_: unqualified "net code", lobby (for either)

**Wire codec / In-match session / Match**:
The two stable in-match boundaries and the gameplay model (ADR 0043 d3/d4). The **wire
codec** (`engine/net/npwire`, ADR 0019) is the retail compatibility contract and
encodes/decodes the byte stream; its message catalog is the single source of
truth (ADR 0013). The **in-match session**
(`opennova::inmatch::Session`, `engine/runtime/inmatch`) owns lifecycle, role,
fixed cadence, retained input, and tick outcomes. The authoritative **Match**
(`world::Match`) owns rules, player/team statistics, clock, winner evaluation,
and the frozen end-round result. `runtime/inmatch` and `runtime/replication` (their
tests keep the old names, `tests/npruntime` and `tests/netsim`) are implementation
directories beneath those boundaries, not peer layers or extension seams.
_Avoid_: "the netcode" (name the wire transaction, session behavior, or match rule);
"net seam" / "net runtime layer" as public architecture

**Retail tick bank**:
The one fixed-tick accumulator (`world::TickAccumulator`), a port of `Game_MainLoop`'s
frame-time bank: each frame banks its elapsed time, smoothed, and the bank drains in fixed
quanta with a logic tick on every fourth, so a hitch is repaid over the next frames rather
than in one burst. The in-match session banks through it; the same bank feeds the FR counter.
_Avoid_: frame pacing, catch-up loop, a second tick clock

**Packet / Draw list**:
A **packet** is wire data — bytes on the network, and nothing else. What a frame
compiler hands a renderer/applier is a **draw list** (`*DrawList` types, "draw-list
order/bounds/diagnostics" in prose); new render-side "packet" vocabulary is a regression.
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
The third session role (D-NET-217): a human admitted into a live match who controls no
Person. The authority still allocates a roster slot and a hidden, damage-disabled body while
the client flies a free camera; the canonical bit is `replication::Connection::spectator`.
Record: `docs/net/novaworld-net-re.md` §5.0e.
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

**Character attributes**:
`charattr.def`'s table (`formats/charattr`, `inmatch/charattr_table`): a row per Soldier
Class, its ATTRIBUTES words and its three camouflage items, with per-property latches a
session's mp_No* words raise.
_Avoid_: anim slot, AnimMap slot (the IDB's former misnomer), input state flags (S2C 0x42 carries the latches)

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
The line-based text form of one `.3di` model (`docs/threedi/o3d-scene-format.md`), carrying
no native limit (ADR 0052): `opennova-3di build` mints a `.3di` from it, and `opennova-3di
scene` writes any `.3di`, retail ones included, back out as it.
_Avoid_: scene file (a Godot `.tscn` is a scene), ASE/OED (the retired authoring
formats)

**Lowering** (of a `.o3d` model to a **target**):
The step that holds a scene text model to what a target game holds
(`engine/formats/threedi/threedi_o3d_lower.h`): it splits meshes into the strips a 3DI3
stores and checks every name, count and extent against the target's limits (the retail
target is the only one), refusing what the target cannot hold.
_Avoid_: validation (the reader validates the text; the lowering is the target's),
export (the add-on exports; the CLI lowers)

**`.o3a` clip-set text**:
The text form of one rig's clip set: its `.adm` table and every `.bad` clip the table
names (`docs/anim/o3a-scene-format.md`). `opennova-3di anim build` mints the set from
it; `opennova-3di anim scene` writes a set, retail ones included, back out as it.
_Avoid_: animation file, clip file (a `.bad` is one clip)

**`opennova-3di`**:
The command-line tool (`apps/3di`) over the engine's one `.3di` reader and writer: the
`.o3d` and `.o3a` texts' `build` and `scene`, `info`, `compare` and `catalog`, and `weapon
timing` (`docs/anim/weapon-timing-format.md`). The Blender add-on bundles it.
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
services and development tools are outside this taxonomy (ADR 0015); so are
`opennova-serve` (ADR 0051) and the OpenNova Switcher (ADR 0054).
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
authored from scratch: OpenNova's own small game, the "Indigo Shore" mission
(single player and Co-op over LAN) and its "Indigo Storm" expansion, whose main
menu also offers **PLAY RETAIL**. The game mounts it when no `--resource-dir` is
given (ADR 0048).
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

**OpenNova Switcher**:
The Windows tray app (`switcher/`, `opennova-switcher.exe`, ADR 0054) that points a stock
retail install's NovaWorld at OpenNova's service, NovaLogic's, or an address the user
enters, by managing one block in the Windows hosts file for `gs.novaworld.net`. It does
nothing else: it starts no game and installs nothing. Not a Godot product.
_Avoid_: launcher (it launches nothing), host file for the Windows hosts file it edits
(the host file is retail's dedicated-server config, below)

**Host file**:
Retail's dedicated-server config, the file `/HOST <file>` names: `<Key> <value>` lines of
`ServerConfig_ApplyHostSetting`'s keys (a text form of the host screen) and
`Mission <file> <launch option>` lines that seed the map rotation. Retail never reaches
its reader; `opennova-serve` reads it (`engine/runtime/inmatch/host_file.h`).
_Avoid_: server config, server.cfg, dedicated.cfg

**Hosted server**:
An `opennova-serve` that an account starts from the NovaWorld site (ADR 0053), as many live at once
as its **hosting entitlement** allows, each in a **slot** of a **machine**, over the **data set** its
owner picks. It lists on our NovaWorld under a service account of its own, its owner administers it
from the site through the service only, and it stays up until stopped; a move to another machine
costs it no restart.
_Avoid_ (for this product only; the words keep their meanings elsewhere): hosted host,
game host, instance, dedicated server

**Support tier**:
The monthly Stripe subscription that supports the project and sets an account's
**hosting entitlement**: tier 1, 2 or 3, one, two or three **hosted servers**
(`apps/novaworld_common/billing/`). Its prices come from the service's configuration,
never from code. The own-machine add-on is a subscription of its own beside it.
_Avoid_: plan, package, premium, donation, membership

**Hosting entitlement**:
How many **hosted servers** an account may have live at once: the larger of its **support tier**'s
servers and an admin's comp grant of servers, plus how many may run on an own machine (the add-on's
quantity while a tier grants, and an admin's comp grant of own machines while the account has
servers), and its **static addresses**. A past-due tier keeps the servers but starts no new one.
Read from the service's cache of Stripe, whether or not billing is configured
(`billing/entitlement.h`). What every account's entitlement holds is what the service has sold,
and no new subscription is sold past what the fleet holds (`hosted/model/sold_capacity.h`).
_Avoid_: quota, allowance (in code; the site's meter may say "N of M servers used"), approval
(the retired admin flag)

**Machine**:
A virtual machine **hosted servers** run on, each in a **slot** of its own (an EC2 instance on
the deployment's pinned ECS-optimized image, a container instance of the fleet cluster, ADR
0053; in the local stack, a container of the hostd image): a **shared machine** or an **own
machine**. The service launches it, places servers on it and ends it; **opennova-hostd** runs its
containers (docker's, never ECS tasks). Never "host": that word is the authoritative session side alone.
_Avoid_: host, server (for the VM), box, node, pod; "instance" alone (say "EC2 instance"
when naming the AWS resource)

**Shared machine**:
A **machine** that packs up to K **hosted servers** of any account (`ONNET_FLEET_SHARED_SLOTS`,
12 to start), one per **slot**; servers go to the fullest one with a free slot, and one
with no server left ends after a while.
_Avoid_: pool, pod, cluster node

**Own machine**:
A **machine** that runs exactly one **hosted server**, its owner's, with the own-machine
add-on (a paid monthly subscription beside the **support tier**): launched for that server,
reserved for it (one slot, slot 0), and ended when it stops.
_Avoid_: dedicated machine, dedicated server (retail's Serve Only term), private host

**Static address**:
An **own machine**'s fixed public IPv4 with a name in the deployment's play zone
(`<name>.play.opennova.net`), which its owner keeps for the own-machine servers it starts, so a
server lists at the same address and port on every start (ADR 0066,
`apps/novaworld_common/hosted/model/address_repository.h`). Each is a monthly add-on of its own (or an
admin's comp address), at most one per own machine; on EC2 it is an Elastic IP.
_Avoid_: Elastic IP (but naming the AWS resource), dedicated IP, fixed IP, vanity address

**Slot**:
One **hosted server**'s position on a **machine** (`hosted/protocol/slots.h`): its index fixes the
server's game and remote-admin ports, its container user and its working directory
(`apps/novaworld_common/hosted/README.md`). Two live servers never hold one slot.
_Avoid_: seat, position (for the place on the machine), player slot (that is a match's)

**opennova-hostd**:
The machine agent (`apps/hostd`, target `opennova_hostd`, image `opennova-hostd`): one per
**machine**, the fleet cluster's ECS daemon on a cloud machine, it fences the slots' users,
redeems the machine's one-time bootstrap token, then syncs with the **fleet program**'s internal
listener (by its name, `ONNET_INTERNAL_URL`) every few seconds, reporting each container (its state, CPU, memory, its slot's
egress) and running the containers the reply wants (a local stack's machine runs them as
its own processes, `--runtime process`). It decides nothing itself.
_Avoid_: daemon, node agent, kubelet

**Title**:
A NovaLogic game identity (JO, DFX2, DFX, BHD...): near-identical engines over different data and
configuration. Titles are in scope as data (ADR 0055): a title is a `gameprofile` game id (`jo`,
`dfx2`) whose wire and NovaWorld identity is one `TitleIdentity` row in `engine/net/title`. No code
outside that table hardcodes a title's identity, and an unwitnessed field is refused where it is
needed, never filled with JO's value.
_Avoid_: game (when you mean the identity, not the running program)

**Data set**:
One product a **hosted server** runs on (ADR 0064): a base game (Joint Operations, Delta
Force Xtreme 2, OpenNova's own data), which speaks a title and lists in a lobby, or one
expansion of a base (Escalation, `jox01`), holding only its own files
(`data_sets/data_set_files.h`). An admin creates one as a row on the Game data page and uploads
its files; the site offers only data sets with a ready **tree**.
_Avoid_: game (for the data), install (a retail install is where a data set's files come
from), snapshot (gone: the data lives in the object store), slug (a data set has an id and a
display name; its lobby has the slug)

**Tree**:
One immutable version of a **data set**'s files in the object store (`trees/<id>/` with a
manifest of every file's name and size, and its missions): a baseline an admin uploaded, or a
published **release** applied to that baseline. A hosted server is pinned to the trees it
started on until its owner asks for the update; a **machine** fetches the trees its servers
need and checks them against the manifest.
_Avoid_: version (a tree has a version line; the tree is the thing), snapshot, volume

**Release**:
An uploaded `1SFS` packed update (ADR 0061; ADR 0064 decision 5): one package, one section
per product it carries, each mapped to a **data set** by its command file. Validated, then a
draft; published, it is what the update listener serves that data set's updater login, and
the worker applies it once to the data set's baseline **tree**, making a new current one;
retired, or replaced by the next publish or a new baseline upload.
_Avoid_: update (the mechanism), patch, package (the file; a release is the row)

**Required resources**:
The resource set the engine hard-requires by name at boot (menu set, game strings, music
banks, HUD layout, defs, default world files...). The witnessed enumeration is
`docs/required-resources.md`; the engine manifest derived from it drives
boot validation and defines the minimum game-data tree.
_Avoid_: core assets, base game files

**Promote**:
Reserved for `mission::promote_mission`: spawning a parsed mission into the live world
(entities, AI brains, nav), the IDA-cited spawn path the game uses. For the word's other
senses say *migrate*, *normalize*, *whitelist* or *move*.
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
its own in `.opennova/local.json` (`game_install`). Its files are copied into a project on
request (the files chosen, their **import closure**, or the whole install), never
redistributed, and never a player's save, configuration or scores. **Strict Play** runs a
build there.
_Avoid_: retail directory, retail root, retail files, resource dir (the runtime's
`--resource-dir`, which may be a build)

**Install check**:
What a folder holds as a game install, read as an import mounts one (the boot archives, no `/d`):
whether they mount, how many files they serve, the expansions, whether the game's program is beside
them; said in a line under every field that names an install (New project, Project settings, the
welcome page), so a wrong folder is said at once (`assets/install_check.h`, the `check_install`
request).
_Avoid_: install validation (nothing is validated: the folder is read)

**Welcome page**:
The editor with no project open: the Document window across the whole workspace, with the
recent projects (each by its title, its game and its expansion), Open, and the New project
form with its game install.
_Avoid_: start screen, home, launcher

**File card**:
Files' page for one project file: what a file of its kind is to the game, where a build puts it, where
it came from, what it names and who names it, a wave's sound with Play; what a double click opens for a
file the editor has no document of (`session/file_card.h`, the `file_card` query).
_Avoid_: properties, details, info panel, References (the card's two lists replaced it)

**Project expansion**:
What a project builds as and on (ADR 0046 S16, `project.opennova`'s `expansion`): its **expansion
name**, the folder `expansion\<name>\` its build is, played with `/exp <name>`, and the installed
expansion it **builds on** (none: the base game). A project with no expansion name is standalone.
The files the game reads by an expansion's name (`<name>.bin`, `version.txt`, `M<name>.*`, ...) are
its **expansion files**.
_Avoid_: mod (a project builds as an expansion; "mod" is the Mods list's word for one), base (for
the expansion a project builds on: the base game is the install with none)

**Logical name**:
The flat, case-insensitive name the engine resolves an asset by (`main.mnu`,
`items.def`), at most 16 bytes as a PFF entry. A project asset's identity; its path in
the tree is organization only. Uniqueness and length are checked on output names.
_Avoid_: path (when the engine-facing identity is meant), resource name

**Font set**:
A bitmap font made from a picture by the editor's font importer: `<name>.fntset`, a short text
naming its **glyph sheet**, a PNG of 16 x 14 equal cells holding the bytes 0x20..0xFF, each
glyph its cell's alpha. The import record's options are the font's metrics and colour; the
import makes `<name>.fnt` through the FNT writer.
_Avoid_: font project, atlas (the `.fnt`'s pages are the packer's layout, not the sheet's)

**Project layout / placement / move**:
Where a project keeps its files: **flat** (most at the top level, as the base game does) or **by
kind** (each in its kind's folder, `AssetKindRow::folder`). A file the editor makes (Create
missing, New file, an import) is **placed** beside the project's files of its kind, else as the
layout says (`assets/project_layout.h`, DI-03), so a flat project stays flat. A **move** puts a
file in another folder under its own name and rewrites no reference, since the game finds it by
its logical name.
_Avoid_: default folder (a kind's folder holds new files only in a project laid out by kind)

**File chore / file history / trash**:
What Files does to a file or a folder beside editing it (DI-25): a **delete**, a **duplicate**, a new file
**here** (in a folder the modder picks), a folder made, renamed or deleted. Each is one step of the
project's **file history**, which Edit > Undo file and Redo file walk (a document's Undo is its records'; a
rename's way back is Rename back). A delete moves the file to the project's **trash**
(`.opennova/trash/<n>/`, never emptied by the editor), so no delete is a permanent one, and asks who names the
file first.
_Avoid_: recycle bin (the OS's; the trash is the project's own), remove (a delete keeps the file in the trash)

**Import / sidecar**:
Bringing a non-native source (an image, a terrain set, a font) into the project the Godot way:
a committed `<file>.import` sidecar records the importer, its version, options, output logical
names, the source's content hash and its **inputs** (every other file the importer read through
its import context), and the outputs are regenerated into `.opennova/imported/` and packed like
native assets. It runs again when the source or an input changes; the machine-local **import
cache** keeps hashes so an untouched file is not read. A file is an **import source** only while
its record is there, and an **import input** while a record lists it; the build packs neither,
only the outputs. The files the import dialog offers are **import choices**.
_Avoid_: convert (the runtime never converts), asset pipeline (the retired Python route),
image source

**Terrain set**:
A terrain made from ordinary images (ADR 0046 S20): `<name>.tset`, a short text naming its
heightmap, colour map and, if wanted, a detail, a tile set and a surface map, each an import
input, its import options the terrain's numbers in world units. The terrain importer bakes it
with TrnGen.exe's own bake (`engine/formats/cpt/trngen`) into the files the game reads for a
terrain, each named after the set, among them the **surface map** (`<name>_m.pcx`, the `.trn`'s
char map of surface classes) and the **foliage map** (`<name>_f.pcx`, foliage codes).
_Avoid_: terrain project (TrnGen's `.tpj`), heightfield document

**Ground overlay**:
What the mission view tints its terrain with (ADR 0046 DI-29, Show > Over the terrain): a picture laid
over the terrain from above, each texel what the game's own sampler reads at its middle, the surface
class (in the char map legend's colours) or the foliage definitions the foliage map grows there, with
its legend over the picture's corner.
_Avoid_: overlay alone (one shape a canvas draws over the picture), debug mode (the terrain shader's
diagnostic colourings)

**Terrain brush**:
The tool a mission's 3D view, its map and a terrain's own view edit the terrain with (ADR 0046
S25): one per session, its tool (Sculpt, Paint, Tiles and their modes), its radius, strength,
falloff and spacing, and each tool's value. A drag is one stroke (one gesture) into the file the
game reads for its plane (the `.cpt`, the surface or foliage map, a texture, the mission's
`.til`), which opens beside the view and is undone and saved with it.
_Avoid_: sculpt mode, paint tool, terrain editor

**Terrain pane**:
The pane beside a mission's or a terrain's picture that shows the terrain brush (ADR 0046 S25; the toolbar's
Terrain, Ctrl+E): where the edits go, the terrain files left unsaved, the bake line, the brush's numbers and a tab
per tool family (Sculpt, Paint, Tiles) holding its values (the surface legend, the foliage codes, the tile palette),
the tabs staying in sight over a body that scrolls. It takes the Logic pane's place: one shows at a time.
_Avoid_: terrain window, tool palette

**Heights document**:
A terrain's heights as the editor holds them (ADR 0046 S25): the `.cpt`'s depth atlas, a raster document the
brush's strokes edit, the regions a stroke changed baked again into the `.cpt`'s polygons as TrnGen's bake makes
them (the bake line says what waits). It opens beside the view a stroke came from, quietly: the view stays the
active document, its Undo takes the stroke back and its Save saves it.
_Avoid_: heightmap (a terrain set's PNG, an import input), heightfield

**Height lock**:
An entity's flag the original mission editor keeps (`height_lock` in its `.mis`; ADR 0046 S25, D-MIS-4): locked,
the entity keeps its height when the terrain under it changes; unlocked (every `.bms` entity and every new one), it
keeps its height above the ground and follows a sculpt. The `.bms` has no room for it: the editor keeps it in the
`.mis` beside the mission, and a save over a terrain that does not read writes every entity locked there, listed
(D-MIS-17).
_Avoid_: pin, anchor, absolute height

**Import closure**:
What an import "with the files these need" brings beside the files chosen (ADR 0046 S14): every
file they reference, and those files' in turn; the file that defines each name they use where the
project lacks one; and for a mission its **mission sidecars** and every file the game opens by a
fixed name. It is planned first (the **import plan**), then written with every file staged before
any is published. The player's own files are never taken. A project is its own files, so a
shipped mission's closure is most of the game.
_Avoid_: dependency mount, base, parent project (a project builds on nothing but what it holds),
bundle

**Import option**:
One setting of how an import makes its outputs, held in its import record by a key (ADR 0046
S18), one of its importer's rows (the image importer's format, size, alpha, palette, DDS
compression, mip levels, ...). What a texture's uses ask of its import is said beside them,
with why, and set in one click.
_Avoid_: import setting, sidecar field, conversion (an option is how the outputs are made, never a
change to the source)

**Mission sidecar**:
A file the game finds by a mission's name rather than by a reference in it, each skipped when
absent: `<mission>.bin` (its text), `.wac` (its script), `.pcx` (its loading image), `.til` (its
tiles), `.dbf` (its dialog bank) and that bank's sounds. One witnessed table (`mission::sidecars`),
which the game's loaders and the editor's mission document share.
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

**Project notes**:
What a project keeps for its people and the game never reads: a `.md`, a `.txt` of any name but those
the game reads (earlyerr.txt, an expansion's version.txt, ...), a text with no extension (a LICENSE).
Its own kind, opened as a text, left out of the build without a word; a file of no kind the game knows
is said instead, as it may be data.
_Avoid_: unknown file (said, and maybe data), docs (the repo's golden docs)

**Display name**:
What a value a document holds reads as to a modder where it stands for something with a name (ADR
0046 S15): an item id by its catalog's name, an SSN by its entity ("Ranger #12"), an event by its
sentence; one naming nothing says so (it is **dangling**). The raw value stays beside it,
secondary. One service makes it for every window and the wire (`engine/editor/graph/display_names.h`).
A record's display name is its **title**; its **name** stays what the graph keys it by; its
**brief** words tell it apart where a column cannot hold the title.
_Avoid_: label (a field's own name), caption, alias

**Game's own data**:
What the game install ships, as the game is served it (ADR 0046 S15). A finding is the game's own
when the install, as it ships, makes the same finding in the file of the same name; whatever the
modder's edits brought is the modder's. Problems shows them apart and counts the modder's first; a
finding that blocks the build is never the game's own.
_Avoid_: stock files, vanilla, unmodified (as Problems words)

**Event sentence**:
A mission's event in words, as the game runs it (ADR 0046 S15): "When <trigger>, and/or <trigger>
..., then <action>; <action>.", with its flags and times, each parameter by its display name
(`engine/editor/documents/mission_sentence.h`). An event's title. A trigger or action type's name
in the pickers is its **title**, under its **group**.
_Avoid_: description, summary, event label

**Base layer**:
What an expansion project's base game (the install's, or a base project's export) gives the
project's asset graph: its files and the names they define, read once and never edited. A lookup
tries the project first, then the base layer; the base layer makes no reference and no finding of
its own. A standalone project has none.
_Avoid_: second graph (one graph, the base under it), import (an import copies files in; the
base layer only answers names)

**Requirement**:
One row of the editor's checklist: a file the engine demands by name (a Required resources manifest
row with its witnessed severity, keyed by a stable role token) or a project feature's own need,
Present, Missing or Wrong kind. One the project lacks is a Problems row whose fixes create it,
import it or assign another file to it. The engine's names stay fixed; the checklist enforces them.
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

**Dialog bank**:
A mission's `.dbf`: its dialogs, each a list of lines a Play dialog action plays by the number its
name forms (`dlg012` is dialog 12), each line a wave of the bank's sounds (the `.lwf` of its name)
with its subtitle from the mission text. It opens in the editor as a document, and Play plays a
dialog's lines as the game does.
_Avoid_: voice bank (the voice sets the go codes play are sets of the global banks), dialog file, DLG
(the dialogs' sets the shipped sound banks carry, which no dialog lookup reads)

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
changing it: a name, one row of the query table (`session/editor_queries`), and its params. Every
list a query serves comes a page at a time; the `catalog` query lists every request kind, query,
section and concern from the tables themselves.
_Avoid_: request (a request changes the session), view (what the windows draw from), getter

**Verb**:
What `opennova-project` is run to do (`new`, `status`, `validate`, `create-missing`, `import`,
`reimport`, `new-terrain`, `build`, `export`, `request`, `query`): a row of the command line's verb
table (`apps/project/cli_verbs`) naming the requests it sends the editor's session, run headless,
and the query whose answer it prints. The command line orchestrates nothing of its own.
_Avoid_: command (a request, a menu's COMMAND, a script's command), subcommand

**Workspace**:
The editor's windows (Files, Document, Preview, Inspector, Problems and Output, the menu bar and
the modals) and their seam to the session (`ui/workspace.h`): the view every window reads, the
requests it raises, and the devices its viewports draw through. What the windows show of their own
(a dialog open, a form's fields, a filter) is the session's too, the view's workspace, so the
editor MCP sets and reads it as a person's controls do.
_Avoid_: host, editor host (host is the game host alone), UI (too broad), project (the data)

**Document**:
A file open in the editor: read as the game's loader reads it, changed through its own undo
history, and written back only while the file still holds what it was read from. A record
document holds rows of records, each edit naming a record and a field; a text document holds a
text whose spans its edits replace; a raster holds its own content.
_Avoid_: file (what is on disk: an open document stands in for it until it is saved), asset (a
project file by its logical name)

**Document view**:
What a document's tab in the Document window shows of it: its type's row of the editor's view
table, one view per open document. Most types show their records as an outline; a type may have a
view of its own, or a Main-role viewport filling the tab (a text's script device, a mission's 3D
view). The selected record's fields are the Inspector's, whatever the type.
_Avoid_: editor (the application), panel, preview (the Preview window's picture), inspector (the
generic form beside it)

**Viewport**:
One document's picture as the game would draw it, of one kind (a menu's screen, a model, a
mission's 3D view, a texture, an effect, a HUD, a definition's record, a text's script device),
kept by the session while the document is open, one per document and kind. Its role is Preview
(shown by the Preview window) or Main (the Document tab's view). Its state (size, options,
camera) changes only by a SetViewport request, or by its follow of the selection; it follows its
document's change set and the files its picture read. Its device is the Shell's (an offscreen
Godot viewport, or a control over the tab), drawn only by its canvas, and may build a picture
over several frames while the canvas shows the last whole one. Headless, it answers as its
envelope (the `viewport` query), and a client drags and commands through it
(`edit_in_viewport`).
_Avoid_: pane (the Preview window's two, which it replaced), device (the Shell's renderer behind
it), preview (the Preview window, or the role)

**Mark**:
A record of a mission as its viewport's overlays draw it and its canvas picks it: an entity's glyph
by its pool, ringed in its team's colour, or an area's footprint, at its anchor. A click picks the
front-most mark near its anchor, a marquee every mark inside it, and the primary selected mark
carries the handles a drag edits it by.
_Avoid_: gizmo (the handles alone), icon, marker (an entity pool of a mission), pin

**Lifted**:
An entity a mission viewport's device draws apart from the placement it last made (one added or
given another item, group or attributes since), built as the placement builds an entity's model,
until the next placement takes it in. None of an add, a removal, a move or their undo places the
mission again.
_Avoid_: spawned (what the game does at run time), dynamic, unplaced

**Palette**:
The mission viewport's list of what its Place tool places: every item the project's catalogs
define, by name, grouped by what items.def says it is, each with the pool a placed one lands in,
searchable, the recently placed first. One is picked, then each click places one; or one is
dragged onto the picture.
_Avoid_: item list, catalog (the items.def file), library, browser (NovaWorld's)

**Script device**:
A text document's Main view: a Godot code editor placed over its Document tab, showing the
document's text as it stands (the text is the document's). What is typed goes to the document as
spans replaced, each run of typing one undo step, Undo and Redo the editor's; findings are marks in
its gutter; a script's WAC words are coloured, completed, explained on hover and Ctrl+clicked to
their definition (ADR 0046 S15, `session/script_assist`). Where no device draws it, the lines show
read only.
_Avoid_: script editor (the whole editor), code view, text view (the lines shown read only where no
device draws), CodeEdit (the Godot control behind it)

**Environment document**:
A mission's environment (a .env: its sky, light, fog and water) open in the editor, read and
written through the engine's own reader and writer: one record whose fields are the keywords the
game reads in the units the file writes, its time-of-day keyframes (at most 16), and its terrain
keys, each over its .trn's. Its textures and models are each a Go to; the Inspector heads it with
the missions that run on it.
_Avoid_: env file (the file alone), weather (the runtime's state), sky (one part of it)

**Environment viewport**:
An environment document's Main view: its sky as the game draws it over the terrain of a mission
that runs on it, through the runtime's own environment, weather and sky nodes, on the game's
mission clock (or an hour a scrub picks), with the weather a script sets. Every edit shows as it
is made.
_Avoid_: sky preview (it is more than the sky), time-of-day player

**Terrain document**:
A terrain (a .trn: the settings a mission's ground is loaded by) open in the editor, read and
written through the engine's own reader and writer: one record whose fields are the keys the game
reads, its grid rows (at most 16) and its foliage definitions (at most four). Each map and model is
a Go to. A terrain an import makes from a terrain set opens read only: Reimport makes it again.
_Avoid_: terrain set (the import's source of images), trn file (the file alone), heightmap (one input)

**Terrain viewport**:
A terrain document's Main view: the terrain drawn on its own through the runtime's own terrain,
water, foliage and sky, under a mission's environment or the engine's own. It can tint what the
game reads at each point over the terrain, and hovering reads the ground under the pointer.
_Avoid_: terrain preview, heightmap view (it draws what the game draws)

**Texture document**:
A texture file of the project (a .tga, .mdt, .pcx, .dds or .png) open in the editor, read as the
game reads it, by the reader its name picks, never by what the bytes would say elsewhere: its
texels and what the texture is in a modder's words (format, size, alpha, mip levels, palette,
whether the game loads it and why not). Its edits are whole-image operations, each one undo step
written through the editor's writers; no paint program. A file an import makes is made by its
import's options, never edited in place (ADR 0046 S18).
_Avoid_: image (the decoded texels alone), bitmap, sprite, asset (a project file by its logical name)

**Texture role**:
One way the game uses a texture file (ADR 0046 S18; 46 in all): a model's diffuse or normal map, a
terrain's colour map, a sky's cloud layer, a HUD's art, a menu's image, ... Each names its loader
and reader, the formats that work, the size needed, what the alpha means and what a wrong or
missing file does, each with its witness. The role, not the file, decides what is right.
_Avoid_: texture type (a model texture row's type field), usage, slot (a model row's slot field)

**Texture use**:
One way a texture file is used (ADR 0046 S18): a reference to it from another file, with the role
that reference gives it, or a name the game opens itself. A use whose loader opens another file of
the name (a `.tga` beside the `.dds`) does not read this one. A texture's tab lists them as Used as.
_Avoid_: usage (the graph's references to any file), referrer (the file that makes the use)

**Texture thumbnail**:
A texture file as a small picture (ADR 0046 S18): read as its use's loader reads it, shrunk to 128
pixels a side, with its size and format in words, shown wherever a field names a texture.
_Avoid_: icon, preview (the Preview window), image

**Texture viewport**:
A texture's picture (a texture document's Main view, and the Preview window's for a texture Files
selects): its texels at a zoom, through its colour, a channel or its alpha, at a mip level, as the
file holds it or as the game draws it for one of its uses, or beside the DXT texture its `.dds`
would hold (a **compare**); each a SetViewport. A point of it names the texel under it.
_Avoid_: image viewer, preview (the Preview window, or the role)

**Texture budget**:
What a model texture costs the game (ADR 0046 S18): the device texture its loader makes of the file
at each of the four **object texture detail** levels (game.cfg's `object_texdetail`), and what its
`.dds` would cost instead; past 16 MB it is a warning. The project's budget is every model texture,
costliest first (the `texture_budget` query, Files' By cost).
_Avoid_: footprint, VRAM (the game keeps every level in its own process too), file size (what the disk holds)

**HUD viewport**:
A HUD layout's picture (hudpos.def's Preview): the game's own HUD drawn over the layout as Save
would write it now, at a screen size, for a player state its options choose. A point of it names the
HUD element under it and the hudpos.def lines and textures behind it, each a Go to.
_Avoid_: HUD editor (nothing it does edits the layout), preview (the Preview window, or the role)

**Preview clock**:
The one clock every viewport reads (a model's animations, a clip's game ticks, a menu's frame clock,
a particle effect's ticks). It runs while it plays, at its rate, as the Shell's frames pass; a
SetViewport plays, pauses, sets its rate or seeks it, and a viewport seeks it as it follows (a clip
newly chosen starts at tick 0).
_Avoid_: clip clock (the model preview's own, which it replaced), game clock (a running match's),
tick (the game's 62 Hz step, which it counts)

**View event**:
A one-shot ask a request makes of one of the editor's windows (show a record's field, show a file
in Files, ask a new name, take a new import plan), which the session's view keeps in its sequence
(the last 64) until the window it is for has taken it once. The editor MCP pages them by their place.
_Avoid_: serial (the per-ask counters the events replaced), reveal state (the view keeps none),
notification (the OS's), signal (Godot's)

**Navigation history / place**:
Where the person has been in the editor, which Back and Forward take them to again
(`session/navigation_history.h`). A place is a pane and a file: a document's tab with its record, a
text's line, a file's page, or Files on a file. A step is a move the request table marks as one
(`navigates`: a document switched to, a Go to, a Problems row, a find's hit); a selection, an edit
or a camera move is none.
_Avoid_: undo (a document's own history of edits), breadcrumb (the outline's path to a record),
selection history (a selection within what shows is no step)

**Finder / jump subject**:
The project's finder is one modal over four scopes (ADR 0046 DI-18, `ui/project_find`): Find in
project (Ctrl+Shift+F), Go to file (Ctrl+P), Go to name (Ctrl+T) and Find usages (Shift+F12). The
jump subject is what Go to definition (F12) and Find usages act on: what the window under the
pointer offers, else the selection. Every jump is a Go to, a step of the navigation history.
_Avoid_: quick open, command palette, peek (none of them is the editor's), go to symbol (a name the files
define is a name, not a symbol, in the windows' words)

**Record / owner**:
A row of a document or anything nested in one, at any depth; the record that holds a
record is its owner (a menu window's owner is its parent window, a root window's is
the screen). A record's **locator** is its place (the row's index, then each
collection's kind token and index, `0/window:0/window:2`), which a reload of the same
file finds it again by; its path is every name from the row down
(`STARTUP/MAIN/EXIT`), which is what findings and graph edges show.
_Avoid_: node (the core's type for a row), child (an identity field, not a relation)

**Record reference / record set**:
A field naming a record of its own file by its index among the file's records of one kind (a
model's generator naming a CTRL register, a mission's stop naming a marker); those records are the
file's record set of that kind. An edit that adds, removes or moves one changes, in the same undo
step, every index that named a record it moved, and is refused while a field the game reads still
names a record it removes. An index past the set is the file's own finding, never a missing
reference.
_Avoid_: id (an id stays with its record; an index is its place), link, pointer

**Table shape**:
How a type of records tells the editor's core what its records are, in one form for every format:
a table with a row per kind of record, each its labelled fields and the lists it holds. The core
answers the outline, the Inspector, every read and write and every add, copy, removal and move from
the table; the type keeps its parse, its writer and the rules no row can say. A new kind of record,
field or catalog family is rows of tables, not code of a document.
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
The menu's viewport in the Preview window: the previewed screen through the runtime's own menu
frame, reading the project's files as the game reads its mounted files (the open documents standing
in), or a status saying why it cannot. Its primary window carries eight handles: a drag moves or
resizes the selected windows in one gesture, writing the fewest POSITION edges that land them there.
_Avoid_: play (a running game), render check (the headless validator's notes)

**Try mode**:
The menu preview behaving as the game's menu (ADR 0046 DI-35), against Edit, where a click selects the
window under it: its windows' clicks and the keys run the menu's runtime (its Actions, the game's
Commands by name, the screen history across menus), from the screen the preview shows. What it
changes is a **sandbox** (in memory until a Reset; never the player's settings nor a project file),
and what would leave the menu (a mission started, a quit, a game joined) is an **outcome**, said
and not done.
_Avoid_: play mode (Play runs the game), simulate, run (the editor's Play)

**Model preview**:
The model's viewport in the Preview window: the previewed model (or a clip or table played on its
rig's model at the preview clock's ticks) through the runtime's own object renderer, at the level
the game would pick or one held, with markers where the game puts the model's user points, lights
and pivots, which a click selects and a drag moves.
_Avoid_: viewer (it edits), avatar preview (the game's player-info portrait)

**Effect preview**:
A particle file's viewport in the Preview window (ADR 0046 DI-14): the named effect played by the
engine's own effect scene as the game spawns one alone, over the catalog the game would load were
the project saved now, so it shows what the game spawns for the name, looping on the preview clock.
An edit of any particle file shows at once.
_Avoid_: particle editor (ParticleEdit's: nothing here edits but the text), effect viewer, emitter
preview (one effect spawns several emitters)

**Definition preview**:
A definition table's viewport in the Preview window (ADR 0046 DI-21): the selected record drawn as
the game draws what it defines, as it would read it were the project saved now: an item in a chosen
state (alive, destroying, its husk) with its death's effects and sounds as the game runs them, a
person posed as it spawns, a weapon's model, an ammo's round.
_Avoid_: item viewer, def preview (a powerup row or a carry limit draws nothing), model preview (the
model's own document's picture)

**Weapon range**:
Where a weapon or ammo record fires in the Definition preview (ADR 0046 DI-22, DI-23): a world of
the editor's own holding the game's local player before a target wall, run tick by tick through the
game's own legs from the gestures (Fire, Reload, Scope, ...) the editor stamps on the preview
clock, so the clock stepped back runs it again.
_Avoid_: shooting range (the game has none), firing test, gun sim

**Clip fire**:
What a clip's fire events fire in the model preview (ADR 0046 DI-24): on an NPC's body, each fire
bit the body reads fires the item's ammo from its launch point through the game's NPC fire entry in
a weapon range of its own. A player's body fires nothing.
_Avoid_: clip shots, anim fire, NPC weapon (an NPC's clip fires ammo, not a weapon.def row)

**Impact board**:
An ammo record's impact rows in the Definition preview (ADR 0046 DI-23): for each of the twenty
surface classes, the effects row a round of it plays there as the game's impact presenter picks it
(its own row, else ammo def 0's fallback), each a Go to and playable on the weapon range.
_Avoid_: effects table (the ammo.def block the board reads), impact matrix

**Shoot tool**:
The mission view's tool (ADR 0046 DI-23) that fires the picked ammo where a click meets the terrain
or an object, in a world built from the mission by the game's own load, and plays the impact, the
damage and any death the game would. It edits nothing.
_Avoid_: fire tool, test fire, gun

**Rig**:
What an animation plays on: an animation table (its reset clip the bind) or a lone clip
(its own bind) over a model's bone table (its parts' pivots and parents), loaded through
the game's own loader.
_Avoid_: skeleton (the rig's bones alone), armature (Blender's)

**Selection / primary**:
The records selected in the active document, of any of its rows. The primary is the one selected
last or the one a marquee names: the Inspector's form, the handles and the place a new record goes
follow it, and an arrange aligns the others to it. Several records of one kind share one form,
where a change sets them all in one undo step. Each open document keeps its own.
_Avoid_: focus (the keyboard's), active (the active document, not a record)

**Arrange**:
The selected menu windows aligned by an edge or their centres to the primary's, spread
with equal gaps between them, or moved in the drawing order among their siblings (the game
draws a window's children in the file's order, a later one over an earlier one), one
undo step, the moves written by the same rules as a drag.
_Avoid_: layout (the runtime's POSITION solve), z-index (the format has none: the order is
the file's)

**Compiler note**:
What the menu frame compiler makes of a screen where the picture may not be what the author meant
(a colour that reads transparent, a label cut short, a file that did not load), as a code on the
record and field that cause it, with its basis: witnessed, port policy or deferred. The engine
keeps codes; the editor words them. Notes observe: the picture is the same without them.
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
A document type's own check across the project's files, run after each file's own findings and
remade only for what moved (the menu type's render check). Its findings are Problems rows after the
build's gate, never in it. A use check is the other cross-file finding, in the gate and keeping
nothing; whether a finding may block a build decides which one it is.
_Avoid_: use check (in the gate, stateless, one per kind of file), validation (a file's own
findings)

**Finding code**:
A finding's stable dotted token (`reference.missing`, `style.line_ending`) and the row it is made
from: its fixes, whether the file still serializes, where Problems takes it, its family, its source,
and whether an error of it refuses a build (every code's does but `reference.missing`'s, ADR 0046
S14). The editor's codes are one table and each document type declares its own family; a code no
table declares cannot be made. The wire's `code` is the token.
_Avoid_: error code (a finding may be a warning or a note), message id, diagnostic code (Diagnostic
is the record's type, the code its row's token)

**Canvas**:
Where the editor shows a device's picture and takes the pointer and the keys over it: it tells a
click from a drag (one gesture at a time), zooms and pans, and draws the shapes its kind makes
(outlines, handles, markers, a marquee). The kind says what a press takes and what a drag writes;
the canvas owns the input, the viewport its state, the device the picture.
_Avoid_: overlay (one shape drawn over the picture), view (a document's view in the Document
window), picture (what the device renders, which the canvas shows)

**Gesture**:
The edits one continuous action on a canvas makes (a drag, an arrow key held): one token, folded
into one undo step until it ends (let go, an undo, a save, Build or Play, another gesture's batch);
a wire client's gesture also ends after ten seconds with no sample. A batch of it that adds, removes
or moves a row is a step of its own. A viewport whose picture is costly to make (a model's scene)
holds its picture while one is open and makes it once at the end.
_Avoid_: transaction (the rename's), group (a coalesced typing burst of one field)

**Batch**:
Edits on any rows of one document applied as one undo step, each against the rows as the ones
before it left them, nothing committed when any is refused. A later edit may name a row an earlier
one made (in the editor MCP, by its label), so one step adds a window and fills it in. A new name
is its record's edit alone: what names the record keeps the old name until Rename everywhere.
_Avoid_: transaction (the rename's), gesture (edits folding one after another until an end),
follow (the same-file rename S13 D5 removed)

**Change set**:
What changed in a document between a state a window or a preview last read and now, in its kind's
words: a record document's rows added, removed and changed; a text document's spans. When the
document cannot say, everything changed. What is made of a document follows it: a viewport makes
its picture again only for a change of what the picture reads, an outline only a changed row's lines.
_Avoid_: diff (of files on disk), delta, dirty (unsaved edits, against the saved file)

**Text document**:
A document of text (a script, a music script, a credits file, a shader, a configuration), held as
the game reads it, in its code page, as lines, its places `line:column`. Its one change is a span
replaced. Its type reads and writes the form its file is stored in (a compiled form held as its text
and written back byte for byte while left as it is), checks it through the game's own reader or the
toolchain that writes the form, and names the references its text makes, each at its span.
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
The one operation behind Play and Export: validate the project, route every asset into the
canonical archives (`language.pff`, `localres.pff`, `resource.pff`) and the mandatory loose files,
write through the streamed PFF writer, verify through the VFS, and publish an immutable
`.opennova/build/play/<build-id>/`. Incremental by per-archive input hash, the machine-local
**build cache** keeping each file's hash, so only changed files are read and an unchanged archive
is linked from the last good build. Import sources and unknown files are left out.
_Avoid_: pack (a step inside a build), export (a build copied to a chosen directory),
stage (the retired retail-staging vocabulary)

**Operation**:
A long job of the editor's session: opening a project, a refresh, an import's plan and write, a
rename's commit, a build. One runs at a time, a step at a time within the frame's budget, so the
editor keeps drawing; nothing it makes reaches the view until it finishes, and a Cancel stops it
between two steps. It declares what it reads and writes, and a request that conflicts with it meets
the busy gate: refused, joined, replacing it or cancelling it, as the two rows say. Validation steps
a file at a time too but is no operation.
_Avoid_: task, job, background work (nothing runs on another thread)

**Play**:
Build, then launch the game runtime (`opennova.exe -- --resource-dir <build> --mcp-port <n>
[--mission <name.bms>]`) as the editor's one managed child, in a run directory; Stop ends it. Play
starts the game at its menu; **Play mission** (Ctrl+F5) in the active document's mission. It runs in
the project's **Play mode** and saves unsaved files first (`save_before_play`, on by default).
_Avoid_: run (ONED's vocabulary), preview (an in-editor render, not a running game),
"see in game"

**Play from here**:
Play of a mission with the player starting at a point of it (the ground under the mission view's
camera, Alt+F5, or under a picked point). Neither game takes a place on its command line, so the
build's copy of the mission in the run directory has its single-player markers moved there (a
**start marker**), never the project's file.
_Avoid_: teleport, spawn point (a deploy zone's), warp

**Play log row**:
A Problems row of what a Play's game looked for and did not find, on the project file that names it,
with that reference's fixes. OpenNova says each miss on its log; the game install's rows are read
from its file and graphics logs once it exited. Each mode's rows stay until its next Play.
_Avoid_: crash log, error list, log scrape

**Run directory**:
Where Play runs the game: `.opennova/run/<mode>/<n>/`, the game's working directory with its log and
the saves the game keeps beside itself, so the build stays as written. Strict Play links the build's
files and the install's executable and Bink DLL there (an expansion's Play also the install's base
game). Each Play takes the first free one of its mode and keeps what that mode's runs wrote (Play
fresh empties it).
_Avoid_: build directory (what the build publishes, never written after), working copy, stage

**Strict Play**:
Play in the game install as a player's drop-in: the run directory holds the build's files and the
install's executable and Bink DLL alone, and the game is launched without `/d`, so it reads its
archives first and writes its own configuration (the **Play mode** `strict`). An expansion plays as
the stock game's `/exp`. The OpenNova runtime is the day-to-day run.
_Avoid_: retail mode, clean run, vanilla run

**Play mode**:
How Play runs a project's build: `runtime` (the OpenNova runtime) or `strict` (Strict Play), each
also the name of its run directories. A project's own, kept in its `.opennova/local.json` with
whether Play saves first, never the editor's (shared by every editor on the machine). A project that
never chose plays in the runtime; a Play request may name a mode for itself alone.
_Avoid_: play setting (it is a project's, not the editor's), retail toggle

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
`world::WeatherState` on the simulation tick, the ONE home for weather authority state:
`MissionKernel::tick_weather` runs the sim legs after every logic tick over `env::WeatherRuntime`,
and standalone owners (previews, GUT fixtures) tick the same runtime. Record: `docs/env/env-tod-re.md`.
_Avoid_: a second weather clock or state copy anywhere; "environment system" for the weather
authority (the `.env` document model is `formats/env`)
