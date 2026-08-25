# scripts/net — live net capture + iteration harness

Tooling so an agent can capture and decode real wire traffic for the four
retail-interop scenarios (`.agents/interop.md` packet-diff matrix), then diff
against a golden. Live capture here uses an external Npcap + dumpcap install.

## Two ways to record OUR side

`dumpcap` (this directory) records what reached the NIC, for any endpoint,
without touching the client. It is the instrument for anything involving the
network stack — fragmentation, retransmits, real timing — and it is what the
parity matrix and `diff_vs_golden.ps1` are built around.

**Self-capture** records from inside the client instead: set
`NW_CAPTURE_WRITE=<path.pcap>` before launching, and `UdpPump` appends every
datagram it sends or receives to a pcap in the same legacy/DLT_RAW shape the
retail-side hook writes. That symmetry is the point — a session recorded from
our client and one recorded from the original game decode through the same
`nw_pp` pipeline and compare per (direction, wire tag).

Reach for self-capture when you want the client's own view with no Npcap
install, no loopback adapter, and no interface guessing — including on a machine
where dumpcap is unavailable. Its limits are inherent, not incidental: it sees
only what this process moved (never a third party's traffic), it records the
payload the application handled rather than the frame that left the machine, and
its IP headers are synthesized, so checksums and fragmentation carry no
information. When the question is "which messages flow", it answers directly;
when the question is about the stack, use dumpcap.

All output goes to `<repo>\.scratch\` (gitignored). Never commit raw captures,
`.sph` logs, account names, local install paths, or machine IPs.

## Scripts

| Script | Purpose |
| --- | --- |
| `lib.ps1` | Shared helpers (repo/.scratch paths, run stamps, Godot + dumpcap discovery). Dot-sourced by the rest. |
| `detect_capture.ps1` | Verify Npcap + dumpcap and a loopback adapter. Run this first. |
| `capture.ps1` | `-Action start\|stop -Tag <t>` — dumpcap to `.scratch\<tag>-<stamp>.pcapng`. Defaults to the loopback adapter; `-Iface` for a real NIC. Stop validates every pcapng block through exact EOF and can require a final-packet tail marker. |
| `decode.ps1` | Decode a `.pcapng`/`.sph` with `nw_pp`. Finds the tool in Release or Debug. |
| `export_parity_corpus.py` | Create or verify `items.def`, `ammo.def`, and mission bytes through the engine-faithful mounted retail VFS; requires the SERVER and CLIENT resolutions, source archives, hashes, and bytes to agree. |
| `exercise_retail_input.ps1` | Focus one exact retail PID and record a create-new witness for the fixed W/D/A movement-and-turn trajectory used by exercised RR/OR runs. |
| `godot/tests/net/parity_joiner_driver.gd` | Tracked frame-driven OpenNova joiner used by verdict-bearing RO/OO runs; publishes readiness/motion evidence and performs request/ACK cooperative teardown. |
| `wait_parity_wire_ready.ps1` | Poll a copied snapshot of a rolling capture until exact session/roster/load milestones and sustained decoded bidirectional gameplay prove wire readiness; writes a create-new JSON witness. |
| `launch_retail.ps1` | Phase 0: launch stock `Jointops.exe` with `/connectlog` + `/profile` oracle flags (host/join still manual). Needs `-GameDir` or `$env:JO_GAME_DIR`. |
| `launch_retail_cfg.ps1` | Launch one isolated retail CLIENT or SERVER directly from the colocated `onhook.cfg`; infers the role, applies window/expansion settings, and supplies create-new hook log/capture metadata. `-PlanOnly` validates without launching. |
| `configs/retail-lan-{server,client}.onhook.cfg` | Sanitized role templates for the isolated retail copies and the cfg-driven OpenNova launchers. |
| `host_opennova.ps1` | Launch a visible OpenNova listen host and wait for a valid JO LAN discovery reply before returning. |
| `join_opennova.ps1` | Launch a visible OpenNova joiner for a host/IP, port, and callsign. |
| `run_lan_pair.ps1` | Launch an OpenNova host, wait for protocol readiness plus a bounded delay, then launch an OpenNova joiner. |
| `run_parity_topology.ps1` | Run one suite-bound RR, RO, OR, or OO topology from parameterized retail roots, hook MCP, and Godot launcher; writes create-new evidence under `.scratch\runs`. |
| `run_parity_matrix.ps1` | Preflight or capture the canonical six-case/four-topology suite from the tracked manifest template, binding the exact executable, artifact, and repository-source state into one create-new suite fingerprint. |
| `test_run_parity_matrix.ps1` | Self-test the matrix's public preflight contract: occupied suite paths fail before machine validation, and the runner verifies the documented VFS-exported corpus root. |
| `generate_parity_manifest.ps1` | Validate a completed tracked suite and generate its create-new ignored parity manifest from the captured evidence and suite provenance. |
| `diff_vs_golden.ps1` | `-Ours <our.pcapng> -Golden <golden.pcapng> [-Items <items.def>]` -- decode both with `nw_pp --histogram` and report per-(direction, wire tag) coverage: required GAP, event-conditioned absence, SPURIOUS S2C, and decode failure. Writes `<our>.vs-golden.txt`; exits non-zero on required gaps, unapproved S2C extras, or decode failures. |
| `verify_parity_matrix.ps1` | Verify a hash-pinned JSON manifest containing RR, RO, OR, and OO for every canonical case. It freshly decodes `nw_pp --parity-events` with hash-bound corpus inputs, compares both directions of RO, OR, and OO against that case's RR capture (including packet grouping and 0x0A/0x0C state), checks maintenance behavior, writes a machine JSON verdict, and exits nonzero on any mismatch. |

The cfg-driven four-way retail/OpenNova procedure, including the hook-owned
retail pair tool, lives in [`.agents/retail-lan-parity.md`](../../.agents/retail-lan-parity.md).

## Verifiable four-way matrix

The tracked suite harness and its machine-path parameters are documented in
the [retail LAN parity runbook](../../.agents/retail-lan-parity.md#6-run-the-comparison-matrix).
It keeps suite provenance, run summaries, captures, and generated manifests in
the ignored `.scratch` tree; no local retail or tool path belongs in a tracked
script.

[`parity-matrix.example.json`](parity-matrix.example.json) is tracked policy for
the runner, generator, and verifier; do not copy it and hand-fill run entries.
Do not copy loose, similarly named DEF or BMS files into the corpus either.
Export the bytes that the retail VFS actually resolves after mounting both
isolated installs, then verify the create-new export before launching the suite:

```powershell
$corpusArgs = @(
    '--server-root', $server, '--client-root', $client,
    '--expansion', 'revx02', '--output-dir', '.scratch\parity\corpus',
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

The exporter rejects different SERVER/CLIENT resolution owners or bytes. The
tracked runner and generator pin each case's `mission_artifact.path`/`sha256`;
every run repeats the actual `mission_path`/`mission_sha256` it loaded. The
verifier hashes every path,
requires the BMS leaf to equal `mission`, and rejects the same BMS bytes under
two case names. The canonical `03TR` case pins numeric Deathmatch `game_type=0`;
zero is a real UInt32 wire value, not an unpinned sentinel. Run entries are
generated from the suite's hash-bound capture/stop evidence.

The artifact inventory is deliberately broader than the exported data. It
hash-binds the mounted retail archives and executables in both role copies; the
hook, Godot launcher and resolved runtime executables, native OpenNova DLL,
`nw_pp`, LAN probe, and hook MCP;
the capture/launch/input/export/runner/generator/verifier scripts; the tracked
`godot/tests/net/parity_joiner_driver.gd`; both tracked cfg templates; and the
complete repo-native Godot `res://` input set selected by
`Get-GodotRuntimeArtifacts`. The joiner launch proof must use that hash-bound
file's exact absolute path as its single `--script` argument. The SERVER and
CLIENT retail mirrors must hash identically. Repository-owned artifact and
evidence paths use `repo:<path>` so
manifest location cannot silently retarget them; `.scratch/...` remains the
legacy repository-relative form, absolute paths are explicit machine inputs,
and other relative paths are manifest-relative. `corpus.source_state` binds the
bytes of every nonignored path reported by Git (tracked plus untracked) outside
the harness evidence/build roots independently of staging state, plus exact
clean submodule commits. Set `corpus.fingerprint` to SHA-256 of the sorted UTF-8
inventory lines `role=sha256` plus `repo_source_state=sha256` (all
LF-terminated). A source edit after capture therefore invalidates generation
and verification even when an old DLL still exists; preflight prints the digest
that capture will recompute. A mismatch prints the derived value.

Once a verdict-bearing capture starts, freeze every input through manifest
generation and verification: do not edit source or cfg/corpus files, update a
dependency, rebuild, redeploy the hook/GDExtension, or replace a launcher/tool.
Any change invalidates the suite; rerun preflight and all 24 topology/case runs
with a new `SuitePrefix`.

Every run has a unique canonical `run_id` and repeats the enclosing `suite_id`.
Every case and run also repeat an exact `readiness_mode`: `in_match` for
local-player/motion cases or `deploy_hold` for a player-paced deployment
control. This is case metadata, never a mission-name special case. A deploy-hold
retail client may remain `phase=loading` with no rendered local `GamePerson`
while ordinary C2S `0x0C` and S2C `0x0A` continue; the matrix therefore binds a
decoded wire-readiness witness before steady start instead of treating onHook's
presentation phase as transport state.
Its `effective_configs` are topology-specific: RR has server and client, RO has
server, OR has client, and OO has none. Each snapshot pins its full hash plus a
`template_role` and `base_sha256`. Base normalization removes only MCP-managed
per-run role keys; every remaining canonical byte must match the corresponding
hash-bound tracked cfg template. Dynamic host/join fields are then checked
exactly against the case metadata.
The repeated mission path/hash, game type, resolution, port, callsigns, corpus
hash, and expansion are intentional: the verifier rejects a mixed capture that
is not bound to the same inputs as its case's RR golden.

The current onHook retail pair automation discovers UDP `32786..32789`.
Every case must use a port in that window, and the six-case template spans all
four distinct ports (its `minimum_distinct_ports` bound) so a fixed launch
config cannot pass. This is a harness
contract in addition to the runtime's broader configurable LAN-port range: a
manifest the automated retail joiner cannot discover is not parity evidence.

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

The default create-new report is
`.scratch\parity-final\$suite.manifest.json.parity.json`; it records the
manifest, derived
artifact and repository-source inventory, per-case BMS hashes, capture hashes,
fresh decode/state counts, RR-derived CRC
oracles, motion witnesses, failures, and explicit limits. `-Force` is required
to replace an existing report. Capture paths and capture SHA-256 values must
both be unique across every case/topology, so copied bytes cannot masquerade as
independent evidence. A prior
`*.vs-golden.txt` sidecar is never read, so a missing, stale, or wrong-case
coverage diff cannot become acceptance evidence.

`steady_started_utc` and `steady_completed_utc` are strict UTC `Z` timestamps
recorded only after both endpoints are in match. Endpoint survival must cover
at least `steady_seconds` and no more than 15 seconds beyond it. The verifier
slices packets, events, and decoded state to exactly
`[steady_started_utc, steady_started_utc + steady_seconds]`; the extra survival
time is health evidence, not comparison padding. Both directions need decoded
gameplay within the first and last 500 ms of that slice and may have no internal
gameplay gap over one second. The separate initial reliable prefix remains
verdict-bearing even when it precedes the steady slice.

Exercised cases run one elapsed-time trajectory on either implementation. After
a 250 ms focus/pre-roll interval, hold W for 1800 ms while turning +320, refocus
or wait 250 ms, hold D for 1200 ms while turning -320, refocus or wait 250 ms,
then hold A for 900 ms while turning +160. Each phase distributes its turn over
exactly 20 nonzero samples. Retail RR/OR input is PID- and window-bound and
writes a create-new, hash-pinned `opennova.retail-input.v1` completion witness.
The helper does not call `SendInput` until the HWND is proved to belong to the
exact retail PID and remain foreground after a settling check. It repeats those
owner/focus checks before and after every key/mouse insertion and wait; a stale
window, focus loss, owner change, short insertion, or retail exit fails closed
without a complete witness. The runner, generator, and verifier reload the
artifact, bind its SHA-256, run/topology, process start, PID, HWND, timestamps,
trajectory and counters to the accepted retail joiner, and require the exercise
wholly inside the steady window. The OpenNova joiner in RO/OO publishes
fresh readiness heartbeats but cannot start motion until the runner creates a
run/PID/topology-bound gate immediately after steady start; its normalized gate
and completed-motion witness are retained in the manifest. This prevents setup
or focus delay from being mistaken for an implementation wire difference.

Verdict-bearing RO/OO runs must also stop the exact OpenNova joiner through the
tracked driver's cooperative request/ACK contract. After steady completion the
runner atomically creates `joiner-stop-request.json` with the exact
`opennova.parity-joiner-stop-request.v1` fields. The driver validates its run,
topology, PID, and strict UTC request time, releases the world/game, atomically
writes an `opennova.parity-joiner-shutdown.v1` ACK that echoes the request time,
and exits with code zero. The ACK must be `clean=true`, bind the exact launched
runtime PID, and arrive with process exit within 20 seconds. The manifest pins
both create-new files and SHA-256 values; generator and verifier independently
reload them and enforce exact fields, hashes, PID/run/topology identity, request
time at or after steady completion, and ACK completion within the bound.

The runner attempts cooperative shutdown on every normal RO/OO path and in
`finally` before exact-process fallback. Missing, tampered, late, mismatched, or
nonzero-exit evidence fails. Any fallback is recorded as
`opennova_joiner_exact_fallback_required=true`; verdict-bearing evidence
requires it to be exactly false and requires the normalized shutdown witness.
RR/OR must not carry either manifest field.

OR/OO also require the exact OpenNova host runtime to accept a normal
`CloseMainWindow()` request and exit within 20 seconds with retained exit code
exactly zero. A force-killed runtime or wrapper job is not evidence: any exact
fallback fails the run. The `opennova.parity-host-shutdown.v1` witness binds
the run ID, topology, exact launch-proof runtime PID, strict request/completion
timestamps, accepted close, exit code, and `clean=true`; the request must be at
or after process start and steady completion.

That witness also pins the exact
`repo:.scratch/runs/<run_id>/host/godot.log` and SHA-256. After exit, the runner,
manifest generator, and verifier independently require a nonempty, BOM-safe
log without ObjectDB, resource, RenderingServer, or RID teardown-leak
diagnostics. The manifest and verifier require
`opennova_host_shutdown_witness` together with
`opennova_host_exact_fallback_required=false` if and only if the topology is OR
or OO. RR/RO must carry neither field; PID/time mismatch, rejected close,
nonzero exit, log/hash failure, forbidden diagnostics, or fallback is fatal.

All four topologies also pass `wait_parity_wire_ready.ps1` against the owned
rolling dumpcap stream before steady start. It copies rather than decodes the
actively written file (onHook's in-process pcap is exclusively locked until
drain) and requires exact recipient-scoped S2C
`0x7B` session/joiner/map/game/expansion identity. The advertised mission must
remain stable and equal either the exact map filename or the printable title
from the hash-bound BMS header; exactly one S2C `0x0B` must equal that header's
616 bytes. The final RR-relative material comparison still compares `0x7B`
byte-for-byte, so this readiness gate cannot conceal a wrong OpenNova title
branch. It also requires both callsigns in name-bearing S2C `0x46`, C2S `0x0B`,
S2C `0x0F`, a decoded nonzero C2S `0x0C` handle with a
matching S2C player entity, decoded/local S2C `0x0A`, at least 40 samples per
gameplay direction spanning ten seconds, and 20 matched `0x2C`/`0x57` liveness
tokens. The completed external capture is independently checked against the
hash-pinned prefix witness, and its session/SIDs must equal the connection in
the completed retail-perspective steady capture. A peer boolean or raw packet
count cannot start the comparison window.

Both directions are verdict-bearing in each mixed run. RO compares the
OpenNova client's C2S surface with the same-case retail client and also proves
that the retail host's S2C reaction remains retail-like; OR checks the OpenNova
host's S2C surface and the retail client's C2S reaction. The verifier compares
the advertised `server_name` separately from both player callsigns. Every case
and run pins it byte for byte, and `host_session_name` must equal it. Retail's
default wire value is exactly `"Untitled "` (including its trailing space);
`host_callsign` identifies the host player and must never be substituted for
it. Decoded S2C `0x7B` records must preserve that exact `server_name` and
identify the recipient joiner. Retail can omit the host's recipient-scoped
`0x7B` from a client trace, so name-bearing S2C `0x46` slot records—not an
invented second `0x7B`—must identify both host and joiner callsigns. The verifier also compares
initial packet record grouping and outer packet flags, tags/counts/order. Each
ordered physical record carries its exact raw flag byte, LEN8/LEN16 choice,
encoded payload length, discarded SKIP bytes, and fragment bits from npwire's
shared parser; missing the current `wire=` contract or changing any one of
those fields fails closed. Every nonzero ACK must name a previously observed
peer sequence, never regress, and materially progress in both directions.
Outer-handshake retries must decode and remain field/body exact; all four
required messages bind the unique gameplay session, while incomplete
0x41/0x81-only readiness probes are classified separately. ServerAuth must
echo ServerHello RIP/RPN. Decoded S2C `0x0A` `flags2` starts at the
pre-incremented value 1 and advances exactly modulo 256. Every run covers at
least 128 distinct values, and any 257-sample steady slice must contain the
255-to-0 edge. Weapon/timer/environment/objective/phase-8 presence must match
the phase byte and numeric game type. The verifier also compares decoded C2S
`0x0C` plus S2C `0x0A` state shapes/classes/types/header flags plus RR-relative carrier,
orientation, movement/state, analog, anchor/local-tail, and compact-entity
fields. `nw_pp --parity-events` emits the declared length and full payload for
every identity through `0x1ff` except ordinary gameplay C2S `0x0C` and S2C
`0x0A`, whose decoded state records are compared instead; high-table settings
identities remain material. Initial material payload sequences compare
byte-for-byte after normalizing the live S2C `0x0F` tick and routing explicitly
dynamic crypto/challenge/clock records to their stateful checks. Stable
team/deployment/loadout bodies compare exact sets. Other material streams still
fail if varied RR bytes collapse to one constant body or nonzero RR bytes become
all zero, so matching tags and lengths cannot hide hardcoding.

The generic packet-group, adjacent-event, tag-count, and material-body
comparison spans the selected connection's first decoded packet through the
declared steady end. This deliberately includes the readiness warmup before
`steady_started_utc`, so a transient or delayed OpenNova-only record cannot hide
between readiness and the steady slice. Steady liveness/density and dynamic
state checks remain restricted to the declared steady interval. SID-bound
distributions require at least 8 samples, a 2% material-share floor, and
8-point absolute and 50% RR-relative share tolerance. Dynamic streams compare
the initial C2S position/S2C anchor within 64 fixed-point world units, then
origin-normalized range, displacement, nonzero-step scale/shape, and short-cycle
rates. Independent input may vary by up to an 8x scale band; a 2..8-sample cycle
present in more than 60% of moving samples fails when RR does not share it.
Every topology must witness changing player and vehicle state somewhere in the
suite; an all-zero, constant, unclassifiable, or unsupported state stream fails
closed. `require_analog=false` does not claim a live occupied-vehicle analog-axis
witness: it proves the exercised on-foot trajectory and whatever AI/map vehicle
traffic the case contains, while occupied-vehicle analog behavior remains
structural regression evidence unless a case is deliberately run with
`require_analog=true`. Packet replay/origin identity uses client port + encrypted SID +
sequence, so a reconnect cannot masquerade as a NACK. Hook captures set
`capture_stats_available=true` and must report all four counters as exact zero.
Counter-less dumpcap evidence is allowed only for OO/external, sets the flag
false, and omits the counters. After both OO endpoints stop and the game port is
proved unowned, the harness sends a unique UTF-8
`OPENNOVA-PCAPNG-TAIL|<run_id>|<32-lowercase-hex>` datagram. Capture stop parses
every pcapng block, validates byte order, lengths and trailing lengths, requires
exact EOF, and requires that marker in the final packet block. The manifest
pins its text, hex, SHA-256, send time, artifact hash, and exact block counts;
the verifier independently reparses the capture and requires the send time not
to precede steady completion or either endpoint's clean shutdown completion.
OO must also pass raw decode, nonzero SID/epoch,
gap-free per-direction sequence, and one-for-one packet-group/event checks. It
is compared bidirectionally with the same-case RR oracle, while remaining a
control that cannot replace mixed RO/OR evidence. Set `maintenance=true` only
on cases long enough to witness at least four alternating integrity events.
Require the later control schedule separately with `control_mode=required`; a
deployment-hold case can use `maintenance=true` with `control_mode=forbidden`
to prove integrity continues before control maturity. Short map/game-type
diversity cases still
receive identity, strict bidirectional, CRC, grouping, ordering, count, and
semantic checks. Verdict-bearing manifests require empty exception arrays;
missing, extra, count, length, or body differences cannot be waived.

The JSON report's `occupied_mount_coverage` deliberately remains
`status=structural-only`: the canonical W/A/D plus relative-look trajectory has
no deterministic seat acquisition. It reports phase-8 and incidental bound
sample counts without promoting them to controlled live evidence and names
`netsim_two_peer_fanout` as the handle/clip/reserve structural witness.

Maintenance verification requires transient S2C `0x16` immediately before each
alternating `0x30`/`0x31` request. It phase-normalizes that pair because retail's
process-global phase advances without a recipient and survives mission reset.
The control gate is no earlier than `Server_TickUpdate` call 1,861 (the age is
checked before increment), followed by the 744-tick cadence. Each healthy
quartet must receive one grouped C2S `0x1C`/`0x08`/optional-`0x3D` reply before
the next quartet; the `0x08` echoes the sequence and the seed-normalized `0x1C`
must match the same-case RR oracle. Exact counter/mission-reset behavior and
timing-invalid `0x08` acceptance remain native regression evidence.

## OpenNova LAN host/join (Phase 3)

These helpers launch the Godot game project at `<repo>\godot`. They find Godot
through `$env:GODOT_BIN` or the repo's `.godot-bin` convention used by the other
network scripts.

**First-run prerequisite:** launch OpenNova normally, select the Joint Operations
resource directory, and close it before using these helpers. The selection is
persistent user state; the scripts deliberately do not select or copy game data.
The host must resolve the complete mission. A normal joiner need not possess the
advertised `.bms`; it resolves shared terrain/environment/model assets named by
the exact S2C `0x0B` header and receives entity rows plus the optional S2C `0x45`
terrain overlay from the host.

Build the native readiness probe once per build tree:

```powershell
cmake --build build --config Release --target opennova_nw_lan_probe
```

The host launcher does not equate process creation or a bound UDP socket with
readiness. By default it repeatedly sends a stock `0x41` discovery probe and
must decode a valid `0x81` game-server reply before it prints
`OPENNOVA_HOST_READY=True` and returns. This matters for large synchronous
mission loads: `00TRg.bms` has taken more than 90 seconds on a cold run. The
defaults are a 120-second deadline and a one-second interval; cfg-driven runs
can set `LanHostReadyTimeoutMs` and `LanHostReadyProbeIntervalMs`.

When the selected Godot executable is the official `_console.exe` wrapper, the
launch module keeps that wrapper alive so its `KILL_ON_JOB_CLOSE` job continues
to own the engine tree. It returns the exact GUI runtime process only after
proving direct parentage, executable path, argument vector, start identity, and
active execution. `-PassThru` callers receive that runtime with
`OpenNovaLauncherProcess` and a serializable `OpenNovaLaunchProof` attached.
Close such a process with `Stop-OpenNovaLanProcess -Process $process`; forced
cleanup targets the wrapper and therefore the complete job, not merely one
runtime PID. The parity matrix records this proof for every OpenNova role and
rejects direct-wrapper bypass or a proof whose paths do not match its hash-bound
Godot launcher/runtime artifacts.

Start the two roles separately (host first):

```powershell
pwsh -File scripts\net\host_opennova.ps1 `
    -Mission ASH_I5A.BMS -Name Host -Port 32768 `
    -Windowed -Resolution 1920x1080

pwsh -File scripts\net\join_opennova.ps1 `
    -Host 127.0.0.1 -Name Joiner -Port 32768 `
    -Windowed -Resolution 1920x1080
```

Or launch a two-instance localhost pair in one command. For a parity run,
`-IntegrityProfile` is applied to both roles: the client answers retail-style
integrity challenges and the host validates the corresponding replies against
that same exact corpus.

```powershell
pwsh -File scripts\net\run_lan_pair.ps1 -Mission ASH_I5A.BMS `
    -Windowed -Resolution 1920x1080
```

`run_lan_pair.ps1` starts the host, waits for protocol readiness, waits another
1500 ms, then starts the joiner. Use `-JoinDelayMs` to adjust that bounded
0-10000 ms post-readiness delay. Pass a machine's LAN IPv4
address or hostname with `-Host` when the joiner is not dialing localhost, and
allow inbound UDP on the selected port (32768 by default).

For PR #403's isolated retail copies, both mixed endpoints can be launched from
their colocated role cfg files. SERVER configures the OpenNova host; CLIENT
configures stock retail autojoin, windowing, and expansion:

```powershell
$run = '.scratch\runs\pr403-cfg-e2e'
New-Item -ItemType Directory -Force "$run\host","$run\joiner" | Out-Null

& powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File scripts\net\host_opennova.ps1 `
  -HookConfig .scratch\SERVER\onhook.cfg `
  -LogFile "$run\host\godot.log"

& powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File scripts\net\launch_retail_cfg.ps1 `
  -GameDir .scratch\CLIENT `
  -RunId pr403-cfg-e2e-joiner `
  -OutputDir "$run\joiner"
```

`launch_retail_cfg.ps1` requires exactly one role in the cfg, uses the exact
hook metadata name `ONHOOK_RUN_ROLE`, clears inherited `NW_LAN_*` values for the
child, and launches CLIENT as `/w /many /exp revx02` for the checked-in parity
configuration. `-PlanOnly` validates and prints the resolved role/arguments
without creating artifacts or launching retail.

The normal join launch contract contains only the endpoint and callsign; the LAN
session supplies its own mission metadata. `join_opennova.ps1 -MissionOverride
ASH_I5A.BMS` (or `run_lan_pair.ps1 -JoinMissionOverride ASH_I5A.BMS`) sets
`NW_LAN_MISSION`, a debug-only expectation/display hint — the runtime never
uses it as a local load override (D-NET-194) and it is not part of normal LAN
discovery.

Each launcher clears inherited `NW_LAN_*` variables before starting its child and
restores the caller's environment immediately afterward, so a stale host variable
cannot turn the joiner into a second host. The scripts print the runtime and
launcher PIDs separately.
Alternate online-service, replay, and single-player launch overrides are also
suppressed in the child, keeping these helpers strictly on the local-LAN path; the
caller's values are restored. Add `-Wait` to keep the launcher attached until its
child exits.

## Multi-interface dumpcap gotchas (learned 2026-07-02)

- A `-f <filter>` argument binds to the **preceding** `-i`; a filter given BEFORE any `-i`
  becomes the default for all. `dumpcap -i A -i B -f udp` filters ONLY B. For multi-interface
  captures put `-f` before each `-i` (or before all of them).
- Same-PC retail-client↔host sessions addressed to the machine's LAN IP do not reliably appear
  on any single adapter — one round captured fully, an identical setup caught nothing. ALWAYS
  sanity-check mid-round: copy the rolling pcapng and run `nw_pp --histogram` on the copy the
  moment the join completes; re-arm on more interfaces if the session is absent. A rolling
  dumpcap file is readable while capture continues (copy first, decode the copy).
- diff_0a.py caches a `<capture>.0a.json` sidecar NEXT TO each input — keep golden captures
  inside the current worktree's `.scratch/` so `--refresh` never writes into a sibling worktree.

## Prerequisites (verified on this machine 2026-06-26)

- Wireshark/dumpcap at `C:\Program Files\Wireshark\dumpcap.exe`; Npcap running with
  the loopback adapter `\Device\NPF_Loopback` present → `CAPTURE_READY=loopback`.
- `nw_pp` reads dumpcap's loopback-adapter pcapng directly (link-type handled by
  `engine/base/pcapio/pcap_reader`). Build the tools with `scripts/build.sh`; binaries land
  in `build\apps\nw_pp\{Release|Debug}\`.
- Pass dumpcap a device **name** (`\Device\NPF_Loopback`), not the `dumpcap -D`
  ordinal — the ordinal is rejected.
- `$env:JO_GAME_DIR` = folder containing `Jointops.exe` (machine-local; set in
  `.claude/settings.local.json` env, never tracked).

## Phase 0 manual golden spike (retail ↔ retail)

```powershell
pwsh -File scripts\net\detect_capture.ps1                       # expect CAPTURE_READY=loopback
pwsh -File scripts\net\capture.ps1 -Action start -Tag retail-ref
pwsh -File scripts\net\launch_retail.ps1 -Tag host             # then: Multiplayer -> Host LAN
pwsh -File scripts\net\launch_retail.ps1 -Tag join             # then: Multiplayer -> Join the host
# play through deploy, then:
pwsh -File scripts\net\capture.ps1 -Action stop -Tag retail-ref
pwsh -File scripts\net\decode.ps1 -Capture <printed CAPTURE_FILE> -Roles
```

If LAN discovery does not work over the loopback adapter, capture a real NIC
instead (`capture.ps1 -Iface <\Device\NPF_{...}>`) — a process still sees its own
subnet broadcasts on one box. Decode should show the handshake (`0x41/0x42/0x43`),
world load (`0x0B/0x10/0x0D/0x0C/0x20/0x42/0x0A/0x0F`), and C2S `0x0C` deploy.
Promote a sanitized golden under `.scratch\golden\`.

## Reference captures (`.scratch/golden/`, local-only — never commit)

The golden bank the `NW_GOLDEN_*` env vars point at (gating matrix:
`docs/asset-gated-tests.md`). Decode with `--items <ITEMS.DEF>` (use
`~/Desktop/JOX/ITEMS.DEF`) — without it the `0x0a` per-frame-update records can't be
length-resolved and print `(DECODE INCOMPLETE)`.

Current bank — the 2026-08-05 retail-LAN parity round (mission 01TR, hash-bound
wire-ready captures; runbook `.agents/retail-lan-parity.md`):

| File | Contents |
| --- | --- |
| `retail-lan-01tr-join-20260805.pcapng` | Join reference (`NW_GOLDEN_LAN_JOIN`). |
| `retail-lan-01tr-join-session-20260805.pcap` | Same join, session-only (`NW_GOLDEN_LAN_JOIN_SESSION`). |
| `retail-gameplay-01tr-20260805.pcap` | Retail↔retail in-match reference (`NW_GOLDEN_GAMEPLAY`). |
| `opennova-host-retail-client-01tr-20260805.pcap` | OpenNova host + retail client (`NW_GOLDEN_OURS`, the "ours" side of `nw_golden_diff`). |

The 2026-06-26 trio (`retail-lan-host-join.pcapng` TDH_I5A join,
`retail-lan-host-join-session.pcapng` session-only, `retail-gameplay-session.pcapng`
~8 min ASH_G3D co-op) is retained beside it and remains the compiled-in `DEFAULT_*`
fallback in `tests/CMakeLists.txt` when the env vars are unset;
`retail-vehicle-session.pcapng` feeds the netsim capture-parent-follow gate. Each
capture has a decode sidecar (regenerate with `decode.ps1 -Items <ITEMS.DEF>`); the
2026-08-05 bank also carries `retail-lan-parity-20260805.NOTES.md`. These are the
oracle side of `diff_vs_golden.ps1`.

## Capture + validate-vs-golden loop (the consolidation harness)

Capture the game traffic between a retail client and OUR Godot game host, then diff
its message coverage against a golden retail capture. The game server is the opennova
Godot game (it owns the in-match `World` + the 62 Hz host loop); the docker NovaWorld
stack is only matchmaking / NAT rendezvous, so capture the game session itself.

```powershell
pwsh -File scripts\net\detect_capture.ps1                      # expect CAPTURE_READY=loopback
pwsh -File scripts\net\capture.ps1 -Action start -Tag ov-host  # dumpcap (loopback; -All for a real NIC)
# bring up matchmaking, host from the Godot game, join from the retail client, play, deploy:
#   docker compose -f deploy/compose/docker-compose.yml -f deploy/compose/docker-compose.dev.yml up --build
#   (Godot game) Multiplayer -> Host ;  (retail client) browse NovaWorld -> Join
pwsh -File scripts\net\capture.ps1 -Action stop -Tag ov-host
pwsh -File scripts\net\diff_vs_golden.ps1 `
     -Ours <printed CAPTURE_FILE> `
     -Golden .scratch\golden\retail-gameplay-session.pcapng `
     -Items ~\Desktop\JOX\ITEMS.DEF
```

`diff_vs_golden.ps1` prints the coverage table and writes `<Ours>.vs-golden.txt`:
the **GAP** rows are the prioritized worklist for aligning the server (replication
tags retail sends that ours doesn't), **STATE_ABSENT** rows are event-conditioned
messages the shorter run did not exercise, **SPURIOUS** S2C rows are wire-illegal
regressions, and **DECODE FAILURES** are our own malformed emissions. Client-only
extras are informational in this exploratory coverage tool because the retail
peer owns them. They are failures in the RO client-direction check performed by
`verify_parity_matrix.ps1`. Pass `--items` so the per
-frame `0x0A` records length-resolve (without it they print `DECODE INCOMPLETE` and
inflate the failure count).

To pin a result in CI, point the env-gated `nw_golden_diff` ctest at the captured
file (it skips clean when unset, like the other capture-gated tests):

```bash
NW_GOLDEN_OURS=.scratch/ov-host-<stamp>.pcapng \
  ctest --test-dir build -C Release -R nw_golden_diff --output-on-failure
```

It fails the build on any required GAP or any unapproved spurious S2C tag;
event-conditioned tags and knowingly deferred tags have explicit policy tables
(with `D-NET-*` references) in
`tests/novaworld/nw_golden_diff_test.cpp`, never silently skipped. `nw_pp
--histogram <capture>` emits the same per-(dir,tag) `HIST` lines by hand.

`diff_vs_golden.ps1` is a per-TAG coverage diff (which messages flow). For the
in-match replication core, `diff_0a.py` is a per-FIELD **shape** diff of just the
S2C `0x0A` stream — the two captures are different sessions, so it compares what
should match between any two hosts on the same map rather than raw bytes:

```bash
python scripts/net/diff_0a.py \
    --ours   .scratch/ov-<stamp>.pcapng \
    --golden .scratch/retail-ashi5a-<stamp>.pcapng \
    --items ~/Desktop/JOX/ITEMS.DEF
```

It reports the sub-block cycle (golden rotates `flags2` low-2 through aim/timer/env/
gametype), the record-class mix (golden replicates vehicles; a `Vehicle ours=0`
row means we send none), the header field value-sets, and per-record-class field
population (a field golden always fills but we leave zero is an under-send). The
golden's parsed profile caches to `<golden>.0a.json`, so iterating on our encoder
only re-decodes our own capture; pass `--refresh` after a decoder change.
