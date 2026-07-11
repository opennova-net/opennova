# NovaWorld integration plan

> **Completed-effort record.** This plan tracked the integration of the NovaWorld server,
> the gate, the website, the launcher, and the deploy infrastructure into this repository;
> the work landed on master (PR #136). Current truth is the source and
> [docs/net/novaworld-net-re.md](../docs/net/novaworld-net-re.md);
> [status.md](status.md) keeps the historical PR tables. Track designs:
> [Track A (server + engine)](track-a-server-engine.md),
> [Track B (infra + deploy)](track-b-infra.md),
> [Track C (launcher + website)](track-c-launcher-web.md).

## Goal

A faithful reimplementation of NovaWorld, the NovaLogic matchmaking and backend service,
deployable from this repository. When this effort lands:

- The original games (JO and newer) keep their multiplayer alive through our NovaWorld
  server, reached via launcher-managed hosts-file redirection. No game files are
  modified, no code is injected.
- Our own runtime implements the multiplayer menu against the same protocol libraries:
  open the game shell, click NovaWorld, get a verified session, browse live servers,
  host a game that other clients can see.
- Anyone can deploy their own site and server with the same commands we use. Secrets
  come from a 1Password vault; the only secret an operator handles is a service-account
  token, and the only dependency on any machine involved is docker.

This effort also lays the core networking and game-flow APIs of the engine. In-match
gameplay networking (the JOINTOPERATIONS protocol) is not implemented here, but its
foundations are: protocol-number dispatch, unknown-message tracking, and the world-tick
seam it will enter through.

## Where the code comes from

Two prior implementations exist. Both inform this effort; one provides the code:

- **The PR #37 C++ stack** (rescued on branch `net/pr37-rescue`) is the code we reland:
  `libs/{novacrypto,napi,novaworld}` protocol libraries, `apps/novaworld_server` (gate
  UDP 7597, NovaWorld UDP 64206, HTTP with the legacy `NW*.dll` routes and the `/api/*`
  backend), the Vue web portal, and the Godot `NovaWorldClient` binding. It handles all
  ten documented session containers and carries wire fixes verified against stock
  clients.
- **opennova-int** (the working Python proof of concept) stays out of this repo as the
  behavioral reference: parity fixtures, deployment lessons, and the launcher port
  source.

The protocol ground truth is `docs/net/novaworld-net-re.md`, reverse engineered from the
original binaries. Where our code and that record disagree, IDA is the source of truth.

## Principles

1. **IDA before guesswork.** Questions about wire behavior are answered by decompiling
   the original client, not by trying things until they work. Grill sessions update the
   code, the IDB, and the RE record together.
2. **Unknown messages are tracked, not dropped.** Every unhandled gate VAR, opcode,
   container, protocol number, and HTTP route is recorded with a sample so it can be
   implemented later.
3. **Protocol code lives once**, in Godot-free libraries shared by the server and the
   engine client. Sockets belong to the apps.
4. **No secrets in the repo.** Public deployability is a feature: a third party with
   their own 1Password vault, AWS account, and Cloudflare zone deploys with the same
   commands.

## Branch and PR flow

- Integration trunk: `web-nw-for-real-master` (created from master). Every PR in this
  effort targets it.
- When the goal is reached, `web-nw-for-real-master` opens as a single PR against
  master.
- The PR sequence, dependencies, and current state are tracked in [status.md](status.md).

## Naming

- **NovaWorld** now refers exclusively to the service and its reimplementation
  (`apps/novaworld_server`, `libs/novaworld`). It is our NovaWorld server, not an
  emulator.
- The runtime world-sim host scene formerly named `NovaWorld` was renamed **GameWorld**
  (`godot/engine/world/game_world.gd`) as part of this effort.
- The gate is `novaworld_gate`; the in-game server list is the `novaworld_browser`.
