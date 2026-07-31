# OpenNova Goals

OpenNova is a faithful reimplementation of NovaLogic's game engine. The aim is
feature and visual parity with the original games, built so the engine runs off the
same asset data those games shipped. Think of it as a build-your-own-game tool in
the spirit of RPG in a Box, but for the NovaLogic catalog: an engine plus a deep
editor for the data that drives it.

## A faithful reimplementation

The goal is parity, not reinterpretation. Terrain, models, foliage, environment,
menus, and the rest should look and behave like the originals. Where we deviate, it
is a tracked decision, not an accident. The engine reads the canonical NovaLogic
formats directly, so original content loads as-is.

## Data driven, by convention

The NovaLogic engine is data driven, and that is what makes it worth studying and so
moddable. Almost everything (weapons, items, terrain, missions, menus) is described
by data files and wired together by convention rather than hardcoded. OpenNova works
off that same data and honors the same conventions. Much of this behavior was
recovered by reverse engineering the original binaries, so the conventions we follow
are the ones the games actually use.

## Why Godot

We chose Godot as the shell. Other options were considered (SDL3 + bgfx, a custom
stack), but Godot is powerful enough that rebuilding the original engine's features
on top of it is largely a matter of mapping NovaLogic concepts onto Godot ones. It
also gives us a mature rendering pipeline, an editor framework, and cross-platform
packaging. The portable engine core is C++; Godot is the shell that renders it and
powers the tools.

## A deep editor

The OpenNova Editor (ONED) is where you author the data the engine runs. Today it
focuses on creating individual assets: terrain, 3D objects, missions, particle
effects, playable characters, fonts, credits, strings, menus, music, sound
profiles, and environment (plus a preview-only HUD layout workspace). The
longer-term goal is depth:

- **An asset dependency graph.** NovaLogic assets reference each other (a `.3di`
  points at its textures, a `.def` points at a `.3di` and its `.bad` animations, a
  mission points at definitions). Modeling that graph lets the editor validate
  references, follow them, and show what breaks when something changes.
- **A Game workspace.** A place to assemble a whole game: set up menus, bind assets
  to the conventions the engine expects, and configure the pieces that turn a pile
  of assets into a playable title.
- **Export to a standalone game.** Eventually, produce a runnable executable from an
  OpenNova project, so creators can ship what they build.

## Target games

Joint Operations (JO) is the first game we are bringing up end to end. Earlier
NovaLogic titles are conceptually the same engine with different data formats and
networking; at a high level each game is a skin of the previous one with upgraded
engine features. We do not aim to reimplement every older title, but we do intend to
support the parts modders care about most: their models and animations (`.3di` and
friends) and their missions, promoted into the newer formats so they load in
OpenNova.

## NovaWorld and multiplayer

Networking is held to the same parity bar as everything else, applied to the byte
stream: it is **wire-compatible by design**. The aim is that our clients can join
original (retail) servers, our servers can serve original clients, and opennova↔opennova
works the same way — so the original games keep working as their official services age
out, and our runtime and theirs are interchangeable on the same protocol. The
matchmaking and server backend (NovaWorld) is reimplemented and maturing
(`apps/novaworld_server`, `libs/novaworld`); in-match replication is exercised in
both directions against real captures and live retail sessions (retail clients join
and play on our hosts), with the remaining gaps tracked in
[the divergence ledger](docs/divergence-ledger.md). The protocol reverse
engineering is recorded in
[docs/net/novaworld-net-re.md](docs/net/novaworld-net-re.md). Single-player already runs
as an in-process listen server, so co-op and multiplayer share one replication path
([the listen-server ADR](docs/adr/0011-single-player-in-process-listen-server.md));
broader in-match gameplay netcode comes later.

## Where we are today

OpenNova is pre-1.0 and under active development. The asset pipeline and the editor
are the most exercised surfaces; the runtime loads exported scenes, runs the terrain
and foliage systems, and simulates authored missions (WAC scripts, BMS events, AI);
player interaction and multiplayer are still being built. Nothing here is
production-ready. See the [README](README.md) for current capabilities, downloads,
and build steps.
