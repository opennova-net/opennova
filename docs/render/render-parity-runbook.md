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
| Publication catalog | `docs/render/render-fixtures-retail-v3.json` |
| Catalog SHA-256 | `ec69de10bf9477554c40f51ce4d507df629ca86e76990e3ce152c3c5dcc6a62c` |
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

Registration hashes four binaries into every record. Only two are stable:

| Binary | SHA-256 |
|---|---|
| `Jointops.exe` | `b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac` |
| onHook forwarder `binkw32_.dll` | `d118512ff119b85e4ba98d8623ff2cc91d426cf4975838f056a7cd87f9979af9` |
| Godot editor binary | `.godot-bin/Godot_v4.6.1-stable_win64.exe` |

The forwarder is the original Bink DLL under a new name and never changes. The
onHook proxy `binkw32.dll` and `onhook-mcp.exe` hash to whatever the deployed
build produces; each publication records its own pair, so read them back from a
registration rather than pinning them here.

The onHook build lives in the separate `opennova-int` repository, never in this
one. Retail install paths and the raw bundle root are local state.

### Building and deploying onHook

The proxy and the MCP are one build and must be deployed as a pair. From the
`opennova-int` checkout, with the msys2 i686 toolchain:

```bash
MSYSTEM=MINGW32 /c/msys64/usr/bin/bash -lc   "cd /c/.../opennova-int/onhook && ./build_onhook.sh --debug --proxy"
```

`--debug` is what produces `onhook-mcp.exe` at all; release builds omit it
deliberately. `--proxy` builds `binkw32.dll` rather than the injectable
`onhook.dll`. A plain `bash -lc` resets PATH and the configure step fails with
`no acceptable C compiler found`; `MSYSTEM=MINGW32` is what puts
`i686-w64-mingw32-gcc` on it.

Back up the installed proxy, copy the new `binkw32.dll` into the retail game
directory, and leave `binkw32_.dll` alone. Then register the MCP at **user
scope** and restart the client:

```
claude mcp add onhook --scope user -- <abs path>/opennova-int/onhook/onhook-mcp.exe
```

Never put that path in the tracked `.mcp.json`. Confirm with `claude mcp list`
that it reports Connected with its tools loaded, not just Connected -- a tool
fetch failure there means the capture tools are unavailable no matter how the
handshake looks.

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
A re-shoot always mints a new catalog revision and recaptures the OpenNova leg
against it (the constraint table in section 3b explains why); the whole path is
scripted -- `retail_capture_driver.py` then `mint_retail_catalog.py` (section
3b).

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
$env:NOVA_RENDER_FIXTURE_CATALOG = "res://../docs/render/render-fixtures-retail-v3.json"
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
  --catalog docs/render/render-fixtures-retail-v3.json `
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

**Read this before re-shooting: a re-shoot forces a new catalog revision.**
The catalog constrains both ends of the retail leg, and they are only
simultaneously satisfiable by the session that minted them:

| Check | Where | Tolerance |
|---|---|---|
| capture frame follows fixture application | `register_retail_capture.py` | <= 120 frames |
| applied player position == `retail_player_bms.applied` | `register_retail_capture.py` | 1e-5 |
| registered camera == `camera_bms` | `build_retail_side_by_side.py` | 0.05 per axis |

`onhook_apply_render_fixture` cannot set a camera, only the player pose; retail
derives the camera from it. So hitting `camera_bms` means correcting the applied
pose, and a corrected pose fails the 1e-5 applied-position check. Measured on
2026-08-20 at the catalog's own applied positions, the resulting camera missed
`camera_bms` by 0.11-0.99 on land, and the CP01 water rows put the camera 2.7 m
BELOW the player.

The honest resolution is to mint a new catalog revision: capture with
`--no-correct` so the applied positions stay verbatim, record the cameras
retail actually produced as the new `camera_bms`, and publish that pair. That
changes the catalog SHA, and every OpenNova manifest binds it, so the OpenNova
leg must be recaptured against the new revision too. Budget both legs.

Build, deploy and register onHook first (see "Building and deploying onHook"
above); a stale build may simply not expose `onhook_capture_retail_reference`
-- check the tool list, not the file date. Retail must be staged (section 3a's
stage command) before any capture.

**Capture.** `scripts/render/retail_capture_driver.py` does the whole retail
leg: it issues the fixture apply and the reference capture back-to-back on one
MCP connection (3-5 frames apart, against the registrar's 120-frame limit),
solves the camera from each frame's own view matrix, and writes the six bundle
sidecars. Run it once per mission, with `--fresh-process` so every fixture gets
its own retail process -- a player already in water is pinned by float/settle
physics and only the first teleport of a process lands verbatim, and even land
fixtures drift in an aged process (measured 0.11-0.99 vs 0.003-0.13 fresh).
Capture into a **durable evidence root outside every worktree**; the absolute
path is baked into `capture-result.json` and is the only place the bundles can
ever be re-registered from (section 0). The `--run-root` must be new per sweep
-- the hook log is create-new.

```powershell
uv run python scripts/render/retail_capture_driver.py --no-correct `
  --fresh-process CP01.bms `
  --catalog docs/render/render-fixtures-retail-v3.json `
  --output C:\evidence\retail-<date> `
  --run-root C:\evidence\retail-<date>\_runs `
  --onhook-mcp <opennova-int>\onhook\onhook-mcp.exe `
  --game-dir "<game dir>" `
  --stage-manifest .scratch\retail\stage\retail-presentation-stage.json `
  cp01-water-wide-retail cp01-water-shallow-retail cp01-water-steep-retail
```

(The catalog named here is the revision being superseded -- its applied fields
are what the captures replay verbatim; only `camera_bms` changes in the mint.)

**Mint.** `scripts/render/mint_retail_catalog.py` turns those bundles into the
next catalog revision: applied fields kept verbatim (verified against each
bundle's `fixture-result.json` at the registrar's own 1e-5), `camera_bms`
replaced with each frame's solved camera, canonical `catalog_sha256`
recomputed:

```powershell
uv run python scripts/render/mint_retail_catalog.py `
  --base docs/render/render-fixtures-retail-v3.json `
  --bundles C:\evidence\retail-<date> `
  --output docs/render/render-fixtures-retail-v4.json
```

Smoke-register ONE bundle against the minted file (any well-formed 40-hex
source commit; delete the throwaway `registered.json` after) to prove the
registrar's recovered camera lands, then commit the new catalog -- it must be
in the frozen source commit before the OpenNova leg runs (section 2). From
there the flow rejoins section 3a's registration loop, pointed at the new
catalog and the new bundles.

## 4. Build the comparisons and publish

```powershell
uv run python scripts/render/build_retail_side_by_side.py `
  --catalog docs/render/render-fixtures-retail-v3.json `
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
  --catalog docs/render/render-fixtures-retail-v3.json `
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
| `claude mcp list` says Connected but tools fetch failed | The server is up but its tool list was rejected; the capture tools are unavailable. Fix the server, do not proceed. |
| `onhook_capture_retail_reference` missing from the tool list | The deployed onHook predates the frame-correlated capture tooling. `onhook_capture_bundle` is not a substitute -- registration needs the v4 presentation proof. Rebuild. |
| `fixture application position does not match catalog` | The applied pose was corrected to chase the camera. Registration pins it at 1e-5; recapture with `--no-correct` and mint a catalog revision. |
| `retail capture is more than 120 frames after fixture application` | Apply and capture were issued separately. Use `retail_capture_driver.py`, which pairs them on one connection. |
| `onhook_host_lan` times out with an empty log | The run's `output_dir` was reused. The hook log is create-new; give every run its own directory. |
| Water frames come out submerged | The player is pinned in a swim state. Capture that fixture in a dedicated process (`--fresh-process`). |
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
