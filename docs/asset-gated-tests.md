# Core and retail test suites

Ordinary CI runs the portable engine and Godot binding/presentation tests without
either retail root. Small authored definitions, generated models and menus, and
in-memory engine scenarios supply their inputs. This removes the private checkout
dependency; it does not make every test asset-free. File-format and Godot resource
tests still need small, public fixtures to exercise their actual APIs.

## Running the suites

```bash
scripts/build.sh --no-godot --suite core
scripts/test_godot.sh --suite core

# Both roots must point at complete retail data before either command runs.
scripts/build.sh --no-godot --suite retail
scripts/test_godot.sh --suite retail

# Local convenience: run both; missing retail data is reported as skipped.
scripts/build.sh --no-godot --suite all
scripts/test_godot.sh --suite all

# Separate graphics validation, on a Forward+ capable machine.
scripts/test_godot.sh --suite core --windowed     # godot/tests/windowed/, no retail data
scripts/test_godot.sh --suite retail --windowed   # godot/tests/retail/windowed/
```

`all` is the default for both runners. `core` unsets `OPENNOVA_JO_DIR` and
`OPENNOVA_JO_ASSETS` before launching tests, even when the developer has them set.
`retail` requires both directories and rejects every skipped test and `SKIP-LEG:`.
A green `all` run with missing data proves only the tests that executed. Windowed
graphics tests (`godot/tests/windowed/`, `godot/tests/retail/windowed/`) are excluded
from all headless selections, and a windowed run rejects every pending test: a
missing RenderingDevice is a failure, never a skip.

CTest's `retail` label is the native source of truth. List it with:

```bash
ctest --test-dir build -C Release -N -L '^retail$'
ctest --test-dir build -C Release -N -LE '^retail$'
```

Godot compatibility scripts live under `godot/tests/retail/`; graphics-only scripts
live under its `windowed/` subdirectory. Core graphics-only scripts live under
`godot/tests/windowed/`. All other GUT scripts are core.
`scripts/ci/test_suites.py` generates the exact GUT configuration and snapshots the
selected scripts and methods, or the actual CTest inventory, before execution.
The runners compare that inventory with JUnit afterward. Empty runs, missing
methods/tests, script parse errors, and dropped scripts fail. Core also rejects
retail-data skips; unrelated existing headless/renderer pending tests remain
visible in its report. CI checks that core scripts contain no retail resolver or
retail presenter-fixture calls.

Reports, inventories and Godot logs are written under `build/Testing/` using the
suite name (`ctest-core.xml`, `gut-retail.xml`, etc.) and uploaded by each CI job.

## Migration and coverage

At this migration there are **523 core and 88 retail CTest entries**. Thirty-six
formerly mixed entries now have a core invocation and a separately labelled
`<name>_retail` invocation. The latter passes `--retail`, enabling the original
corpus leg in the same executable; it also repeats the synthetic assertions.
The 52 wholly retail entries retain their original names. No native assertions
were removed. `scripts/ci/native_migration.json` records the 36 pairs and sources;
`scripts/ci/test_suites.py --check-layout` verifies each pair is still registered
and each source still exists.

The audit followed retail access paths from 260 Godot methods (259 headless and
one graphics-only). **146 had their retail inputs replaced**, and one already
authored player-info scenario stays in core after removing its helper's optional
retail-loading branch. Thus **147 audited methods belong to core**; the remaining
**112 headless methods and one windowed method** retain retail compatibility
coverage. Every audited method and its current home is recorded in
`scripts/ci/retail_migration.json`; the layout guard checks that these homes still
exist.

| Converted behavior | Methods | Replacement input and retained validation |
| --- | ---: | --- |
| Wire presentation | 43 | One authored weapon row plus the existing generated models and clips; node, pose and event assertions remain. |
| Local-player presentation | 23 | Authored action/weapon catalog; switching, reload, animation and loadout assertions remain. |
| Two-simulation networking | 21 | Authored weapons and ammo; real UDP transport, prediction, damage, impacts and replication assertions remain. |
| Menu shell | 13 | Minimal authored menus; navigation, selection, quit and shell state remain. |
| Armory menu and presenter | 18 | Authored catalog, menus and strings; filtering, weights, ammo, icons and commit/cancel remain. |
| Deploy and host-punt presentation | 10 | Authored death-screen controls and strings; real driver and transport assertions remain. |
| Throwable regressions | 8 | Authored action timings, ammo and motors; switching, charge, bounce and detonation assertions remain. |
| HUD and player weapon binding | 6 | Nonzero authored HUD fields and catalog; projection and null behavior remain. |
| Wire-header world materialization | 3 | Authored catalog plus existing generated missions/models; camera, culling and weapon selection remain. |
| Main-game lifecycle | 1 | Generated shell menus; pause and armory lifecycle remain. |
| Existing authored player-info ammo-type case | 1 | Keep the inline catalog in core; require the helper's explicit database argument. |

Portable logic continues to be tested under `tests/` against the engine's public
APIs. Godot tests validate the binding, scene, UI and device boundaries. The new
helpers in `godot/tests/support/` create small inputs through those same public
loaders; they do not copy or edit private retail files and do not fake the engine.

Remaining retail checks establish facts synthetic inputs cannot establish:
real parser/corpus compatibility and round trips; shipped weapon timings, menu
layout and localization; authored missions and mounted-vehicle transforms;
installed PFF/audio/model resolution; and actual retail animation/rendering
witnesses. For example, the armory's three shipped-menu/string checks and both
shipped smoke-fuse checks remain separate from their authored behavior tests.
Do not replace these with generated bytes and call that equivalent coverage.

## Local retail data

The only roots are `OPENNOVA_JO_DIR` (the packed JO:CA install and expansions)
and `OPENNOVA_JO_ASSETS` (the extracted tree plus reference fixtures). Native
tests resolve them through `tests/common/retail_paths.h`; Godot uses
`godot/tests/support/retail_data.gd`. The runner only checks their availability.
Machine paths belong in `.claude/settings.local.json` `env`, never tracked:

```json
{
  "env": {
    "OPENNOVA_JO_DIR": "C:/Games/Joint Operations Combined Arms",
    "OPENNOVA_JO_ASSETS": "C:/Assets/JOX"
  }
}
```

Export these variables when invoking the runners from a separate terminal.
The extracted root must include the `fixtures/` subtree from
`opennova-net/opennova-reference-assets`: shipped menus, definitions, string
tables and other interoperability witnesses. A plain JOX extract lacks that
subtree. The packed root must include the required expansion data. An incomplete
mount fails the retail suite instead of silently reducing its coverage.

## CI

The ordinary `test`, `test-linux`, `godot-tests`, and `godot-tests-linux` jobs use
`--suite core` on every PR, master push and manual run, and never mount the
private repositories. Linux builds the native executables and a GDExtension
`.so`; its headless Godot job consumes the Linux `template_debug` artifact.
Both platforms also build `template_release` on master pushes and manual runs.
The separate `test-retail` and
`godot-tests-retail` jobs run on the same PR, master-push and manual triggers
when reference-data credentials are available. Retail compatibility runs on
Windows; the Windows Godot jobs consume the same `template_debug` DLL artifact.
Windowed graphics validation (core and retail) is a separate
local run; hosted headless CI does not establish its pixel/instance-row coverage.

`retail-availability` explicitly reports unavailable credentials, such as on a
fork PR, and only the retail jobs skip. With `REFERENCE_ASSETS_TOKEN` available,
the composite `mount-reference-data` action mounts these two private repositories:

- `opennova-net/opennova-reference-assets`: the extracted assets and reference
  fixture set behind `OPENNOVA_JO_ASSETS`.
- `opennova-net/opennova-reference-retail-packed`: the packed JO:CA install behind
  `OPENNOVA_JO_DIR`, including the expansion; `reassemble.sh` verifies the archive
  parts against `MANIFEST.sha256`.

The token needs read access to both repositories. The existing action caches
each root by its repository commit. A mount failure or skipped compatibility
case fails the retail job. There is no duplicated expected-test allowlist:
CTest labels, the Godot directory layout and the collected method inventory
define what must run. `.github/workflows/ci.yml` is the CI record.

## Why captures are never committed

Beyond the blanket rule for retail data, **NovaWorld traffic can embed the account
username and password** (the gate/auth exchanges), and `.sph` recordings carry
account identity and machine paths. Treat every capture as credential-bearing
until proven otherwise. The tracked-fixture line is drawn in `.agents/interop.md`:
promote only small *sanitized* artifacts (`.nwmsg`, focused `.hexcap`, `.gsb`,
manifest rows) — `fixtures/novaworld/` shows the shape. Raw `.pcapng`/`.sph` stay
machine-local and gitignored, full stop.

## The two-tier wire-compat gate (maturity program NET-0)

CI can never run a live retail session, so a net-touching change could look
green while silently altering wire bytes. The maturity program
(docs/maturity-program.md) closed that with two tiers (the original tier-2
retail golden diff retired with the capture root, 2026-08-29 — the live
harness below replaced it):

- **Tier 1 — default CI, cannot skip.** `nw_codec_identity` pins the EXACT bytes
  of every in-match encoder against committed FNV-1a64 vectors over a synthetic
  corpus (our bytes only, so it is committable and unconditional; it also runs in
  the Linux net job). It exists because encode↔decode round-trips cannot see a
  SYMMETRIC codec change — both sides edited together stay field-identical while
  the wire moves. Updating a vector is a wire-format change: it requires the
  [orig] witness or a D-NET entry in the same commit, never a bare regeneration
  (`nw_codec_identity_test --dump` prints the replacement table). The second
  tier-1 leg (NET-0b) is `nw_self_capture` — the opennova-vs-opennova coverage diff: a
  deterministic in-process opennova↔opennova join + play session is captured
  live and coverage-diffed per (direction, tag) against the committed
  opennova-produced fixture `fixtures/novaworld/self-capture-session.pcap` (LFS;
  zero retail bytes, so committable). Because both sides are ours the comparison
  is EXACT set equality in both directions — no noise floor, no allowlists — and
  a missing fixture FAILS rather than skips. Regenerating the fixture
  (`nw_self_capture_test --write-fixture`, which re-reads and re-verifies the
  file) is a wire-coverage change: justify the tag delta in the same commit.
- **Tier 2 — local, mandatory protocol for net-touching PRs.** Run the live
  retail interop harness (`scripts/net/README.md`: an OpenNova host with a
  retail client, and a retail host with an OpenNova joiner, decoded with
  `nw_pp --coverage`), and **attest the run in the PR description** (the
  commands + the decode summary). A net-touching PR without the attestation
  is not reviewable. This is the standing substitute for the retail sessions
  CI cannot run.
