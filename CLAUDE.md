# OpenNova — agent notes

Open-source (MIT) reimplementation of NovaLogic's game engine. Joint Operations (JO) is the first
game being brought up. Repo layout, downloads, and end-user build docs live in
[README.md](README.md); the vision is [GOALS.md](GOALS.md). This file covers what is
easier to relay than to rediscover.

## Map

- `engine/` — the engine: the portable, Godot-free C++ core (ADR 0028; namespace
  `opennova`, unchanged). Four groups — the directories and, since ADR 0029, the CMake
  build targets too (five STATIC group targets: `opennova_base`, `opennova_formats`,
  `opennova_runtime`, `opennova_net`, plus the separate `opennova_novaworld_service`;
  header-only `opennova_io`; no per-lib targets except the `opennova_crt`
  STATIC leaf — the shared CRT rand stream both formats and runtime link):
  `base/` (io, crt, vfs, resource_index, gameprofile, pcapio),
  `formats/` (one directory per NovaLogic format — pff, threedi, def, mnu, env, ...;
  ADR 0024 layout; the target also builds mission's format half), `runtime/`(
  world, wac, mission, anim, audio, particle, renderer, controls, terrain,
  terrain_query, environment, hud, menu, simassets, devtools — the Dear ImGui
  pass with the game's F3 dev-tool windows (debug builds only) and ONED's run
  surface, ADR 0039),
  `net/` (novacrypto, napi, npwire, novaworld, inmatch, plus the internal
  netsim/npruntime implementation directories). Native consumers link the engine
  groups directly.
  See `engine/CLAUDE.md`.
- `godot/` — the Godot 4.6.1 project: `src/` (pure C++ GDExtension bindings —
  part of the core engine, ADR 0034 d6; see `godot/src/CLAUDE.md`),
  `game/` (the game shell plus its game-level GDScript runtime — world,
  debug, mission, object, terrain, ui, ...),
  `modtools/` (ONED: settings, loose OpenNova run,
  staged retail run, Stop, and the hidden release pack command; its surface
  is the engine's ImGui `OnedUi` window, the app only executes its requests),
  `tests/` (GUT suite).
- `apps/` — `novaworld_server/` (the NovaWorld service), `nw_server/` (dev/golden-harness
  in-match host; never shipped), `nw_lan_probe/` (LAN readiness probe),
  `nw_pp/` (NovaWorld in-game packet pretty-printer/decoder), `common/` (shared
  socket helpers, deliberately app-layer; pcap I/O lives in `engine/base/pcapio`).
- `web/` — NovaWorld web portal (Vue 3 + TS); `launcher/` — Windows tray app pointing a
  stock install at our servers; `backend/` + `deploy/` + `infra/` — service data and
  deployment stack (DEPLOY.md).
- `tests/` — C++ ctest suite (separate from `godot/tests/`; different runners).
- `docs/` — tracked golden docs (ADRs, RE records), kept pristine: they represent the
  best current understanding of the original engine. RE findings land there directly
  (via the `re-doc` skill) — there is no scratch directory.
- `third_party/` — vendored submodules (godot-cpp and gut; never edit in
  place — bump submodules upstream) plus vendored in-tree bcrypt sources and two
  hash-pinned FetchContents: sqlite (bump by editing the URL/URL_HASH in
  `third_party/sqlite/CMakeLists.txt`) and Dear ImGui (`third_party/imgui/`,
  pinned to the commit the imgui-godot addon bundles — bump it and
  `scripts/bootstrap_imgui_godot.sh` together, never one alone).

## Build & test

Important: in this linked worktree, run build/test/bootstrap/Godot commands outside the sandbox
(request escalation). The submodule bootstrap and Godot test runner touch git metadata and
processes in the main checkout outside the worktree, so sandboxed runs can fail or leave stuck
processes.

```bash
scripts/build.sh          # C++ build + full ctest (Release); BUILD_GODOT=0 skips the GDExtension
scripts/build_godot.sh    # GDExtension only -> godot/bin/; fully restart the editor after
scripts/test_godot.sh     # GUT GDScript suite, headless
python scripts/lint/<check>.py --enforce   # the CI maturity gates (stdlib Python, no venv; ledger_check takes --check)
```

- Fresh worktree/clone: `git submodule update --init --recursive` first — `third_party/`
  ships empty and the build scripts self-init only GUT (`scripts/build.sh` and
  `scripts/test_godot.sh` both run `scripts/bootstrap_godot.sh`); godot-cpp still
  needs the manual init. Then build the GDExtension (`godot/bin/` has no DLL in a
  fresh worktree) and run `"$GODOT_BIN" --headless --path godot --import` once, or engine
  classes appear missing.
- `scripts/test_godot.sh` needs `GODOT_BIN` or a `Godot_v4.6.1-stable_*` binary in
  `.godot-bin/` (worktrees don't have one — point `GODOT_BIN` at the main checkout's).
- The GUT suite shares `user://` state and is flaky in full runs. Before believing a
  failure, re-run that one file in isolation (see `godot/tests/CLAUDE.md`).
- Full ctest is ~90 s. Scope during focused work:
  `ctest --test-dir build -C Release -R "<pattern>"` (e.g. `-R "mission|terrain"`).
- For a stale Godot editor, rebuild via `scripts/build_godot.sh` and fully restart it.
- Asset-gated tests SKIP-AS-PASS unless env vars point at local retail installs or
  captures — a green run does not mean they exercised data. The full var→test→data
  matrix, local setup, and the never-commit-captures policy live in
  [docs/asset-gated-tests.md](docs/asset-gated-tests.md). Machine paths go in
  `.claude/settings.local.json` `env` (never tracked); capture files default to the
  gitignored `.scratch/` under the repo root (goldens in `.scratch/golden/`).
- Windows PowerShell 5.1 `Get-Content`/`Set-Content` corrupts BOM-less UTF-8 `.gd` files.
  Do bulk text rewrites with bash sed/perl, not PowerShell.

## Conventions

- `engine/` libraries: built as the five group targets (ADR 0029 — no per-lib CMake
  targets), C++ namespace `opennova`; `godot/src/` is the typed Godot binding layer.
- This is a faithful reimplementation — parity, not reinterpretation ([GOALS.md](GOALS.md)).
  Implementing "our own version" of engine behavior is never allowed: port the witnessed
  original as a structural translation and cite it inline (`[orig: Name @ 0xADDR]`) unless a
  tracked decision (ADR / RE-record divergence entry) says otherwise. A request to
  "implement X" (the HUD, a weapon, an effect) is a request to first understand how the
  original engine did X — its behavior AND its look — via the `engine-research`/`grill-ida`
  skills, then port that; it is never a request to invent an X. Excluded: CRT/OS/platform
  primitives (strcpy/sprintf, D3D, file I/O) — use standard or platform equivalents. Engine-wide
  conventions (binaries/IDBs, fixed-point, coordinates, the 62 Hz tick) are in
  [docs/engine-primer.md](docs/engine-primer.md); RE-doc conventions in [docs/README.md](docs/README.md).
- The in-match session owns lifecycle, role policy, fixed-tick banking, and
  input consumption in portable C++ (`engine/net/inmatch/session.*`; ADR 0036,
  superseding ADR 0035's old name/location). An `inmatch::TickTarget` supplies
  the concrete simulation kernel. Godot's first-class `GameFramePipeline` samples one
  typed frame input, advances that session, and orders Godot-only presentation/device
  work once per display frame. A `godot/` line earns its place only as that device
  work (node writes, GPU dispatch, input sampling, audio players) or a thin typed seam.
  Nova formats never touch Godot's resource system:
  documents read/write themselves (`load_from_path`/`save_to_path`).
- Never carry raw original bytes through a writer to make a parity test pass — writers
  produce output from scratch (docs/adr/0003-no-raw-passthrough-create-from-scratch.md).
- Pre-1.0, no internal backwards compatibility: refactors update every caller of our own
  code in the same change — no deprecation shims, migration paths, or compat readers for
  files our tools write. Retail interop (wire/format parity) is the product, not
  back-compat, and is never relaxed.
- Rendering targets the original fixed-function look, not PBR.
- Networking is wire-compatible by design — the parity rule applied to the byte stream.
  We write code such that our clients can join original (retail) servers, our servers can
  serve original clients, and opennova↔opennova works the same way. Every encoder produces
  bytes a stock client/server accepts; every decoder reads what a stock client/server
  emits; opennova↔opennova requires encoder/decoder self-consistency, retail interop
  requires byte-parity. (Pointing a client at NovaLogic's *live hosted* service is a
  separate matter, done sparingly with no load/abuse — that caution is not a license to
  write non-wire-compatible code.) See [docs/net/novaworld-net-re.md](docs/net/novaworld-net-re.md)
  and ADRs 0009–0012.
- "Host" means the game/server host and nothing else (CONTEXT.md "Host / Joiner");
  attach-points are Mounts, presentation owners are Presenters, front-ends are Shells,
  a lib's embedding app is its embedder. CI enforces via `scripts/lint/host_lint.py`
  (code suffixes only — Markdown gets a non-failing added-lines advisory and `.agents/**`
  is exempt, so vocabulary in docs is honor-system).
- ONED is run-only (ADR 0037). Do not add authoring workspaces, project/import
  state, preview runtimes, an asset database, or embedded MCP.
- Public-facing copy (README, release notes): name "JO and newer" titles (JO/DFX/DFX2),
  don't bundle pre-JO Delta Force titles; say pre-1.0/experimental, never
  "production-ready"; no em dashes.
- Completed TODO/checklist entries are DELETED, not checked off — the history lives in
  git, not the tracked file. (Exception: docs that self-identify as historical records,
  e.g. `plan/status.md`, keep their completed rows.)

## Git, PRs, CI

- Agent work happens in git worktrees under `.claude/worktrees/`; edit only inside the
  current worktree.
- Never post PR comments — context goes in the PR description and commit messages. Never
  merge PRs; the maintainer merges.
- PR CI builds only the `template_debug` GDExtension and packages debug-mode exports
  (`-ExportMode debug`); `template_release` + release-mode packaging run on master
  pushes/manual runs, so a release-flavour breakage surfaces after merge — build via
  `scripts/package_godot_windows.ps1` when touching `godot/src` build glue. The engine
  test job runs Ninja + sccache (the VS generator is local-only).

## Deeper docs

- [docs/current-state.md](docs/current-state.md) — start here for "what phase are we in
  and what is next": the maturity program is closed, work now runs as retail-fidelity
  slices off [docs/divergence-ledger.md](docs/divergence-ledger.md), and this page routes
  each domain to the record that names its next step. It is a router, not a priority list.
- [docs/maturity-program.md](docs/maturity-program.md) — the maturity program
  (pre-reimplementation rearchitecture), CLOSED 2026-07-12 with the freeze lifted:
  the dashboard, close-out dispositions, and the permanent enforcement instruments
  live there. ADRs 0015–0018 and 0022–0024 carry the standing rules every slice
  still builds under (two products/serve mode, engine layering, typed
  records, public-API testability, divergence burn-down, render parity, family
  topology).
- [.agents/README.md](.agents/README.md) — agent runbooks for networking work:
  architecture guardrails, retail interop, IDA witness rules, debugging, and task templates.
- [docs/README.md](docs/README.md) — documentation index: ADRs, RE records by domain.
- [docs/engine-primer.md](docs/engine-primer.md) — what the original engine is: binaries
  and IDBs, engine-wide conventions, subsystem index, and how to research it. Read it
  before engine work.
- [CONTEXT.md](CONTEXT.md) — the project glossary; use its canonical vocabulary.
- [docs/runtime-architecture.md](docs/runtime-architecture.md) — how a mission runs. Read
  it plus the ADRs before touching `mission_presentation.gd`,
  `game_frame_pipeline.gd`, the native present appliers
  (`godot/src/simulation/present_applier*.cpp`), or `Simulation`.
- [godot/modtools/README.md](godot/modtools/README.md) — ONED,
  retail staging, and hidden release pack command.
- Directory-scoped agent rules: `engine/CLAUDE.md`, `godot/src/CLAUDE.md`,
  `godot/modtools/CLAUDE.md`, `godot/tests/CLAUDE.md`.
- Project skills in `.claude/skills/`: `gut`, `oned-run`, `new-format-lib`, `re-doc`,
  `extract-pr`, `grill-ida`, `engine-research`, and `diagnosing-bugs`.
