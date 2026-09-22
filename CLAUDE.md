# OpenNova agent guide

OpenNova is a faithful, pre-1.0 reimplementation of the JO-generation
NovaLogic games. [README.md](README.md) covers the repository and first
run; [GOALS.md](GOALS.md) states the product goal; [CONTEXT.md](CONTEXT.md)
defines project vocabulary. Start at the
[documentation index](docs/README.md) for architecture and research.

## Where work belongs

- `engine/` is the portable C++ core: formats, world simulation, runtime
  systems, and wire protocols. Read [engine/CLAUDE.md](engine/CLAUDE.md)
  for its include, dependency, and test rules.
- `godot/src/` is the C++ GDExtension binding and device layer;
  `godot/game/` is the game shell and game-level GDScript;
  `godot/probes/` contains runtime MCP probes. Read
  [godot/src/CLAUDE.md](godot/src/CLAUDE.md) for the binding boundary.
- `tests/` runs CTest; `godot/tests/` runs GUT. Read
  [godot/tests/CLAUDE.md](godot/tests/CLAUDE.md) before adding or
  diagnosing GDScript tests.
- `apps/` contains native tools and NovaWorld services. `launcher/`,
  `web/`, `backend/`, `deploy/`, and `infra/` cover distribution
  and hosted services. [DEVELOPING.md](DEVELOPING.md) owns local commands;
  [DEPLOY.md](DEPLOY.md) owns deployment.
- `docs/` owns architecture, ADRs, and cited retail findings.
  [docs/current-state.md](docs/current-state.md) routes to current work;
  the [divergence ledger](docs/divergence-ledger.md) owns open parity gaps.

## Working rules

- Recreate witnessed retail behavior and its look. Research an unwitnessed
  behavior before porting it; cite the original at the implementation site
  as `[orig: Name @ 0xADDR]`. Use standard equivalents for CRT, OS, and
  device primitives. The [engine primer](docs/engine-primer.md) and
  [research skills](.claude/skills/engine-research/SKILL.md) describe
  the evidence workflow.
- Retail file and wire compatibility is the product contract. Encoders
  produce bytes retail accepts; decoders accept retail bytes. Do not make
  a parity test pass by carrying raw original bytes through a writer.
  Record intentional differences in an ADR or the owning RE record and
  ledger. See [the network record](docs/net/novaworld-net-re.md) and
  [ADR 0003](docs/adr/0003-no-raw-passthrough-create-from-scratch.md).
- Follow [ADR 0043](docs/adr/0043-canonical-cpp-and-godot-hard-cut.md):
  engine facts live in engine functions; Godot owns device work and
  typed bridges. NovaLogic documents read and write themselves, outside
  Godot's resource system. Pre-1.0 internal refactors update every caller
  in the same change; retail compatibility is never relaxed.
- Use [CONTEXT.md](CONTEXT.md) terms. In particular, *host* means the
  authoritative game-session side. Rendering targets retail's
  fixed-function look.
- Keep TODO and checklist files open-only; completed entries live in Git
  history. Historical records may retain completed milestones.
- Public copy names JO and newer titles, says pre-1.0 or experimental,
  and avoids claims of production readiness.

## Build and verification

Use [DEVELOPING.md](DEVELOPING.md) for prerequisites and commands.
The usual entry points are `scripts/build.sh` (native build, CTest, then
GDExtension), `scripts/build.sh --no-godot` (native only),
`scripts/build_godot.sh` (GDExtension), and `scripts/test_godot.sh`
(GUT). A fresh checkout needs submodules; a fresh Godot worktree also
needs the addon bootstrap, a built GDExtension, and one headless import.
Restart the editor after a native binding change.

A green asset-gated run may have skipped retail data. Check
[asset-gated-tests.md](docs/asset-gated-tests.md) for the two retail roots
and the skip signals. GUT can drop an unparseable script; use its wrapper
and isolate a failing file before treating a full-suite failure as
deterministic. The [GUT skill](.claude/skills/gut/SKILL.md) has the exact
procedure. Avoid PowerShell 5.1 Get/Set-Content for bulk rewrites of
BOM-less UTF-8 `.gd` files.

## Git and further guidance

Use an isolated worktree for agent changes and leave unrelated working
tree changes untouched. Never merge a PR; the maintainer merges. Put
review context in the PR description and commits, not PR comments.
The [agent runbooks](.agents/README.md) cover networking and retail
interop; [project skills](.claude/skills/) cover research, record
updates, GUT, and game MCP work.
