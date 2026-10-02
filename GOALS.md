# OpenNova Goals

OpenNova is a faithful reimplementation of NovaLogic's game engine. The aim is
feature and visual parity with the original games, built so the engine runs off the
same asset data those games shipped. The result is an engine and an open toolchain
for understanding, producing, validating, and shipping NovaLogic-compatible game
data.

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
also gives us a mature rendering pipeline and cross-platform packaging. The portable
engine core is C++; Godot is the shell that renders it and powers the game.

## A source-first game-data loop

The game's canonical files live in an ordinary source tree: a project directory of
loose NovaLogic-format files that the OpenNova Editor (ADR 0046) creates, edits through
the engine's own format libraries, validates against the names the engine requires, and
packs into PFF archives only when the user plays or exports. There is no proprietary
project database: the project file is plain text and every asset is a readable file.
That keeps source ownership visible and makes retail compatibility the acceptance test.

OpenNova ships the runtime and format libraries plus its own bundled
`assets/` (authored from scratch: today the placeholder main menu whose PLAY
RETAIL picks and remembers a retail install, ADR 0048, and a first-person
carbine exported from the Blender sources in `art/`); it never distributes
retail game data. Users point it at their own game-data directory, on the
command line (`--resource-dir`, ADR 0045) or through that picker. The editor is
a separate download and a separate application. Test-only synthetic fixtures
remain separate from both products.

## Target games

Joint Operations (JO) is the first game we are bringing up end to end. Earlier
NovaLogic titles are conceptually the same engine with different data formats and
networking; at a high level each game is a skin of the previous one with upgraded
engine features. We do not aim to reimplement every older title, but we do intend to
support the parts modders care about most: their models and animations (`.3di` and
friends) and their missions, migrated into the newer formats so they load in
OpenNova.

## NovaWorld and multiplayer

Networking is held to the same parity bar as everything else, applied to the byte
stream: it is **wire-compatible by design**. The aim is that our clients can join
original (retail) servers, our servers can serve original clients, and opennova↔opennova
works the same way, so the original games keep working as their official services age
out, and our runtime and theirs are interchangeable on the same protocol. The
matchmaking and server backend (NovaWorld) is reimplemented and maturing
(`apps/novaworld_server`, `engine/net/novaworld`); in-match replication is exercised in
both directions against real captures and live retail sessions (retail clients join
and play on our hosts), with the remaining gaps tracked in
[the divergence ledger](docs/divergence-ledger.md). The protocol reverse
engineering is recorded in
[docs/net/novaworld-net-re.md](docs/net/novaworld-net-re.md). Single-player already runs
as an in-process listen server, so co-op and multiplayer share one replication path
([the listen-server ADR](docs/adr/0011-single-player-in-process-listen-server.md)).

## Where we are today

OpenNova is pre-1.0 and under active development. The engine and its format libraries
are the most exercised surfaces; the runtime loads the game data, runs the terrain
and foliage systems, and simulates missions (WAC scripts, BMS events, AI);
the gameplay systems (weapons, projectile physics and damage, throwables,
mounted and emplaced weapons, vehicles, item destruction, optics and the HUD)
are ported with test coverage, and multiplayer runs on the wire-compatible
in-match protocol ("NovaWorld and multiplayer" above). Every system here is
experimental. See the [README](README.md) for current capabilities, downloads,
and build steps.
