# OpenNova

Glossary of the project's domain language. Definitions only: what a term *is*, not how it's implemented. Pick one canonical word per concept; alternatives go under _Avoid_. The runtime architecture map lives in [docs/runtime-architecture.md](docs/runtime-architecture.md); the documentation index is [docs/README.md](docs/README.md).

## Menu UI (MNU)

The vocabulary for NovaLogic's `.mnu` menu system and OpenNova's runtime + editor support for it.

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
A behavior the `.mnu` file itself can express, via an `<ACTION>` element: navigate to a screen or menu, show/hide a named window, pop, or open a URL. This is the *entire* behavior vocabulary the format carries.
_Avoid_: command, event, handler

**Command**:
Game-semantic behavior a button performs that the format *cannot* express (start a mission, apply video settings, connect to a server). The host/engine supplies a Command by matching a widget's **name**; it is never written in the `.mnu`.
_Avoid_: action (reserve that strictly for the `<ACTION>` element)

**Menu Host**:
The runtime front-end that loads a menu set, drives a live interactive menu, plays its audio, and supplies Commands by control name. The menu counterpart to the world runtime.
_Avoid_: menu manager, controller

**Menus workspace**:
The OpenNova Editor (ONED) surface for authoring `.mnu` files (WYSIWYG canvas + tree + inspector).
_Avoid_: menu editor (ambiguous with the runtime menu)

**Edit mode**:
The flag that makes a live menu inert and click-through so the editor can reuse the exact runtime node as a WYSIWYG preview. Off = fully interactive runtime.
_Avoid_: preview mode, design mode

**Interactive preview**:
An Edit-mode menu the Menus workspace can put into a "play" state: navigators wire up so clicking a Tab runs its window show/hide and screen Actions, while external Commands (launch/quit/URL/cross-menu) are sandboxed to no-ops. Lets an author preview tab/screen flow without leaving the editor.
_Avoid_: play mode, runtime (it is still a preview)

**Tab**:
A Window shown or hidden by a sibling button's `window` Action (e.g. the Options panels). Not a widget type, just an authored convention: one button per panel, each `<ACTION type="window">` hiding the siblings and showing its own.
_Avoid_: page, panel (when you mean the toggling mechanism)

## World & NovaWorld

The vocabulary separating the in-game world from the online service. The names collided
historically; they are now distinct.

**GameWorld**:
The runtime world-sim host scene (`godot/engine/world/game_world.tscn`): terrain,
environment, mission runtime, and audio under one embeddable root. The standalone
game is the sole live mission host; ONED authoring previews do not run gameplay
(ADR 0025). Formerly named `NovaWorld`.
_Avoid_: NovaWorld (that name now belongs to the service), world scene

**NovaWorld**:
NovaLogic's online matchmaking and account service, and our reimplementation of it (`apps/novaworld_server`, `libs/novaworld`). Always the service, never the in-game world. It is our NovaWorld server, not an emulator.
_Avoid_: emulator, lobby server

**Gate**:
The first-contact UDP service (`novaworld_gate`, port 7597) that bootstraps a client with the HTTP service URL and the NovaWorld UDP endpoint.
_Avoid_: lobby

**Browser**:
The in-game server list (`novaworld_browser`) populated from the service's GSB/GLB data.
_Avoid_: lobby, server list (in code)

**Wire-compatible / wire protocol**:
Code that produces and consumes the exact byte stream the original game uses, so original
and OpenNova endpoints interoperate: our clients can join original servers, our servers can
serve original clients, and opennova↔opennova works the same way. The protocol witness
record is `docs/net/novaworld-net-re.md`.
_Avoid_: "our own protocol", custom packet format

**Host / Client**:
The authoritative side of an in-match session (the **host**) versus a connected peer (a
**client**). Under the listen server the host runs a local client too.
_Avoid_: master/slave, owner (when you mean the host)

**Listen server**:
A host that is simultaneously the authoritative server and a local client. OpenNova's
single-player runs this way — the sim serializes real entity state through the wire codec
and the present pass renders the locally-decoded result, so SP, co-op, and multiplayer
share one replication path (only the transport differs). See ADR 0011.
_Avoid_: standalone server, dedicated server (those have no local player)

**In-match / Matchmaking**:
The two network protocol domains. **In-match** is the 62 Hz game session between a host
and its clients (the wire codec + replication runtime). **Matchmaking** is everything that
gets players INTO a match: gate, browser, accounts, NAT rendezvous — the NovaWorld
service's domain. Code and libs are named by their domain, never bare "net".
_Avoid_: unqualified "net code", lobby (for either)

**Wire codec / Net runtime / Net seam**:
The three in-match layers: the **wire codec** (`libs/npwire`, ADR 0019) encodes/decodes the
byte stream (the message catalog is its single source of truth, ADR 0013); the **net
runtime** (`libs/npruntime`)
runs the 62 Hz host/client session over it; the **net seam** (`libs/netsim`) is where the
world sim and the wire meet (`INetCommandSink`, the replication fan, ADR 0009).
_Avoid_: "the netcode" (say which layer)

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

## Products & modes

**Product**:
A shipped executable. There are exactly two: **`opennova.exe`** (the game) and
**`opennova-modtools.exe`** (ONED). Everything else that builds from this repo is a tool
or a service, not a product (ADR 0015).
_Avoid_: app (ambiguous), the runtime (as a product name)

**Serve mode**:
`opennova.exe` hosting a match without being a player: the server-options menu path,
runnable windowed or `--headless`. A mode of the game product, never a separate binary,
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
`docs/required-resources.md` (landed at ENG-6); the engine manifest derived from it is what
boot validation and ONED diagnostics consume, and it defines what a person starts with to
make a new game.
_Avoid_: core assets, base game files

**Promote**:
Reserved for `mission::promote_mission` — spawning a parsed mission into the live world
(entities, AI brains, nav), the IDA-cited spawn path used by the standalone game
and dedicated dev hosts. Other historical uses of the word (old-title format upliftment, 3DI→IR
normalization, fixture curation, code relocation) should be phrased as *migrate*,
*normalize*, *whitelist*, and *move* respectively.
_Avoid_: promote (for anything but the mission→world spawn)

## Editor & Runtime

**ONED**:
The OpenNova Editor (`godot/modtools/`): the authoring application, thirteen workspaces over
one code-first framework. "ONED" or "the editor" in prose.
_Avoid_: terrain editor, modtools (as a name)

**Workspace**:
One asset-domain authoring surface inside ONED (Terrain, Object, Avatars, Mission, Fonts, Credits,
Strings, Menus, HUD, Music, Particles, Sound, Environment). A workspace declares itself as a typed
registry row and exposes capability hooks; the shell never switches on its type.
_Avoid_: tab, tool, mode

**HUD**:
The in-game heads-up display, laid out by `hudpos.def`; also the read-only ONED workspace
that previews that layout. RE record: `docs/interface/hud-re.md`.
_Avoid_: overlay (that is the debug overlay), UI (too broad)

**Present pass**:
The per-frame apply step that projects simulation state onto scene nodes
(`MissionPresentPass`). It runs once in the standalone game host; F6 tests the
current saved loose mission through that same game path (ADRs 0006 and 0025).
_Avoid_: render pass, sync pass
