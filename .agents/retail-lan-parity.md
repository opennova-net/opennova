# Retail LAN parity runbook

Use this runbook to reproduce and compare the four Joint Operations LAN paths:

1. retail host -> retail joiner (the ground-truth oracle);
2. retail host -> OpenNova joiner;
3. OpenNova host -> retail joiner;
4. OpenNova host -> OpenNova joiner.

The mission, game type, expansion, resource corpus, port, callsigns, 1920x1080
viewport, and capture window must stay fixed across one comparison set. The
maps/game types must vary across sets: run every topology A-D for the baseline
cases and retain the CP04/03TR coverage cases that exposed state/map-dependent
traffic:

| case | mission | numeric `g_GameType` | readiness mode | distinct wire surface |
|---|---|---:|---|---|
| waypoint control | `01TR.bms` | `65568` (`0x10020`) | `in_match` | confirmed waypoint-family baseline |
| TDM | `TDH_I3A.bms` | `65536` (`0x10000`) | `in_match` | team mode without A&S zone flow |
| Co-op deploy hold | `CP04.bms` | `196640` (`0x30020`) | `deploy_hold` | confirmed integrity-during-deployment control |
| Deathmatch / dense training | `03TR.bms` | `0` (`0x00000`) | `in_match` | non-team, non-objective branch plus a different entity/map/loadout surface |
| 01TR TDM holdout | `01TR.bms` | `65536` (`0x10000`) | `in_match` | repeats the 01TR bytes with TDH's game type on rotated/reused port 32788 |
| TDH waypoint holdout | `TDH_I3A.bms` | `65568` (`0x10020`) | `in_match` | repeats the TDH bytes with 01TR's game type on rotated/reused port 32789 |

The first four are the mandatory canonical subset. The two holdouts deliberately
cross mission and game-type identities: together the six cases prevent a
mission-name lookup, game-type lookup, or fixed case-to-port table from looking
wire-compatible by accident. Every case still runs all four topologies, for 24
verdict-bearing cells.

`auto` is useful for discovery, but a parity set is accepted only after the
resolved value is recorded and pinned numerically on all host launches. Zero
is retail's valid Deathmatch value, not an unpinned sentinel. The
launcher cannot force retail's menu loadout, so record the observed equipped
ADM -> AmmoDef row in each endpoint trace and compare only like-for-like rows.
The examples below show the confirmed `01TR.bms`, `revx02`, UDP 32786,
four-player control case.

The current onHook pair automation discovers only UDP `32786..32789`. The
canonical subset must cover all four. Runs are sequential, so additional
holdouts may reuse a supported port; the tracked holdouts rotate onto 32788 and
32789 instead of preserving a one-case/one-port mapping. Ports outside that
automation window fail before a retail join can produce evidence. This is
narrower than the runtime's general configurable LAN-port range.

Raw captures, retail files, hook logs, screenshots, account data, local paths,
adapter addresses, and DLLs belong under the ignored `.scratch/` tree. Commit
only structural tests, sanitized fixtures, implementation, and conclusions in
the network RE record.

## Prerequisites

- A DRM-free Joint Operations 1.7.5.7 installation.
- Two isolated copies of the retail game, called `SERVER` and `CLIENT` below.
- A debug proxy build of `binkw32.dll` from the `opennova-int` repository.
- Godot 4.6.1 and a built OpenNova GDExtension.
- A Release build of `nw_pp` and `nw-lan-probe`.
- Npcap and Wireshark's `dumpcap` for an independent capture.

Keep the repositories' commit IDs and the deployed proxy SHA-256 in every
local run note. Never compare endpoints running different hook DLLs.

## 1. Prepare isolated retail copies

Set machine-local paths in the shell, not in tracked files:

```powershell
$repo = (Resolve-Path '.').Path
$source = $env:JO_GAME_DIR
$server = Join-Path $repo '.scratch\SERVER'
$client = Join-Path $repo '.scratch\CLIENT'

if (-not (Test-Path (Join-Path $source 'Jointops.exe'))) {
    throw 'JO_GAME_DIR must name the retail directory containing Jointops.exe'
}
New-Item -ItemType Directory -Force -Path $server, $client | Out-Null
robocopy $source $server /E /COPY:DAT /DCOPY:DAT /R:2 /W:1 /MT:16
if ($LASTEXITCODE -ge 8) { throw "SERVER copy failed: $LASTEXITCODE" }
robocopy $source $client /E /COPY:DAT /DCOPY:DAT /R:2 /W:1 /MT:16
if ($LASTEXITCODE -ge 8) { throw "CLIENT copy failed: $LASTEXITCODE" }
```

`Jointops.exe` and all static retail resources should initially match the
source. These files are intentionally role-local after launch:

- `binkw32.dll` (the debug proxy replaces the retail DLL);
- `onhook.cfg`;
- `ghw.txt`, `onhook.log`, and `_connectlog.txt`.

### Export the mounted parity corpus

Do not use loose `items.def`, `ammo.def`, or BMS files merely because their
names look right. Export the bytes that each retail role actually resolves
through the engine-faithful mounted VFS, then verify the create-new output
before launching anything:

```powershell
$corpus = Join-Path $repo '.scratch\parity\corpus'
$corpusArgs = @(
    '--server-root', $server, '--client-root', $client,
    '--expansion', 'revx02', '--output-dir', $corpus,
    '--mission', '01TR.bms', '--mission', 'TDH_I3A.bms',
    '--mission', 'CP04.bms', '--mission', '03TR.bms',
    '--expect-archive', 'items.def=expansion/revx02/RevX02.pff',
    '--expect-archive', 'ammo.def=expansion/revx02/RevX02.pff',
    '--expect-archive', '01TR.bms=localres.pff',
    '--expect-archive', 'TDH_I3A.bms=localres.pff',
    '--expect-archive', 'CP04.bms=localres.pff',
    '--expect-archive', '03TR.bms=localres.pff'
)
uv run python scripts\net\export_parity_corpus.py @corpusArgs --mode create
uv run python scripts\net\export_parity_corpus.py @corpusArgs --mode verify
```

The exporter fails if SERVER and CLIENT disagree on the resolution owner,
archive, size, hash, or bytes. For this six-case matrix, the DEF files must
resolve from `expansion/revx02/RevX02.pff`. There are still only four distinct
mission artifacts, all from `localres.pff`: repeated holdout missions are
exported and hash-bound once, then referenced by both case identities.

## 2. Build and deploy the current hook

From a 32-bit MinGW/MSYS shell in the `opennova-int` checkout:

```bash
cd onhook
./build_onhook.sh --debug --proxy
```

Then deploy that exact proxy to both copies:

```powershell
$proxy = Join-Path $env:OPENNOVA_INT_DIR 'onhook\binkw32.dll'
Copy-Item -LiteralPath $proxy -Destination (Join-Path $server 'binkw32.dll') -Force
Copy-Item -LiteralPath $proxy -Destination (Join-Path $client 'binkw32.dll') -Force
Get-FileHash -Algorithm SHA256 -LiteralPath `
    $proxy, (Join-Path $server 'binkw32.dll'), (Join-Path $client 'binkw32.dll')
```

All three hashes must match. Rebuild and redeploy both copies after every hook
change. A Release proxy does not expose the automation bridge or packet capture.

## 3. Install the role cfg files

The cfg beside each `Jointops.exe` is authoritative. Do not mix `LanHost*` and
`LanJoin*` keys in one role file.

Copy the tracked templates, then adjust only the pinned comparison inputs when
starting a new matrix:

```powershell
Copy-Item scripts\net\configs\retail-lan-server.onhook.cfg `
    (Join-Path $server 'onhook.cfg') -Force
Copy-Item scripts\net\configs\retail-lan-client.onhook.cfg `
    (Join-Path $client 'onhook.cfg') -Force
```

Repeat this template deployment before every topology; do not carry an MCP-
mutated cfg forward from a prior run. Preserve each retail endpoint's effective
cfg as create-new run evidence. The manifest role set is exact: RR records
server and client, RO only server, OR only client, and OO none. Each snapshot
pins its full SHA-256, its `server_cfg_template` or `client_cfg_template` role,
and a normalized base SHA-256. Base normalization removes only the MCP marker
and the owned `LanHost*`/`LanJoin*` per-run keys. All comments, unknown keys,
hook settings, and OpenNova settings remain byte-significant after newline
normalization and must equal the hash-bound tracked template; the removed
mission/port/callsign/game-type fields are checked separately against the case.

SERVER `onhook.cfg`:

```text
DebugMode 1
RendererHooksEnabled 1
PoolHooksEnabled 0
PatchesEnabled 1
ForceThirdPerson 0

LanHostMission 01TR.bms
LanHostPort 32786
LanHostLanMode 1
LanHostCallsign RetailHost
LanHostGameType 65568
LanHostMaxPlayers 4
LanHostReadyTimeoutMs 120000
LanHostReadyProbeIntervalMs 1000

OpenNovaResourceDir .
OpenNovaExpansion revx02
OpenNovaIntegrityProfile retail-revx02-024f56f2-2d087374
OpenNovaWindowed 1
OpenNovaResolution 1920x1080
```

CLIENT `onhook.cfg`:

```text
DebugMode 1
RendererHooksEnabled 1
PoolHooksEnabled 0
PatchesEnabled 1
ForceThirdPerson 0

LanJoinAddress 127.0.0.1
LanJoinPort 32786
LanJoinCallsign RetailJoiner
LanJoinDiscoveryTimeoutMs 30000

OpenNovaResourceDir .
OpenNovaExpansion revx02
OpenNovaWindowed 1
OpenNovaResolution 1920x1080
OpenNovaIntegrityProfile retail-revx02-024f56f2-2d087374
```

`OpenNova*` keys are ignored by retail and consumed by the PowerShell launchers,
so the same files configure both implementations. Put the integrity profile in
both role cfgs: the OpenNova client uses it to answer a retail host, and the
OpenNova host uses the same corpus to validate a retail client's answers. The
profile is valid only for the witnessed 1.7.5.7/revx02 corpus. It contains CRCs
for all 102 live ammo-definition rows; a different corpus/profile must fail
silent rather than emit or validate invented checksums. Retail's separately
witnessed out-of-range arm still answers zero beginning at index 102.

`PoolHooksEnabled 0` preserves retail's allocator for oracle work. The enlarged
replacement pool has previously caused a post-join allocator-list fault; it is
not needed for LAN automation.

Validate the cfg role and retail command line without launching anything:

```powershell
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\launch_retail_cfg.ps1 -GameDir $server -PlanOnly
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\launch_retail_cfg.ps1 -GameDir $client -PlanOnly
```

Require SERVER=`host`, CLIENT=`joiner`, windowed=`True`, expansion=`revx02`,
OpenNova resolution=`1920x1080`, and retail arguments `/w /many /exp revx02`.

## 4. Build the OpenNova tools and runtime

```powershell
cmake --build build --config Release --target nw_pp opennova_nw_lan_probe
cmake --build build-godot --config RelWithDebInfo
```

Build the GDExtension as RelWithDebInfo (the `scripts/build_godot.sh` Dev
flavor), never `--config Debug`: the Visual Studio generator ignores
`CMAKE_BUILD_TYPE`, and an MSVC Debug (/Od /RTC1) extension costs 5-10x on
native sim work — wire parity is unaffected but every live frame-time or
smoothness observation made on it is invalid.

Set `GODOT_BIN` to the Godot 4.6.1 console executable when the launcher cannot
find it through the checkout's `.godot-bin` convention.

Keep the official `_console.exe` wrapper in the process tree. The shared launch
module returns the exact GUI runtime only after proving that it is the wrapper's
direct child with the expected path, argument vector, start identity, and active
execution. A `-PassThru` runtime carries `OpenNovaLauncherProcess` plus the
serializable `OpenNovaLaunchProof`; stop it with
`Stop-OpenNovaLanProcess -Process $process` so forced cleanup closes the
wrapper-owned job rather than stranding part of the engine tree. Final matrix
summaries retain one proof for each OpenNova role (RO joiner, OR host, OO both),
and the verifier requires `wrapper_used=true` against the hash-bound launcher
and runtime artifacts.

Verdict-bearing RO/OO joins use the tracked
`godot/tests/net/parity_joiner_driver.gd`, never an ignored scratch copy. The
corpus inventory hash-binds that file, and the launch proof must contain its
exact absolute path as the single Godot `--script` argument. The driver is a
frame-driven `MainLoop`: it owns readiness/motion publication and the
cooperative teardown described below without retaining GDExtension objects in
a suspended coroutine.

The OpenNova host launcher does not equate a PID or bound socket with readiness.
It repeatedly sends the retail `0x41` discovery probe and must decode a valid
game-server `0x81` before printing `OPENNOVA_HOST_READY=True`. Large cold mission
loads can take longer than the retail client's discovery window; never replace
this check with a fixed sleep. `nw-lan-probe` is stateless and stops before
`0x42`, so it does not consume a player slot.

## 5. Arm independent capture

Detect the available Npcap adapters first:

```powershell
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\detect_capture.ps1
```

For a same-machine run, capture loopback and all connected physical adapters.
Start before either endpoint so the handshake and session keys are present:

```powershell
$tag = 'retail-parity-unique-run-id'
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\capture.ps1 -Action start -Tag $tag -All `
    -Filter 'udp and port 32786' -DurationSeconds 240
```

Use the bounded `DurationSeconds` path where practical so dumpcap closes its own
pcapng blocks. The hook additionally records role-isolated process-local PCAPs.
Both layers matter: adapter capture finds harness/routing mistakes, while the
hook confirms what retail actually sent and received. The runner persists a
hash-bound `external.pcapng` and structural exact-EOF proof for RR, RO, OR, and
OO. For RR/RO/OR the verifier decodes that pcapng independently and requires
its interior steady packet identities to match the hook-owned evidence; a
clean hook capture cannot conceal malformed or missing adapter traffic.

## 6. Run the comparison matrix

Use a unique, create-new output directory for every run. Never reuse a hook log
or PCAP path. Complete A, B, C, and D for one case before changing its inputs;
then repeat all four for the next mission/game type. A retail-retail run from a
different case is not a valid golden. Record the observed equipped ADM ->
AmmoDef row alongside each capture. Record the actual root viewport as
1920x1080 for both non-dedicated hosts; discard/reshoot a run at another size
because S2C 0x68 wraps its cursor against the viewport height.

Give all 24 runs unique canonical `run_id` values and bind them to one
create-new `suite_id`. Every case declares `readiness_mode` explicitly; never
infer it from a mission filename. `in_match` requires the local-player/render
gate used by the exercised cases. `deploy_hold` deliberately leaves the client
on the player-paced deployment presentation, where retail can report
`phase=loading` and `local_player=false` while its authenticated peer exchanges
ordinary gameplay traffic. Begin the declared window only after the selected
lifecycle gate and the common decoded wire-readiness gate below both pass.
Record strict UTC `Z` timestamps as
`steady_started_utc` and `steady_completed_utc`; keep endpoints healthy for at
least `steady_seconds` and at most 15 seconds beyond it. Steady liveness and
density checks use exactly the first declared interval from steady start,
require C2S and S2C decoded gameplay in its first and last 500 ms, and reject
any internal gameplay gap over one second. The generic RR-relative packet,
event, and material-body comparison covers the selected connection from its
first decoded packet through the end of that interval, including the readiness
warmup before steady start. Do not compensate for slow setup by widening or
shifting a capture after the fact.

### Tracked suite harness

Use the tracked harness for the verdict-bearing six-case, 24-cell run. Supply every
machine-local dependency explicitly; the scripts resolve and hash those exact
paths, while all suite provenance, captures, witnesses, and manifests remain
under the ignored `.scratch` tree. `SuitePrefix` is mandatory and create-new:
the runner refuses an existing suite, manifest, or canonical run directory.

```powershell
$suite = 'parity-20260803-a'
$server = 'C:\absolute\retail\SERVER'
$client = 'C:\absolute\retail\CLIENT'
$mcp = 'C:\absolute\onhook\onhook-mcp.exe'
$godot = 'C:\absolute\Godot_v4.6.1-stable_win64_console.exe'
$machine = @(
    '-RetailServerRoot', $server,
    '-RetailClientRoot', $client,
    '-OnHookMcpPath', $mcp,
    '-GodotLauncher', $godot
)

& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\test_run_parity_matrix.ps1

# Hash and configuration validation only; this launches no game endpoints.
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\run_parity_matrix.ps1 `
    -SuitePrefix $suite @machine -PreflightOnly

# Omit -PreflightOnly only when ready to capture all 24 runs.
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\run_parity_matrix.ps1 `
    -SuitePrefix $suite @machine

& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\generate_parity_manifest.ps1 `
    -SuitePrefix $suite @machine

& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\verify_parity_matrix.ps1 `
    -Manifest ".scratch\parity-final\$suite.manifest.json"
```

Once the non-preflight capture command starts, freeze every verdict-bearing
input until manifest generation and verification finish: no source, cfg,
corpus, dependency, build, hook/GDExtension deployment, or launcher/tool change.
The source-state and artifact hashes reject drift by design. If anything changes,
discard the suite and repeat preflight plus all 24 runs under a new `SuitePrefix`.

The matrix runner reads mission, game type, port, resolution, callsigns,
readiness, input policy, and steady duration from the hash-bound tracked
`parity-matrix.example.json`. Do not keep a second local case table. The
runner and manifest generator derive run slugs, mission artifact roles, and
exported mission bindings from that single ordered case list. They re-derive
the same artifact and repository-source inventory and refuse any fingerprint
drift between capture and generation. The four canonical IDs remain required,
but tracked holdouts may extend the ordered list. Repeated mission names share
one hash-bound corpus artifact, while every case/topology keeps a unique run ID
and capture. Port reuse is allowed only because the runner completes cells
sequentially and still requires coverage of all four automation ports.

### A. Retail host -> retail joiner (ground truth)

The preferred agent-owned path is the debug hook MCP tool
`onhook_run_lan_pair`. Pass absolute paths to the distinct SERVER and CLIENT
copies, the pinned mission/expansion/port, `capture=true`, `windowed=true`, and
a timeout long enough for a cold mission load:

```text
onhook_run_lan_pair({
  "mission": "01TR.bms",
  "host_game_dir": "<absolute SERVER directory>",
  "joiner_game_dir": "<absolute CLIENT directory>",
  "expansion": "revx02",
  "join_address": "127.0.0.1",
  "port": 32786,
  "host_callsign": "RetailHost",
  "joiner_callsign": "RetailJoiner",
  "game_type": "65568",
  "max_players": 4,
  "run_id": "<unique safe run id>",
  "output_dir": "<absolute ignored run directory>",
  "capture": true,
  "windowed": true,
  "wait_timeout_ms": 120000
})
```

For `readiness_mode=in_match`, the tool writes the role blocks atomically, launches SERVER first, waits for an
authoritative responder and local player, launches CLIENT, and waits for both
roles to reach `in_match`. Retain both returned run IDs and instance IDs. Stop
the joiner run first and the host run second with `onhook_stop_run`.

`onhook_run_lan_pair` and `onhook_host_lan` are not deploy-hold launchers: they
do not register the run until a rendered local player exists. For
`readiness_mode=deploy_hold`, launch SERVER and then CLIENT independently with
`launch_retail_cfg.ps1 -PassThru`, retain both exact `Process` objects, and
discover each bridge instance by exact PID. Require the host's exclusive UDP
ownership plus responder readiness and the joiner's active authenticated peer;
do not reinterpret a missing retail `GamePerson` as a broken wire session.

### B. Retail host -> OpenNova joiner

Launch retail SERVER from its cfg and wait for the live responder:

```powershell
$run = Join-Path $repo '.scratch\runs\retail-host-opennova-joiner-unique'
New-Item -ItemType Directory -Force -Path "$run\host", "$run\joiner" | Out-Null
$retailHost = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\launch_retail_cfg.ps1 `
    -GameDir $server -RunId 'retail-host-unique' `
    -OutputDir "$run\host" -PassThru
```

After the bridge reports the host ready, launch OpenNova from CLIENT cfg:

```powershell
$openNovaJoiner = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\join_opennova.ps1 `
    -HookConfig (Join-Path $client 'onhook.cfg') `
    -LogFile "$run\joiner\godot.log" -PassThru
```

Do not pass a mission override. The joiner must learn `01TR.bms` and expansion
identity from the retail host.

### C. OpenNova host -> retail joiner

Launch OpenNova from SERVER cfg. This command blocks until the real LAN
responder is ready:

```powershell
$run = Join-Path $repo '.scratch\runs\opennova-host-retail-joiner-unique'
New-Item -ItemType Directory -Force -Path "$run\host", "$run\joiner" | Out-Null
$openNovaHost = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\host_opennova.ps1 `
    -HookConfig (Join-Path $server 'onhook.cfg') `
    -LogFile "$run\host\godot.log" -PassThru
```

Require `OPENNOVA_HOST_READY=True`, then launch retail CLIENT:

```powershell
$retailJoiner = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\launch_retail_cfg.ps1 `
    -GameDir $client -RunId 'retail-joiner-unique' `
    -OutputDir "$run\joiner" -PassThru
```

The successful retail path consumes the intro gate, initializes the required
MainMenu state without presenting a menu frame, discovers the configured host,
executes the stock selected-server transition, and enters `PreMenu`/loading.
No click, key press, LAN browser, join dialog, or `NW_LAN_JOIN` variable is part
of this path.

### D. OpenNova host -> OpenNova joiner

```powershell
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\run_lan_pair.ps1 -Mission 01TR.bms `
    -Port 32786 -HostName RetailHost -JoinName RetailJoiner `
    -GameType 65568 -Expansion revx02 -Windowed -Resolution 1920x1080 `
    -HostHookConfig (Join-Path $server 'onhook.cfg') `
    -JoinHookConfig (Join-Path $client 'onhook.cfg') `
    -HostResourceDir $server -JoinResourceDir $client `
    -IntegrityProfile retail-revx02-024f56f2-2d087374 -Wait
```

`run_lan_pair.ps1` waits for protocol readiness before its bounded
post-readiness join delay. `-IntegrityProfile` binds both OpenNova roles to the
same independently witnessed corpus: the joiner answers the challenge and the
host validates the reply. Matrix D is also compared directly with the
same-case retail-retail oracle, but cannot substitute for directional matrices
B or C.

## 7. Acceptance and clean shutdown

### Common decoded wire-readiness gate

Do not start a steady window merely because a socket, peer boolean, or packet
counter exists. Copy the owned rolling dumpcap stream and decode the copy with
the hash-bound `items.def`. (onHook holds its in-process `traffic.pcap`/`net.pcap`
exclusively until capture drain, so it cannot be the live polling source.) The
tracked helper performs this poll and writes a create-new witness:

```powershell
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\wait_parity_wire_ready.ps1 `
    -Capture $externalCapture -Items "$corpus\items.def" `
    -Mission CP04.bms -MissionArtifact "$corpus\missions\CP04.bms" `
    -Map CP04.bms -GameType 196640 -Expansion revx02 `
    -HostSessionName 'Untitled ' -HostCallsign P40303Host `
    -JoinerCallsign P40303Join -Output "$run\wire-ready.json"
```

Acceptance requires the exact recipient-scoped S2C `0x7B` joiner, session
name, map filename, game type, and expansion. Its advertised mission must be
one stable value equal to either that exact filename or the printable
`mission_name` read from bytes 4..35 of the hash-bound BMS; retail selects
between those sources in `NapiNPMsg_0x7B_BuildPayload @0x507822`. The same
connection must carry exactly one S2C `0x0B` whose 616 bytes equal that BMS
header. This bounded readiness choice is not a parity waiver: the final
same-case RR-relative material comparison still requires every `0x7B` body
byte-for-byte. Acceptance also requires S2C `0x46` roster identities for both
callsigns; C2S `0x0B`; S2C `0x0F`; a nonzero decoded C2S `0x0C` handle with
a matching S2C player entity; decoded/local S2C `0x0A`; at least 40 gameplay
samples in each direction spanning at least ten seconds; and at least 20
matched C2S `0x2C`/S2C `0x57` liveness tokens. Cleanup copies that exact
dumpcap stream into `$run\external.pcapng`. The final manifest hashes both the
complete external artifact and its witnessed prefix; the verifier decodes that
prefix again and requires its session/SIDs to be the same connection present
in the completed retail-perspective steady capture.

For each retail endpoint, use `onhook_instances` and retain the exact
`instance_id`. `readiness_mode=in_match` requires:

- `phase=in_match` and `failure=none`;
- peer and local-player state present;
- authority and responder-ready state on a retail host;
- no intro, MainMenu, LAN browser, or join dialog presented on a successful
  retail CLIENT run;
- a captured CLIENT frame showing the expected HUD/player count;
- sustained `0x0A`/`0x0C` gameplay traffic, not merely discovery;
- changing (not merely nonzero) player position in C2S `0x0C` and changing
  player plus vehicle compact positions in S2C `0x0A` somewhere across each
  topology's case set. In every exercised case, use the same elapsed trajectory
  on both clients: after a 250 ms focus/pre-roll, W for 1800 ms with +320 turn,
  a 250 ms refocus/gap, D for 1200 ms with -320 turn, another 250 ms gap, and A
  for 900 ms with +160 turn. Spread each turn over exactly 20 samples. RR/OR
  targets one exact retail PID/window and retains its completion witness; RO/OO
  releases a run/PID/topology-bound OpenNova motion gate immediately after
  steady start and retains the normalized gate plus completed-motion witness.
  Also retain at least one case with moving/turning map or AI vehicle traffic;
  otherwise reshoot instead of accepting a trace that cannot detect hardcoding.

For RR/OR, `exercise_retail_input.ps1` is fail-closed around `SendInput`. It
must prove that the selected HWND is still owned by the exact retail PID and is
the foreground window after a settling check, then recheck that ownership and
focus before and after every key/mouse insertion and wait. A stale HWND, owner
change, focus loss, short `SendInput`, or retail exit aborts the run instead of
writing a complete witness. The create-new `opennova.retail-input.v1` artifact
binds run ID, topology, PID, process start, HWND, strict start/completion times,
the fixed trajectory, inserted-event count, focus attempts, and foreground
checks. The runner, generator, and verifier reload and SHA-256-bind it to the
accepted retail joiner PID and require the entire input interval inside the
declared steady window.

`readiness_mode=deploy_hold` instead requires the retail joiner to remain
`phase=loading`, `local_player=false`, `failure=none`, active with an
authenticated peer, client network/socket/connection modes `2/2/2`, the exact
requested port, recording capture, and zero capture-error counters. A retail
host still requires authority, responder readiness, exact UDP port ownership,
and host modes `2/3/3`. An OpenNova joiner proves the corresponding
player-paced state with a fresh heartbeat carrying
`deployment_pick_pending=true`, a nonzero learned self handle, no join error or
session loss, and no automatic deployment pick. Do not request input or a
screenshot in this mode.

The example cases set `require_analog=false`. They do not prove a live occupied-
vehicle analog-axis path: they prove on-foot movement and the vehicle traffic
the map/AI produces. Keep occupied-vehicle analog behavior as structural test
evidence unless a separately controlled live case sets `require_analog=true`.

The canonical input trajectory also has no deterministic, map-independent seat
acquisition/attach action: it sends only W/A/D and relative look. Therefore an
on-foot phase-8 sentinel (or even an incidental bound sample) must not be called
a controlled occupied-mount witness. The verifier reports
`occupied_mount_coverage.status=structural-only`, per-topology phase-8 and
observed-bound sample counts, and `netsim_two_peer_fanout` as the structural
clip/reserve/handle witness. Upgrade that status only after adding a reliable
live attach action that every selected mission can execute and verify.

Call `onhook_screenshot` before shutdown only for `in_match`. Drain each retail capture with
`onhook_stop_capture` or `onhook_stop_run` and require:

- `capture_complete=true`;
- `capture_stats_available=true`;
- stopped/clean state;
- zero dropped, truncated, write-error, and unsupported-async counters.

Every topology's adapter capture is dumpcap-owned and carries a generated
`external_capture`, `external_capture_sha256`, and
`external_capture_proof`. Capture stop validates pcapng byte order, every block
header/length/trailer, at least one packet block, and exact EOF. The manifest
generator persists that proof for RR, RO, OR, and OO, and the verifier repeats
the structural parse directly from the hash-bound artifact. RR/RO/OR also keep
their hook-owned primary capture and must cross-check against the independently
decoded pcapng.

The OO adapter capture has no hook/ISB async counters.
Record `capture_stats_available=false` and omit all four counter keys; never
invent zeroes. This exception is valid only for OO with `perspective=external`.
After both OO endpoints stop, prove the game port is unowned and send one unique
UTF-8 `OPENNOVA-PCAPNG-TAIL|<run_id>|<32-lowercase-hex>` datagram before stopping
capture. In addition to the structural proof above, require marker presence in
the final packet block. Pin the
capture hash, marker text/hex/hash/send time, and exact block/packet-block counts
in `external_capture_proof`. The verifier independently repeats that parse,
requires the send time at or after steady completion, and also requires clean
raw decode, nonzero SID/epoch binding, gap-free sequences in both directions,
and a one-for-one packet-group/event image. The verifier binds the outer
handshake to that unique protocol/gameplay client-port session. A foreign
incomplete `0x41`/`0x81` readiness probe remains admissible evidence noise, but
its records are never eligible for RR handshake comparison.

RO/OO are verdict-bearing only when the exact launched OpenNova joiner exits
cooperatively with code zero. After steady completion, the runner atomically
creates `joiner-stop-request.json` with schema
`opennova.parity-joiner-stop-request.v1`, run ID, topology, runtime PID, and a
strict UTC request time. The tracked driver validates that exact property set,
drains/releases the world and game, atomically creates `joiner-shutdown.json`
with schema `opennova.parity-joiner-shutdown.v1`, echoes the request time, and
exits. Both files are create-new and SHA-256-bound; the ACK must name the same
run/topology/PID, carry `clean=true`, begin no earlier than steady completion,
and complete within 20 seconds of the request. The generator and verifier each
reload both files and independently enforce their hashes, exact fields, PID,
timestamps, and tracked-driver launch binding.

The runner tries this cooperative path on every normal RO/OO completion and
again from `finally` before any exact-process fallback. A missing, late,
tampered, mismatched, or nonzero-exit ACK fails the run. Any required forced
fallback sets `opennova_joiner_exact_fallback_required=true`; successful
manifest generation and verification require that flag to be exactly false and
the normalized shutdown witness to be present. RR/OR must carry neither field.

OR/OO additionally require a clean shutdown proof for the exact OpenNova host
runtime. Its normal `CloseMainWindow()` request must be accepted, the retained
runtime must exit within 20 seconds with exit code exactly zero, and neither
the runtime nor its wrapper-owned job may need a force fallback. The
`opennova.parity-host-shutdown.v1` witness binds the run ID, topology, exact
launch-proof runtime PID, strict request/completion timestamps, accepted close,
exit code, and `clean=true`. The request must follow both process start and
steady completion; completion must not precede it.

The same witness binds the exact
`repo:.scratch/runs/<run_id>/host/godot.log` and its SHA-256. After process exit,
the runner, manifest generator, and verifier independently require a nonempty,
BOM-safe log with no ObjectDB, resource, RenderingServer, or RID teardown-leak
diagnostic. Manifest generation and verification require
`opennova_host_shutdown_witness` plus
`opennova_host_exact_fallback_required=false` if and only if the topology is OR
or OO; RR/RO must carry neither field. A missing/mismatched PID or timestamp,
nonzero exit, rejected close, hash/log diagnostic failure, or any fallback
invalidates the run.

Close only the recorded endpoint PIDs. Request a normal window close first;
force-stop an exact owned PID only after a bounded graceful-close timeout.
Never kill processes by executable name when multiple roles may be live.

## 8. Decode and compare

Record `capinfos` packet count, byte count, duration, and SHA-256 for every
process-local and external capture in the ignored run note.

Compare by endpoint role, not merely by topology label:

- matrix B's OpenNova-client trace must match matrix A's retail-client behavior
  against the same retail host;
- matrix C's retail-client trace must see the same host behavior it saw from
  the retail host in matrix A;
- matrix D remains a self-consistency control, but both decoded directions are
  also compared directly with the same-case retail-retail oracle. It cannot
  replace the mixed RO/OR evidence, which proves each OpenNova half against a
  live retail peer.

The canonical acceptance input is the generator-produced, hash-pinned manifest,
not a collection of ad hoc sidecars. The tracked
`scripts/net/parity-matrix.example.json` is immutable suite policy; do not copy
it and hand-fill its placeholders. The generated corpus block points at
the VFS-exported retail `items.def`, `ammo.def`, and mission bytes used by the
endpoints and carries their computed SHA-256. It must also hash-bind the retail
executable and mounted base/expansion archives (with identical SERVER/CLIENT
mirrors), hook, both the selected Godot launcher and the resolved runtime it
executes, native OpenNova DLL, decoder, LAN probe, hook
MCP, all capture/launch/input/wire-readiness/export/runner/generator/verifier
scripts, the tracked `godot/tests/net/parity_joiner_driver.gd`, both cfg
templates, and the complete repo-native Godot `res://` resource set returned by
`Get-GodotRuntimeArtifacts`. The suite also binds `corpus.source_state` to every
nonignored path reported by Git (tracked plus untracked) outside the harness
evidence/build roots, independent of staging state; clean submodules contribute
their exact commits. Use `repo:<path>` for repository-owned artifacts and
evidence so moving the manifest cannot retarget them. Each case must pin a unique
`mission_artifact.path`/`sha256`; every run repeats the actual
`mission_path`/`mission_sha256` it loaded. The leaf must equal the case mission,
and renamed duplicate BMS bytes are rejected. The corpus fingerprint is derived from the sorted,
LF-terminated UTF-8 inventory lines `role=sha256` plus
`repo_source_state=sha256`; it is not an operator-chosen label. Thus a source
edit after capture is rejected even if the previously built DLL has not
changed; preflight prints the digest that capture will recompute. The verifier
supplies the hash-checked `items.def` to `nw_pp`, so an
unclassified `0x0A` entity fails closed. Every run repeats its case's mission,
numeric game type, resolution, port, callsigns, advertised session name, derived corpus fingerprint, and
expansion beside its unique `run_id`, enclosing `suite_id`, capture SHA-256, and
capture-stats declaration. Its topology-correct effective cfg snapshots bind
their full hashes and normalized bases to the tracked templates: RR server plus
client, RO server, OR client, and OO none. Only MCP-owned dynamic role keys are
removed for base comparison; all fixed cfg bytes must match. This redundancy
binds each mixed trace to the intended same-case RR golden. The verifier keeps
the advertised `server_name` from the host player `host_callsign`. Every case
and run pins `server_name` exactly, and the compatibility
`host_session_name` field must equal it byte for byte. Retail's pinned wire
value is exactly `"Untitled "`, including the trailing space; it is not the host
player's callsign. Decoded recipient-scoped S2C `0x7B` records must match that
`server_name` byte for byte and identify the joiner, while name-bearing S2C
`0x46` slot records must identify both host and joiner callsigns exactly.
Retail need not send the host a second recipient's `0x7B`. This prevents a
caller or implementation from hardcoding one identity into the other without
imposing a wire record retail itself omits. The verifier
rejects reuse of either a capture path or its SHA-256 bytes under another
case/topology, then requires exactly RR, RO, OR, and OO for each of the four
tracked canonical cases; a missing or additional `runs` topology property is
rejected. The evidence manifest cannot redefine or lower that
policy: the hash-bound tracked template fixes the `revx02` expansion profile,
all four mission/game-type/port/readiness tuples, each topology's steady
duration, and every comparison tolerance, including Deathmatch zero; the
verifier also requires non-team, team, objective, non-objective, waypoint, and
non-waypoint branches.
Verdict-bearing cases must carry an explicit empty `exceptions` array; wire
differences cannot be waived by manifest-authored reasons.

Build and self-test the stable event decoder, then produce a create-new verdict:

```powershell
cmake --build build --config Release --target nw_pp
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\test_run_parity_matrix.ps1
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\wait_parity_wire_ready.ps1 -SelfTest
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\verify_parity_matrix.ps1 -SelfTest
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\verify_parity_matrix.ps1 `
    -Manifest ".scratch\parity-final\$suite.manifest.json"
```

Acceptance requires `PARITY_STATUS=PASS` and preservation of the generated
`*.parity.json` beside the ignored manifest. The verifier always decodes the
hash-matched captures again with `nw_pp --parity-events`; it never consumes a
possibly missing, stale, or wrong-golden `*.vs-golden.txt` report. It compares
both directions of RO, OR, and OO to the same-case RR run: the implementation
direction proves what OpenNova emits, while the peer direction proves the
retail endpoint continued to react as it did against retail in the mixed
topologies. This bilateral rule also covers the outer handshake: client
`0x41`/`0x42` and server `0x81`/`0x82` are compared for every mixed and OO run,
after gameplay-session binding excludes any earlier readiness probe. An
unexpected OpenNova-client C2S tag is a failure. `nw_pp` emits exactly one
`PARITY_DATAGRAM` record for every captured UDP payload. Any malformed outer
envelope, unknown opcode, failed handshake body, missing session key, failed
protocol decode, duplicate accounting frame, or protocol datagram without one
matching `PARITY_PACKET` is a failure inside the comparison interval; it cannot
silently disappear behind a valid decoded subset. The deliberate OO tail marker
is later than that interval.

Initial packet record grouping and outer packet flags are compared through the
first gameplay packet. The complete comparison interval also compares
RR-normalized session-packet cadence, header/ACK-only cadence, header-only flag
coverage, and the packet shape/flag distribution. Thus recordless packets are
evidence rather than invisible transport noise. Every identity
through `0x1ff` exposes its full material body except ordinary C2S `0x0C` and
S2C `0x0A`, which use decoded state records; high-table settings bodies remain
material. Initial bodies compare byte-for-byte after normalizing the live S2C
`0x0F` tick and routing explicit crypto/challenge/clock records to their
stateful checks. Stable team/deployment/loadout bodies compare exact sets, and
other streams reject RR-varied bodies collapsed to constants or RR-nonzero
bodies replaced by zeroes. After the initial prefix, SID-bound packet-group and adjacent
event-order distributions must stay within the documented 2% material-share,
8-point absolute, and 50% RR-relative tolerances. Decoded C2S `0x0C`/S2C `0x0A` summaries
must match RR's shapes/classes/types, header flags, carrier presence,
movement/state flags, analog axes, anchors/local tails, and per-entity
orientation/state while preserving nonzero, changing player and vehicle state.
Initial C2S positions and S2C anchors must remain within 64 world units of RR;
origin-normalized ranges, displacements, steps, step shapes, and short-cycle
rates reject wrong-map offsets and canned motion while allowing an 8x independent-input band.
Both ammo `0x31` and ADM/entity `0x30` integrity replies remain suite-wide
RR-derived CRC/checksum oracles. In addition, every OpenNova-host OR/OO run must
exercise the exact same-case RR row set for each challenge family, so a row
hardcoded from some other mission cannot pass merely because it appeared
elsewhere in the suite.
Unsupported semantic decode is a failure, not an omitted comparison. OO remains
a control that cannot replace live mixed-peer evidence, while its decoded wire
behavior is still fully compared with RR.

Decode sequencing and fields with the same build used for both sides:

```powershell
& build\apps\nw_pp\Release\nw_pp.exe `
    "$run\joiner\traffic.pcap" --sequencing --max-frames 60

& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\decode.ps1 -Capture "$run\joiner\traffic.pcap" `
    -Items $env:JO_ITEMS_DEF
```

Use the retail CLIENT process capture from matrix A as the immutable golden:

```powershell
& powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File scripts\net\diff_vs_golden.ps1 `
    -Ours "$run\joiner\traffic.pcap" `
    -Golden '<ignored retail-client golden capture>' `
    -Items $env:JO_ITEMS_DEF
```

For the exploratory coverage report, require `GAPS=0`, `SPURIOUS=0`, and
`DECODE_FAILURES=0`.
`STATE_ABSENT` is informational: it names event-conditioned messages not
exercised by the shorter run. Induce those states in dedicated witnessed runs;
do not fabricate them in a steady-state stream. Client-only extras are
informational only in this broad coverage report; the manifest verifier's RO
client-direction comparison is strict. A green coverage sidecar alone is not
matrix acceptance.

For the initial reliable stream, compare record grouping as well as tag order.
The witnessed mode-1 `00TRg` boundary includes:

```text
S 0x75,0x60
C 0x47
C 0x37
S 0x75,0x64,0x16
C 0x22,0x09
S 0x46,0x2c
S 0x08
S 0x2a (six packets)
S 0x1c,0x0b,0x66,0x76,0x11
C 0x0a
S 0x19
```

Normalize session IDs, sequence/ack numbers, timestamps, ports, and other
nondeterministic identity fields before comparing sessions. Within one capture,
however, retain the encrypted protocol SID together with client port and
sequence when identifying an original/replay packet; a reconnect can reuse a
port and sequence without being a NACK replay. Never use encrypted raw-byte
equality as the primary semantic verdict.

For maintenance parity, also make these stateful assertions from decoded
traffic rather than comparing tag totals alone:

- S2C `0x31` row equals the live player's equipped ADM -> AmmoDef index; it is
  not fixed to `0x18`. C2S `0x21` echoes that row and key and carries
  `xorKey ^ sanitized-276-byte-CRC`. The verifier removes the echoed key and
  derives its row -> CRC oracle only from
  this suite's RR captures and refuses an unwitnessed mixed-run row. Require at
  least two distinct RR rows so a fixed-row implementation cannot pass by
  accident. It similarly checks S2C `0x30` -> C2S `0x20` against the RR-derived
  whole-ADM/entity checksum oracle.
- S2C `0x30`/`0x31` alternate on one process-global 311-tick scoreboard
  cadence and continue while a live player is held on the deployment UI.
  Every boundary emits transient S2C `0x16` immediately before the integrity
  request in the same packet. The family phase initializes to `0x31`, advances
  even when no recipient is eligible, and survives mission reset. Therefore a
  newly joined client's first *observed* family may be either `0x31` or `0x30`;
  validate adjacency and alternation instead of hardcoding capture-local phase.
  Both requests are one-send/transient records and must be absent from a NACK
  reconstruction.
- Beyond the load-time S2C `0x42`, no additional periodic control quartet is
  emitted during the first 1,860 eligible live-age increments. Retail checks
  the previous age before incrementing it, so the earliest quartet is
  `Server_TickUpdate` call 1,861. Pending/hidden presentation pauses rather
  than resets the pre-maturity age. Once mature, the 744-tick countdown keeps
  running through death and deployment presentation state. Require a
  same-datagram/round `0x39` + `0x42` + `0x43` (+ non-dedicated `0x68`) every
  744 server updates while that mature slot remains in match. Pin transient
  `0x42=0000`, reliable retained per-player `0x39`, reliable time sequence
  `1,1,2,2`, and transient 50-row `0x68` wrap at the pinned height 1080. A
  healthy lossless client must answer each quartet once before the next one,
  grouped as reliable C2S `0x1C` (seed-normalized against the same-case RR
  charattr CRC oracle), reliable `0x08` (`[echoed sequence][client GetTickCount]`),
  and transient `0x3D` when `0x68` was present. A mixed packet's NACK replay
  must retain only its reliable records. Any dispatched C2S `0x08` clears the
  silence counter, but only a matching 8-byte reply whose round and full
  baseline client deltas each fit within truncation of 103% of host elapsed
  time can close the round and let the next producer increment the sequence.

Set a case's manifest `maintenance=true` only when its steady window is long
enough for at least four alternating integrity challenges. Treat the later
control schedule separately: four control quartets need at least 4,093 server
calls, so the deployed `control_mode="required"` example uses 80 seconds after
readiness. CP04 deliberately combines `maintenance=true` with
`control_mode="forbidden"` while the player is held before maturity. In the
deployed long required-control case, mature the age first and then
exercise death or deployment UI long enough to prove the countdown continues.
Capture timestamps can check the 311/62 Hz and 744/62 Hz intervals only within
the manifest's millisecond tolerance; the exact 311/1,861/744 counter boundaries,
the process-global integrity phase, and its mission-reset persistence remain
structural native-test evidence. The malformed/timing-invalid C2S `0x08`
silence-reset path and exact 3% delta rejection are likewise structural native
test evidence unless a controlled-clock negative capture is supplied. Passive
lossless traffic
cannot prove NACK retention unless the capture contains a same-sequence replay.
When no replay exists the JSON verdict records that limit; require the
`protocol_message` retransmission regression, or add a controlled-loss witness,
before claiming retention parity.

## 9. Failure triage

### CLIENT presents MainMenu instead of joining

Read the process-local capture first. Repeated `0x41` with no `0x81` means the
host responder missed the discovery window. The OpenNova host launcher must
return `OPENNOVA_HOST_READY=True` before retail starts. A cold mission may bind
UDP long before it can answer discovery.

If the hook reports `invalid_environment`, use `launch_retail_cfg.ps1`. The
owned-run key is exactly `ONHOOK_RUN_ROLE`, not `ONHOOK_ROLE`. The launcher
clears inherited `NW_LAN_*`, sets child-only metadata, and restores the caller's
environment.

### Intro video appears

Verify that CLIENT loaded the current debug proxy, that `onhook.cfg` is beside
the exact `Jointops.exe` being launched, and that the three proxy hashes match.
A successful cfg join consumes the one-shot intro gate before rendering it.

### CLIENT discovers a physical address while cfg says loopback

This is expected on one machine. The hook accepts the discovered address only
after proving that it belongs to a local adapter and that the requested port
matches. Do not loosen endpoint validation for remote addresses.

### Capture is empty or incomplete

Confirm `ONHOOK_RUN_ROLE`, log/capture paths, and capture port came from the cfg
launcher or MCP-owned run. An unsupported completion-based Winsock operation
increments `unsupported_async` and makes the capture unsuitable as a golden.
For adapter capture, include loopback and the active physical interfaces and
repeat the filter for each dumpcap `-i`.

### Expansion or integrity failure

Both endpoints must use `/exp revx02` and the same retail corpus. Do not reuse
the named integrity profile with a different install. An unknown profile or a
challenge outside its proved table contract must not be answered with an
invented in-range CRC.

## 10. Iteration discipline

For every divergence:

1. preserve the failed run and write an ignored `RUN.md` with topology, commit
   IDs, DLL hash, commands, lifecycle result, capture counters, file hashes,
   and the first mismatching semantic boundary;
2. verify that the retail-retail control exercised the same precondition;
3. reduce the mismatch to a small sanitized fixture or structural unit test;
4. make the protocol fix in Godot-free libraries, leaving sockets/process/UI in
   apps or bindings;
5. rebuild OpenNova and the hook, redeploy the same proxy to both copies, and
   re-run all affected tests;
6. repeat matrices B and C against the unchanged matrix-A golden;
7. update `docs/net/novaworld-net-re.md`, `docs/divergence-ledger.md`, and the
   correspondence table with the witnessed conclusion.

Do not commit raw captures as evidence. The durable evidence is the structural
test plus the cited, sanitized conclusion.

## Focused regression commands

```powershell
ctest --test-dir build -C Release --output-on-failure -R '^npruntime_'
ctest --test-dir build -C Release --output-on-failure `
    -R '^(protocol_message|npruntime_server_session)$'
ctest --test-dir build -C Release --output-on-failure -R '^nw_golden_diff$'
```

Run the affected GUT network/session files after rebuilding the GDExtension.
Run the hook repository's `make -C onhook/tests run` from its MinGW/MSYS shell.
The capture-gated golden test skips when its environment variables are absent;
a green skip is not a retail-interoperability witness.
