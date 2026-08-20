# Registered render-parity runbook

Use this runbook to refresh the T3 headline evidence for
[ADR 0023](../adr/0023-render-visual-parity.md): the registered retail/OpenNova
side-by-sides published under
`screenshots/parity/render-lighting-2026-08/registered-<date>/`. It is the
operator procedure behind the contract described in
[README.md](README.md); that page stays the record index and states what the
evidence must prove, this page states how to produce it.

Two legs meet at a single frozen OpenNova commit:

1. **OpenNova** — `godot/tests/render_fixture_capture_probe.tscn` renders each
   catalog fixture at 2000x1200 in `hud_hidden` mode and emits five diagnostic
   variants plus a provenance manifest.
2. **Retail** — an onHook-captured pre-HUD backbuffer snapshot of the same pose,
   registered by `scripts/render/register_retail_capture.py`.

`scripts/render/build_retail_side_by_side.py` refuses to pair them unless the
registration's `build.opennova_source_commit` equals the OpenNova manifest's
source commit, so **every refresh re-registers retail**, even when the retail
pixels are unchanged.

## Pinned parameters

Machine-specific paths belong in `.claude/settings.local.json` `env`, never in
tracked files. The values below are the authoritative capture machine's.

| Parameter | Value |
|---|---|
| Publication catalog | `docs/render/render-fixtures-retail-v2.json` |
| Catalog SHA-256 | `607c7d66d7ce35ac915264fd462565fc262683aed34e4585a48d9093ce1a36d8` |
| Fixtures | 15 (`00TRa` x5, `CP01` x6, `CP04` x1, `CP12` x3) |
| OpenNova capture size | 2000x1200, vertical FOV 53.4468 |
| Retail backbuffer | 1920x1200 (one horizontal bicubic squeeze at comparison time) |
| Loose mission root | `NOVA_MISSION_RESOURCE_DIR`, the loose `.bms` corpus |
| Packed runtime root | `NOVA_RUNTIME_RESOURCE_DIR`, **the same install retail captures from** |
| Expansion, both engines | `revx02` (`NOVA_EXPANSION` and retail's `/exp`) |
| Retail video profile | `retail_reference_highest_retail_selectable_v2` |
| Original `game.cfg` | `556880e9ec85d60021f2ce17d584bde30702a6ae3ecfba6a1e6eb54402c8cad3` |
| Staged `game.cfg` | `e7bd7d27d6dcb22c8b58e3daa6ef543e2489f0600ffee28ffcb9a53d79806ed3` |
| Approved `weapon.sav` | `f4907820a58505a6988f140a27638dae7dbaaea2cc446642f0bc44c868905623` |

Binaries that registration hashes into every record. Keep them byte-stable
across a refresh or the identity rows churn for no reason:

| Binary | SHA-256 |
|---|---|
| `Jointops.exe` | `b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac` |
| onHook proxy `binkw32.dll` | `928905bffc39d2880dc027f1ae09c3fdbab328c939ae724ff05dab32511708b7` |
| onHook forwarder `binkw32_.dll` | `d118512ff119b85e4ba98d8623ff2cc91d426cf4975838f056a7cd87f9979af9` |
| onHook MCP `onhook-mcp.exe` | `e0306c402c21e47e7d87e2344958297634ee74fdce9569923112fc26f6cd8b70` |
| Godot editor binary | `.godot-bin/Godot_v4.6.1-stable_win64.exe` |

The onHook build lives in the separate `opennova-int` repository, never in this
one. Retail install paths and the retained bundle root are local state.

## 0. Decide the retail path before anything else

**Re-register retained bundles (the default).** Retail pixels do not change when
our source changes. If a previous run's raw bundles are on disk, a refresh needs
no game launch at all: re-stage the config, re-register the same bundles against
the new commit, restore. Precedent: the 2026-08 publication's per-fixture
`registered.json.c067.bak` is a registration of the same bundles at an earlier
OpenNova commit.

A retained bundle is one directory per fixture holding all seven artifacts:

```text
retail.png              retail.state.json       instance-status.json
fixture-result.json     capture-result.json     onhook.log
retail-stage.json
```

**A retained bundle is pinned to the absolute path it was captured at.** Its
`capture-result.json` records the absolute `path` and `state_path` onHook wrote,
and registration fail-closes unless `--raw-image` and `--raw-state` resolve to
exactly those, with `--output` inside the same directory
(`capture result does not bind the selected raw pair`, then
`raw retail image must be colocated under the registered bundle directory`).
A copy elsewhere is not registerable. Re-registration therefore happens **in
place**, in the directory the bundle was originally captured into.

That makes the capture destination a long-lived decision: **write raw bundles to
a durable local evidence root, never into a worktree's `.scratch`.** A worktree
is disposable; a bundle captured inside one can only ever be re-registered from
inside that same worktree, for as long as it survives.

**Re-shoot retail** when the catalog poses, the video profile contract, the
capture-mode contract, onHook, or the retail install change -- or when the only
retained bundles are stranded in a worktree you are not willing to write into.
That path needs the onHook MCP server registered with your MCP client and the
client restarted before any capture can run; see section 3b.

## 1. Worktree preflight

Agent work happens in a git worktree under `.claude/worktrees/`. Run build and
Godot commands outside the sandbox.

```bash
git submodule update --init --recursive     # third_party/ ships empty
bash scripts/build_godot.sh                 # -> godot/bin/libopennova.*.dll
main="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")"
export GODOT_BIN="$main/.godot-bin/Godot_v4.6.1-stable_win64.exe"
"$GODOT_BIN" --headless --path godot --import
```

Use the non-console `Godot_v4.6.1-stable_win64.exe`: the manifest hashes the
exact executable, and switching builds changes an identity row for no reason.
Worktrees have no `.godot-bin/` of their own, so point at the main checkout's.

## 2. Freeze the source, then capture OpenNova

Land every code and doc change for the slice FIRST and commit. The manifest
binds a clean tree, and rebuilding the GDExtension after capture invalidates it.

```powershell
$sourceCommit = (git rev-parse HEAD).Trim()
if (git status --porcelain) { throw "capture worktree is not source-frozen" }

$env:NOVA_EVIDENCE_SOURCE_COMMIT = $sourceCommit
$env:NOVA_GDEXTENSION_BINARY     = (Resolve-Path "godot\bin\libopennova.windows.template_debug.x86_64.dll").Path
$env:NOVA_RENDER_FIXTURE_CATALOG = "res://../docs/render/render-fixtures-retail-v2.json"
$env:NOVA_RENDER_CAPTURE_MODE    = "hud_hidden"
$env:NOVA_MISSION_RESOURCE_DIR   = "<loose .bms corpus>"
$env:NOVA_RUNTIME_RESOURCE_DIR   = "<retail install>"
$env:NOVA_EXPANSION              = "revx02"

$env:NOVA_RENDER_FIXTURE_ID     = "00tra-courtyard-retail"
$env:NOVA_RENDER_FIXTURE_OUTPUT = (Join-Path (Resolve-Path ".").Path ".scratch\golden\render\fixtures\$env:NOVA_RENDER_FIXTURE_ID")
& $env:GODOT_BIN --path godot --resolution 2000x1200 res://tests/render_fixture_capture_probe.tscn
```

Rules the probe enforces, so do not fight them:

- Both engines mount the **same install and the same expansion**.
  `NOVA_RUNTIME_RESOURCE_DIR` is the retail game directory retail itself is
  captured from, and `NOVA_EXPANSION` is `revx02`. The probe's default `jox01`
  belongs to the `world_only` diagnostic catalog and a different install; using
  it here silently pairs OpenNova revx02-less content against a revx02 retail
  frame. The manifest's `mission.expansion` is the receipt -- check it.
- Run the `.tscn` positionally. Never pass the `.gd` to `-s`, never add
  `--headless` — the capture needs a real window.
- Run captures in the **foreground of an interactive desktop session**. A
  detached or background shell has no usable desktop, so the probe takes its
  `<fixture-id>.publication-transaction` lock and aborts having written only
  `owner.json`. A sweep launched that way fails every fixture with no
  explanation; the leftover transaction directories are the signature. Delete
  them before retrying.
- Do not judge success from stdout. Under PowerShell 5.1 a `2>&1` redirect of
  the Godot executable wraps its stderr in `NativeCommandError` records, and the
  non-console binary may deliver nothing to a redirected pipe at all. Check the
  output directory instead: a good run leaves exactly 11 files.
- `NOVA_RENDER_FIXTURE_OUTPUT` must be a strict descendant of this repository's
  `.scratch`.
- Omit `NOVA_RENDER_FIXTURE_MINUTE`: every retail fixture declares exactly one
  `minutes_of_day`.
- Each invocation emits five PNGs (`beauty`, `shadows_off`, `lighting_only`,
  `unshaded`, `directional_shadow_atlas`), five `.state.json` sidecars, and one
  `<fixture-id>-manifest.json`.

**Smoke exactly one fixture before sweeping all fifteen.** The probe fail-closes
on the matched-presentation runtime witness: `WPN_M16BURST` with 30 loaded and
270 in reserve, character `0x0402`, bare `IndoArms.3di` arms with camo
`[1,0,0]`, `gameplay_hud_visible=false` while the HUD `CanvasLayer` stays active
and visible, live player-view effects and viewmodel, terrain available and
visible, hipfire with no ADS, no big map, and the catalog pose. Armory,
customization or viewmodel drift on the branch shows up here first.

Then loop the remaining fourteen fixture ids from the catalog.

### The probe's environment contract

Only the path values are installation-specific. The probe rejects a
catalog/driver mismatch, a mission hash mismatch, a missing production gameplay
camera, a viewport-size mismatch, a source/build provenance failure, a
capture-mode mismatch, or any drift from the requested integer minute.

| Variable | Requirement |
|---|---|
| `GODOT_BIN` | A Godot 4.6.1 binary capable of windowed rendering. This is the shell launcher; the probe does not read it. |
| `NOVA_RENDER_FIXTURE_ID` | **Required.** One exact `id` from the catalog. |
| `NOVA_MISSION_RESOURCE_DIR` | **Required for reproducible evidence.** A loose-mission root containing the catalog mission as a loose `.bms`. The probe has a persisted-setting fallback for interactive convenience; evidence runs set it explicitly. |
| `NOVA_RUNTIME_RESOURCE_DIR` | **Required.** A valid packed runtime resource root. |
| `NOVA_EXPANSION` | Optional; defaults to `jox01`. **Registered captures must set `revx02`** so both engines mount the expansion retail runs. |
| `NOVA_RENDER_FIXTURE_CATALOG` | Optional; defaults to `res://../docs/render/render-fixtures-v1.json`. An override is held to the same schema, resolution, mission-hash and variant-order checks. |
| `NOVA_RENDER_FIXTURE_OUTPUT` | Optional; defaults to `res://../.scratch/golden/render/fixtures/<fixture-id>`. An override must be a dedicated strict descendant of this repository's `.scratch`; the probe rejects `.scratch` itself, paths outside it, and any linked/reparse ancestor. |
| `NOVA_RENDER_FIXTURE_MINUTE` | Optional strict integer selector; must be one of the fixture's declared `minutes_of_day`. Empty captures all declared minutes. `720.0` and undeclared minutes are rejected. |
| `NOVA_RENDER_CAPTURE_MODE` | Optional verifier. If set it must equal the fixture's declared `world_only`, `full_frame` or `hud_hidden` mode; it cannot relabel a fixture. |
| `NOVA_EVIDENCE_SOURCE_COMMIT` | **Required.** The lowercase full 40-character commit that produced the capture. Do not identify a dirty or subsequently rebuilt tree with its current `HEAD`. |
| `NOVA_GDEXTENSION_BINARY` | **Required.** Absolute path to the exact GDExtension binary this Godot build loaded. The manifest hashes it together with the Godot executable. |

Fixture anchors record both serialized and runtime identity. The nested
`entity.bms_record` distinguishes the BMS `write_order_index` from the
`mission_kind` pool (`item`, `building`, `marker`, `organic`) and its
kind-local `kind_index`; `bms_id` remains the stable authored identity check.
Do not read the write-order index as an item-pool index.

### The `world_only` diagnostic catalog

`docs/render/render-fixtures-v1.json` drives the same probe for within-run
subsystem isolation rather than cross-engine comparison: 1600x900, vertical FOV
50.534, `world_only`. Compare its variants with
`scripts/render/build_render_comparison.ps1 -ComparisonMode subsystem-ab`. That
workflow is independent and is never a substitute for a registered HUD-hidden
retail comparison. The legacy `retail-parity` mode of that script and the
free-form side-by-side script are not sanctioned for cross-engine evidence.

## 3a. Retail: re-register retained bundles

Retail must not be running. `stage_retail_presentation.py` refuses otherwise.

Copy the retained bundles into this worktree's own `.scratch` before touching
them — never write into another worktree.

```powershell
uv run python scripts/render/stage_retail_presentation.py stage `
  --game-cfg "<game dir>\game.cfg" `
  --weapon-sav "<game dir>\expansion\revx02\weapon.sav" `
  --backup <create-new local path outside the repo> `
  --manifest .scratch\retail\retail-presentation-stage.json
```

Staging changes exactly one key, `object_texdetail` 1 -> 3, and preserves
`hud_detail`, adapter identity, and tip preferences. Confirm the live `game.cfg`
now hashes the staged value from the pinned table; registration reads the LIVE
`game.cfg` and `weapon.sav` and fails closed if they disagree with the stage
record.

Register each fixture against the retained artifacts and the retained
per-fixture `retail-stage.json`:

```powershell
uv run python scripts/render/register_retail_capture.py `
  --catalog docs/render/render-fixtures-retail-v2.json `
  --fixture-id <fixture-id> `
  --raw-state       .scratch\retail\raw\<fixture-id>\retail.state.json `
  --raw-image       .scratch\retail\raw\<fixture-id>\retail.png `
  --instance-status .scratch\retail\raw\<fixture-id>\instance-status.json `
  --fixture-result  .scratch\retail\raw\<fixture-id>\fixture-result.json `
  --capture-result  .scratch\retail\raw\<fixture-id>\capture-result.json `
  --onhook-log      .scratch\retail\raw\<fixture-id>\onhook.log `
  --game-dir "<game dir>" --expansion revx02 `
  --retail-executable "<game dir>\Jointops.exe" `
  --onhook-mcp <path to the onhook-mcp.exe that produced the bundle> `
  --onhook-proxy "<game dir>\binkw32.dll" `
  --onhook-forwarder "<game dir>\binkw32_.dll" `
  --opennova-source-commit $sourceCommit `
  --retail-stage-manifest .scratch\retail\raw\<fixture-id>\retail-stage.json `
  --confirm-retail-presentation-contract `
  --output .scratch\retail\raw\<fixture-id>\registered.json
```

Write `registered.json` **into the bundle directory**. Its
`capture.image_path` and `raw_evidence.retail_stage_manifest_name` are resolved
relative to the record's own directory, so a registration parked elsewhere
cannot find its own frame.

Pass the same `onhook-mcp.exe` that produced the bundle. Its hash is recorded as
build provenance; substituting a different build silently relabels the evidence.

Restore immediately after the last registration, and verify the original hash:

```powershell
uv run python scripts/render/stage_retail_presentation.py restore `
  --game-cfg "<game dir>\game.cfg" `
  --backup <backup path> `
  --manifest .scratch\retail\retail-presentation-stage.json
```

Never commit the backup, the absolute-path restore token, or the restore
receipt.

## 3b. Retail: a fresh capture (only when the contract changed)

Everything in 3a applies, plus the capture itself. Register the onHook MCP with
your MCP client and restart it first; a server added mid-session is not
discoverable. Register it at user scope with an absolute path to the build you
intend to use -- never in the tracked `.mcp.json`, which must stay free of
machine-specific paths.

Capture into a **durable evidence root outside every worktree**, one directory
per fixture. The absolute path you choose is baked into `capture-result.json`
and is the only place those bundles can ever be re-registered from (section 0).
Then, per fixture:

1. Launch `<game dir>\Jointops.exe /exp revx02 /w` through the owned mission
   session and retain the exact `instance_id` and PID.
2. Save `onhook_instances()` and require that same instance to report proxy
   mode, `capture_bundle_supported=true`, `render_fixture_supported=true`, a
   ready 1920x1200 backbuffer, and available render state.
3. If the deployment or control-point overlay is up, activate the window owned
   by that exact PID and press SPACE once. Never target a window chosen by
   executable name or by whichever retail window is foreground.
4. `onhook_apply_render_fixture` for that instance with the catalog
   `retail_player_bms.applied`, yaw, pitch, `vertical_fov_degrees`, the single
   `minutes_of_day` times 60 as `time_of_day_seconds`, the exact `mission_file`
   and catalog `mission_sha256`, and `camera_mode: "first_person"`. Require
   `exact=true`. Every field is mandatory on every call; nothing inherits from a
   prior fixture.
5. `onhook_capture_retail_reference({instance_id, path, fixture_id})`
   immediately. Save its PNG, `.state.json` and same-process log.

`cp01-waterline-below-retail` needs an isolated fresh process: dismiss
deployment for that PID, apply the underwater pose, and capture before breath
expiry. Do not reuse a surfaced or damaged process.

## 4. Build the comparisons and publish

```powershell
uv run python scripts/render/build_retail_side_by_side.py `
  --catalog docs/render/render-fixtures-retail-v2.json `
  --fixture-id <fixture-id> `
  --opennova-manifest .scratch\golden\render\fixtures\<fixture-id>\<fixture-id>-manifest.json `
  --retail-bundle .scratch\retail\raw\<fixture-id>\registered.json `
  --output-dir .scratch\publication\<fixture-id> `
  --opennova-caption "HUD hidden - bare arms - M16 Burst 30/270 - terrain" `
  --retail-caption "Pre-HUD snapshot - bare arms - M16 Burst - terrain - frame-correlated" `
  --roi world_center=240,180,1320,420 `
  --roi viewmodel_arms=850,700,900,500
```

Both ROIs are required by the production catalog and are descriptive regions,
not pixel-parity gates. The builder performs exactly one full-source
HighQualityBicubic 2000 -> 1920 horizontal normalization: no crop, no
translation, no vertical scale, no second normalization.

Then assemble the publication and generate its index:

```powershell
uv run python scripts/render/publish_registered_comparisons.py `
  --catalog docs/render/render-fixtures-retail-v2.json `
  --opennova-root .scratch\golden\render\fixtures `
  --retail-root .scratch\retail\raw `
  --comparison-root .scratch\publication `
  --output screenshots\parity\render-lighting-2026-08\registered-<date>
```

It copies only the publishable subset, verifies every copied byte against the
hash its own manifest declares, refuses a publication that mixes source
commits, catalogs or tool versions, and derives the README's identity table,
inventory line and metric table from the records themselves.

The publication layout, one directory per fixture:

```text
<fixture-id>/retail/      retail.png, registered.json, retail-stage.json
<fixture-id>/opennova/    5 variant PNGs + 5 .state.json + manifest.json
<fixture-id>/comparison/  side-by-side, overlay-50, absolute-diff,
                          opennova-normalized, comparison.json
README.md                 identity table + per-fixture metric table
```

`.gitattributes` routes `screenshots/**` to LFS but pulls
`screenshots/**/*.md` and `screenshots/**/*.json` back out as plain text. Do not
add a nested `.gitattributes` and do not let the manifests become LFS pointer
stubs.

Delete the superseded publication directory in the same commit. History keeps
it; two directories both claiming to be current is the failure mode this
replaced.

## 5. Repoint everything that names the publication

- `tests/test_retail_render_evidence.py` — `PUBLISHED_EVIDENCE_ROOT` and
  `CURRENT_SOURCE_COMMIT`.
- `docs/render/README.md` — publication path, frozen source commit, Godot and
  GDExtension hashes, and the descriptive MAE/RMS spans.
- `docs/render/render-lighting-parity-2026-08-15.md` — the dated session
  record's publication contract section.
- `docs/divergence-ledger.md` — only if a row's visual status genuinely moved.
  Regenerate the scoreboard with `python scripts/lint/ledger_check.py --write`
  when one does.

## 6. Verify

```bash
uv run --frozen pytest tests/test_retail_render_evidence.py -v
uv run --frozen pytest tests/test_retail_capture_registration.py tests/test_render_comparison_tool.py
ctest --test-dir build -C Release -R "renderer|render"
git check-attr filter -- screenshots/parity/render-lighting-2026-08/registered-<date>/*/retail/retail.png
```

`test_retail_render_evidence.py` skips as pass when the checkout holds LFS
pointer stubs, so confirm the run actually exercised the publication-pinned
tests rather than skipping the whole module.

The gate itself is the by-eye retail pass over each of the fifteen sheets,
recorded scene by scene in the slice PR (ADR 0023 section 4). MAE and RMS are
descriptive measurements and never a threshold.

## Failure triage

| Symptom | Cause and fix |
|---|---|
| Probe aborts on the runtime witness | Armory/customization/viewmodel drift changed the matched presentation. Fix the seam or ledger the divergence; do not relax the contract. |
| `capture worktree is not source-frozen` | Uncommitted files. Commit or stash, then recapture; the manifest binds a clean tree. |
| Parse errors naming `Nova*` classes | Missing or stale GDExtension. Rebuild with `scripts/build_godot.sh` and fully restart Godot; registration does not hot-reload and a running editor holds the DLL lock. |
| `live game.cfg hash does not match the staged effective config` | Retail is unstaged, or restored mid-run. Re-stage before registering. |
| `OpenNova build does not match retail pairing bundle` | The registration and the OpenNova manifest name different source commits. Re-register at the frozen commit. |
| Every fixture in a sweep fails, leaving only `<id>.publication-transaction/owner.json` | The captures ran without an interactive desktop. Re-run them in the foreground and delete the stale transaction directories first. |
| `capture result does not bind the selected raw pair` | The raw bundle was moved. Register it at the absolute path `capture-result.json` records, or re-shoot retail. |
| `raw retail image must be colocated under the registered bundle directory` | `--output` is outside the bundle directory. Write `registered.json` beside `retail.png`. |
| Sheets pair but content differs structurally | Check `mission.expansion` in the OpenNova manifest against retail's `/exp`. A `jox01` OpenNova frame against a `revx02` retail frame is not a comparison. |
| Registration rejects the mission | The loose `.bms` under `NOVA_MISSION_RESOURCE_DIR` does not hash to the catalog value. Repoint at the correct corpus. |
| Every command in the shell is refused | The persistent shell was `cd`-ed into the shared checkout. Re-enter the current worktree to unwedge it. |
| Deep-water pose drifts before capture | The player sinks then buoys. Re-apply the pose and capture within about 50 ms. |
| AAS mission sits on a control-point overlay | Focus the window owned by the exact PID and press SPACE once before posing. `00TRa` (training) has no overlay. |

## Never

- Never publish a capture from a dirty or rebuilt tree.
- Never widen a tolerance or relax the presentation contract to make a pair
  agree; a divergence is a tracked ledger row.
- Never commit raw retail captures, the config backup, the restore token, or any
  absolute caller path.
- Never leave retail staged. Restore before the session ends.
