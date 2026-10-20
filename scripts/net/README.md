# Network interoperability helpers

These PowerShell scripts support local LAN, capture, and retail-interoperability
work. They are operator tools, not release inputs or CI gates. Captures and
retail data stay machine-local and gitignored and must never be
committed without the sanitization review described in
[`docs/test-suite.md`](../../docs/test-suite.md).

The parity verdict is `opennova-parity` (C++, `apps/parity`), run per case by
`run_parity_suite.ps1` ("Parity verdict" below). It judges the runs it is given; it
does not hash-bind binaries, retail trees or a corpus.

## Tools

| Script | Purpose |
| --- | --- |
| `detect_capture.ps1` | Locate `dumpcap` and list usable capture interfaces. |
| `capture.ps1` | Start or stop a tagged local packet capture. |
| `decode.ps1` | Decode a capture with the native `opennova-wire` application. |
| `diff_0a.py` | Per-FIELD shape diff of the S2C 0x0A stream vs a retail-host golden (sub-block cycle, record-class mix, field population); caches `<golden>.0a.json` beside the input. |
| `read_integrity_profile.py` | Read a running retail `Jointops.exe`'s ammo-definition CRC rows (read-only, Windows; D-NET-181): the oracle the computed integrity rows must reproduce (`tests/npruntime/integrity_oracles.h`, the `npruntime_integrity_sources` ctest). |
| `host_opennova.ps1` | Start an OpenNova LAN host on launch flags, await LAN discovery and its MCP endpoint. |
| `join_opennova.ps1` | Start an OpenNova LAN joiner on launch flags with its MCP endpoint. |
| `run_lan_pair.ps1` | Start a local OpenNova host/joiner pair (MCP ports 8975/8976). |
| `run_parity_topology.ps1` | Run one explicitly configured live topology, optionally playing a scenario (`-Scenario`). |
| `run_parity_suite.ps1` | Run every case of `parity-cases.json` as RR, RR-aa, RO, OR and OO cells (resumable), then judge each case with `opennova-parity`. |
| `parity_preflight.ps1` | Read-only check of everything a suite binds (retail copies, hook pair, `game.cfg`, corpus pins, Godot, capture, judges, the CI GDExtension, ports); run it before a suite. |
| `diff_verdicts.py` | Compare two suites' verdicts row by row (FIXED, REGRESSED, PERSISTS, NEW, GONE, SKIP-CHANGE), with the widening report, the D-NET attribution and the minting queue. |
| `parity-attribution.json` | The verdict-row to D-NET attribution table (`opennova.parity-attribution.v1`), transcribed from `docs/net/lan-parity-verdict.md`. |
| `parity-cases.json` | The tracked parity case table (`opennova.parity-cases.v1`). |
| `compare_scenario.py` | Check scenario runs against their expectations and against the RR reference. |
| `wait_parity_wire_ready.ps1` | Wait for and validate the topology's live wire-ready state. |

Shared process and path helpers live in `lib.ps1` (which dot-sources the
opennova-game MCP client, `scripts/mcp/game_mcp.ps1`); retail instances are
launched only through onhook-mcp.

## Build native helpers

```powershell
cmake --build build --config Release --target opennova_wire opennova_lan_probe opennova_parity
```

The scripts locate binaries in the Release or Debug output directories.

## Capture and decode

```powershell
pwsh -File scripts\net\detect_capture.ps1
pwsh -File scripts\net\capture.ps1 -Action start -Tag retail-ref
# Run the session, then stop with the same tag the start command used.
pwsh -File scripts\net\capture.ps1 -Action stop -Tag retail-ref
pwsh -File scripts\net\decode.ps1 -Capture <capture.pcapng> -Items <items.def> -Stream
```

NovaWorld traffic can contain credentials, account identity, and machine paths.
Treat every `.pcap`, `.pcapng`, and `.sph` file as sensitive until reviewed.

## OpenNova LAN host/join

The launchers find Godot through `GODOT_BIN` or the repo's `.godot-bin`
convention. Pass the retail resource directory with `-ResourceDir`
(`-HostResourceDir`/`-JoinResourceDir` for the pair): without a `--resource-dir`
the game boots its bundled menu and ignores the LAN launch flags (ADR 0048).
Each game starts behind every other window and never takes the foreground, and
the console wrapper's console gets no window (`Start-OpenNovaProcess`;
`docs/mcp.md` "Launching"); `-Front` is an ordinary start.

```powershell
pwsh -File scripts\net\host_opennova.ps1 `
    -Mission ASH_I5A.BMS -Name Host -Port 32768 -ResourceDir <jo-dir> `
    -Windowed -Resolution 1920x1080

pwsh -File scripts\net\join_opennova.ps1 `
    -Host 127.0.0.1 -Name Joiner -Port 32768 -ResourceDir <jo-dir> `
    -Windowed -Resolution 1920x1080
```

Or start a local pair:

```powershell
pwsh -File scripts\net\run_lan_pair.ps1 -Mission ASH_I5A.BMS `
    -HostResourceDir <jo-dir> -JoinResourceDir <jo-dir> `
    -Windowed -Resolution 1920x1080
```

Use a reachable LAN address instead of `127.0.0.1` for a second machine, and
allow inbound UDP on the selected port. The host waits for a valid discovery
reply before reporting ready; process creation or a bound socket alone is not
sufficient.

## Coverage check

`opennova-wire --coverage <capture>` ranks a capture's undecoded backlog by volume; the
automated native coverage is the `nw_*`, `npruntime_*` and `netsim_*` ctests
over the committed `fixtures/novaworld/` set and the inline-pcap unit tests.

## 0x0A shape diff

For the in-match replication core, `diff_0a.py` is a per-FIELD **shape** diff
of just the S2C `0x0A` stream: the two captures are different sessions, so it compares what
should match between any two hosts on the same map rather than raw bytes:

```bash
python scripts/net/diff_0a.py \
    --ours   ov-<stamp>.pcapng \
    --golden retail-ashi5a-<stamp>.pcapng \
    --items ~/Desktop/JOX/ITEMS.DEF
```

It reports the sub-block cycle (golden rotates `flags2` low-2 through aim/timer/env/
gametype), the record-class mix (golden replicates vehicles; a `Vehicle ours=0`
row means we send none), the header field value-sets, and per-record-class field
population (a field golden always fills but we leave zero is an under-send). The
golden's parsed profile caches to `<golden>.0a.json`, so iterating on our encoder
only re-decodes our own capture; pass `--refresh` after a decoder change.
The report always exits 0; `opennova-parity` carries the same sub-block,
record-class, header-value and field-population checks as failing metrics.

## Parity verdict (ADR 0050 R4)

`opennova-parity` judges one parity case: every subject topology (RO, OR, OO)
against the same case's RR run, in both directions, with a second
retail-vs-retail run (RR-aa) as the noise floor.

```powershell
build\apps\parity\Release\opennova-parity.exe --items <corpus>\items.def `
    --rr .scratch\runs\<prefix>-<case>-rr --rr-aa .scratch\runs\<prefix>-<case>-rr-aa `
    --ro .scratch\runs\<prefix>-<case>-ro --or .scratch\runs\<prefix>-<case>-or `
    --oo .scratch\runs\<prefix>-<case>-oo --out <case>.verdict.json
```

Each argument is a `run_parity_topology.ps1` run directory. Its
`run-summary.json` names the evidence capture (the retail endpoint's capture for
RR, RO and OR, the external one for OO; `--evidence external` takes every run's
`external.pcapng` instead) and the steady window. A run directory moved since
its run still resolves: a capture recorded under the old run root is found
under the new one. The capture decodes into the canonical trace, the records
`opennova-wire --parity-events` prints (`apps/wire/parity_trace.h`), and binds
to its one gameplay session.

The windows: the steady window is `steady_started_utc` plus `steady_seconds`.
The comparison interval runs from the session's first packet to the steady
window's end. The initial prefix is each direction's packets through its first
gameplay packet (C2S 0x0C, S2C 0x0A). An onHook capture stamps a datagram when
the process reads it, so through a blocking load its stamps bunch: wire timing
(the scoreboard cadence) reads every run's `external.pcapng` (the summary's
`external_capture`), whatever the evidence capture.

A subject slot also takes an RR run: `--rr <rr-aa> --rr-aa <rr> --ro <rr>` is the
A/A sanity and must pass (its only failures can be evidence findings).

The A/A rule: a metric's distance from RR fails when it exceeds
`max(fixed floor, distance(RR-aa, RR))`, each metric's fixed floor its own
(`apps/parity/metric.h`). The A/A distances form the verdict's `aa` section,
`pass` or `widened` against the fixed floors alone: where retail's own
run-to-run noise exceeds a floor, the floor widens to it for every subject and
the report says so.

| Metric family | Distance | Floor | Finding codes |
| --- | --- | --- | --- |
| Tag histogram (comparison interval, per `tag:settings`) | presence 0/1; count vs RR rate-normalized (when RR has 20 or more); length-set (but S2C 0x10 and C2S 0x3D) and stable-body-set Jaccard | 0; `max(1, ceil 20%)` for counts | `MISSING_TAG`, `UNEXPECTED_TAG`, `COUNT_MISMATCH`, `LENGTH_SET_MISMATCH`, `LENGTH_CONSTANT`, `BODY_CONSTANT`, `BODY_ALL_ZERO`, `STABLE_BODY_MISMATCH`, `FIRST_ORDER_MISMATCH` |
| Initial stream | edit distance over origin-packet groups (`flags\|tags\|raw/LEN/SKIP`, timer-phase identities left out, gameplay / world-page / fragment lengths `*`; C2S as one record sequence); bodies by position over the common prefix and their count; S2C 0x16 / 0x46 as sets of distinct (decoded) bodies; S2C 0x0C / 0x10 decoded and compared by slot | 0; the count rule for counts | `INITIAL_PACKET_GROUPING`, `INITIAL_BODY_MISMATCH`, `INITIAL_COUNT_MISMATCH`, `INITIAL_RECORD_MISMATCH` |
| Steady distributions (after the first gameplay packet) | largest share delta over keys of at least 2% share, in units of `max(0.08, RR share / 2)`; a packet groups as its record multiset, a gameplay packet without the S2C 0x57 replies it carries | 1 | `GAMEPLAY_GROUP_DISTRIBUTION`, `GAMEPLAY_ORDER_DISTRIBUTION`, `GAMEPLAY_DISTRIBUTION_UNEXERCISED` |
| Session packet envelope | class presence 0/1; packet and header-only cadence vs RR rate-normalized; header-only flag set; shape distribution | 0; `max(1, ceil 20%)`; 0; 1 | `SESSION_PACKET_CADENCE`, `SESSION_PACKET_HEADER_FLAGS`, `SESSION_PACKET_SHAPE_DISTRIBUTION` |
| Outer handshake (first 0x41/0x42, 0x81/0x82) | edit distance over field keys, dynamic values masked or reduced (SCRK's size as 62-63, CU CI0/CI1 as the low 16 bits); whether the auth SCRK walks the manager LCG's cycle; each opcode's count | 0 | `HANDSHAKE_MISMATCH`, `HANDSHAKE_MISSING`, `HANDSHAKE_KEY_GENERATOR`, `HANDSHAKE_COUNT_MISMATCH` |
| Requests and replies | C2S 0x3D's length per start both runs answered; RTT pongs echoing no earlier S2C 0x57 ping, pings running backward | 0 | `REPLY_LENGTH_MISMATCH`, `RTT_ECHO_MISMATCH` |
| Integrity stream | invalid pairs, CRCs off the retail oracle (RR's rows plus RR-aa's of the same `integrity_profile`), unwitnessed rows (none with a retail host that punted nothing), OR/OO host rows (Jaccard), repeats, challenges without a preceding 0x16, PUNTs | 0 | `INTEGRITY_PAIR_INVALID`, `INTEGRITY_CRC_MISMATCH`, `INTEGRITY_ROW_UNWITNESSED`, `HOST_AMMO_ROW_MISMATCH`, `HOST_DYNAMIC_ENTITY_ROW_MISMATCH`, `INTEGRITY_NOT_ALTERNATING`, `INTEGRITY_SCOREBOARD_ORDER`, `INTEGRITY_PUNT` (entity-family variants prefixed `ENTITY_`) |
| Milestones (handshake, 0x7B, C2S 0x09, S2C 0x2C, 0x08, 0x0B, C2S 0x0B, 0x0F, first 0x0C/0x0A) | one-sided milestones plus order inversions | 0 | `MILESTONE_MISMATCH` |
| Scoreboard cadence (external capture, comparison interval) | S2C 0x16 lists under 4,000 ms after the previous one | 0 | `SCOREBOARD_OFF_CADENCE` |
| Bandwidth and gameplay cadence (steady window) | `\|subject / RR - 1\|` | 0.20 | `BANDWIDTH_MISMATCH`, `GAMEPLAY_CADENCE` |
| C2S 0x0C state (steady window) | value-set Jaccard (shape, ADM, state, move, carrier none/bound, priority occupancy); fill 0/1; dynamic `\|log2(subject / RR)\|` (positions, orientation, analog, each priority slot's median score); initial position delta | 0; 0; 3 (the 1/8x..8x band); 64 world units | `UPLINK_*_MISMATCH`, `UPLINK_ALL_ZERO`, `UPLINK_CONSTANT`, `DYNAMIC_RANGE_MISMATCH`, `DYNAMIC_STEP_MISMATCH`, `DYNAMIC_BASELINE_MISMATCH` |
| S2C 0x0A state and entities (steady window) | value-set Jaccard (sub-block, flags (flags2's route nibble and modal step), record classes, local state, health, state word, carried, weapon/timer (breathtime, fallmps)/env/objective/mount blocks, entity class:type, the recipient's own record against the anchor); fields RR fills that the run never does (the server CPU byte included); dynamic as above, entity positions in world units with carrier-local records left out (p90 nearest rank) | as above | `FRAME_*_MISMATCH`, `FRAME_ANCHOR_ZERO`, `FRAME_ANCHOR_OFFSET_MISMATCH`, `FRAME_ENTITY_TYPES_MISMATCH`, `ENTITY_FIELD_EMPTY`, `FRAME_ENTITY_CONSTANT` |

Every run's evidence must hold too, or the verdict fails: one gameplay session,
a steady window inside the capture, decoded gameplay within 500 ms of each
window edge with no gap over 1 s, every datagram in the comparison interval
decoded with one packet per protocol datagram, and the same mission, game
type and port as RR.

The normalization registry (`apps/parity/normalization.h`) lists every field a
comparison masks, strips or reduces with the nondeterminism source that makes
that correct: the handshake's dynamic fields and reductions, the initial bodies
routed to stateful, set or decoded-record checks, S2C 0x0F's live tick, the
timer-phase identities (0x30/0x31, 0x16, 0x46, 0x79, 0x6E, 0x6F), the grouping
lengths and the client's send-pump boundaries, the session-local handles, the
measured fps/CPU bytes and the send counter. The masks are driven from it, a
mask elsewhere asserts its entry at compile time, and every verdict carries it.
A mask whose finding is real keeps it visible through another row (the
scoreboard cadence, the anchor's own offset, the C2S 0x09 / S2C 0x2C milestones,
the key generator).

Output: `--out` (default `parity-verdict.json`) is an
`opennova.parity-verdict.v1` document. Its `rows` give each topology,
direction, metric and key with the status, distance, floor, A/A distance,
threshold, the subject's and RR's values and the finding code; `aa` holds the
sanity rows, `evidence` each run's findings. Stdout carries `PARITY_FAIL` lines
for the first failures, `PARITY_AA_WIDENED=<n>`, `PARITY_STATUS=PASS|FAIL`,
`PARITY_FAILURES=<n>` and `PARITY_REPORT=<path>`. Exit 0 is PASS, 1 FAIL, 2 a
usage or input error (nothing judged).

### The suite

Before a suite, `parity_preflight.ps1` (read-only; it launches nothing)
checks every input the suite binds and prints one `PARITY_PREFLIGHT <check>
PASS|FAIL <detail>` line each, then `PARITY_PREFLIGHT_STATUS`: both
`Jointops.exe` the pinned jox01 image; both copies' onHook proxy
`binkw32.dll` one build, the one beside `onhook-mcp.exe`, with `binkw32_.dll`
present; the SERVER `game.cfg` against the case table's pin, and its
`game_name` and port range against the table; the corpus against its pins; the Godot 4.6.1 console wrapper and its
runtime; dumpcap and the Npcap loopback adapter; the three judges newer than
their sources; a clean checkout; both template_debug DLLs in `godot\bin` equal
to the `gdext_windows_template_debug` artifact of `-DllRunId`, a `ci.yml` run
whose commit's `engine/` and `godot/` trees equal HEAD's (the artifact is
fetched to `%TEMP%`); the MCP ports and the case ports free; no `Jointops.exe`
running. `-ExpectOnHookMcpSha256`, `-ExpectOnHookProxySha256` and
`-ExpectGodotLauncherSha256` pin those builds too.

```powershell
powershell -File scripts\net\parity_preflight.ps1 -CorpusRoot C:\retail\corpus `
    -RetailServerRoot C:\retail\jox01\SERVER -RetailClientRoot C:\retail\jox01\CLIENT `
    -OnHookMcpPath C:\opennova-int\onhook\onhook-mcp.exe `
    -GodotLauncher C:\Godot\Godot_v4.6.1-stable_win64_console.exe -DllRunId <run id>
```

The OpenNova cells run the CI build: fetch the `ci.yml` run of a commit whose
`engine/`, `godot/src`, `godot/game` and `godot/probes` trees equal the
checkout's (`gh run download <id> -R opennova-net/opennova -n
gdext_windows_template_debug -D godot\bin`, then `scripts/bootstrap_godot.sh`
and one `--headless --path godot --import`), and build the judges at the
same commit.

`run_parity_suite.ps1` runs the tracked case table through
`run_parity_topology.ps1`, five cells per case with identical inputs (RunId
`<prefix>-<case>-rr`, `-rr-aa`, `-ro`, `-or`, `-oo`), then calls
`opennova-parity` per case and prints `PARITY_SUITE_CASE` lines and
`PARITY_SUITE_STATUS=PASS|FAIL`:

```powershell
powershell -File scripts\net\run_parity_suite.ps1 -CorpusRoot C:\retail\corpus `
    -RetailServerRoot C:\retail\jox01\SERVER -RetailClientRoot C:\retail\jox01\CLIENT `
    -OnHookMcpPath C:\opennova-int\onhook\onhook-mcp.exe `
    -GodotLauncher C:\Godot\Godot_v4.6.1-stable_win64_console.exe `
    -Prefix parity-20261011 [-Cases 01tr-coop] [-Topologies RR,RR-aa] [-Plan]
```

`-CorpusRoot` holds the extracted `items.def` and `missions\<MISSION>` (the
layout the wire-readiness gate reads; see `.agents/retail-lan-parity.md`). The
corpus is pinned: the suite refuses an `items.def` off the table's
`items_sha256` and a mission off its case's `mission_sha256`, and hands each
mission's hash to the runner. A cell whose run root already holds `run-summary.json` is
reused, so rerunning with the same `-Prefix` resumes a suite. A run root
without one is a failed attempt, moved aside to `<RunId>.failed-<stamp>` before
the cell runs again. `-Cases` and `-Topologies` choose which cells run; each
case's verdict judges every successful cell it has. PASS needs every selected
cell to succeed and every selected case judged PASS with RO, OR and OO all
present. The verdicts and `suite-summary.json` land in
`.scratch\parity-suites\<prefix>\`. `-Plan` prints the cells and their runner
arguments and runs nothing. The suite prints `PARITY_SUITE_PREFIX` and
`PARITY_SUITE_DLL` (both GDExtension variants' hashes and the commit), which
`suite-summary.json` keeps with the joiner dial and the reference.

`-ReferencePrefix <prefix>` judges the subject cells against an earlier suite's
RR and RR-aa cells instead of running new ones (`-ReferenceRoot`: where those
run roots live, `.scratch\runs` by default, or the evidence copy
`retail-parity\runs-<prefix>` once a suite's runs moved there). A reference is
taken only while its summaries record the current `Jointops.exe`,
`onhook-mcp.exe` and onHook proxy hashes, the case's mission hash and inputs,
and StockObserver, and its RR and RR-aa rendered the same hook configs; a
subject whose retail role's config differs from the reference's (RO's host,
OR's joiner) leaves its case unjudged (`ERROR`). Every cell refuses a dirty
checkout unless `-AllowDirtyRepo`, and the OpenNova joiner browses unless
`-JoinerDial direct` (both diagnostics, passed through to the runner).

A case (`parity-cases.json`, `opennova.parity-cases.v1`) gives `id`,
`mission`, `mission_sha256`, `game_type`, `port` (inside the SERVER copy's `mplanserverportmin`..`max`, stock 32768-32787), `readiness` (`in_match` or
`deploy_hold`), `steady_seconds`, `exercise_input` and `auto_deploy`, and
optionally `scenario`: a `scenarios\` file its actor plays at the steady
window's start in place of the input exercise (a reference is reused only
when it played the same scenario). The table
fixes `expansion` (`jox01`), `items_sha256` and `host_session_name`, which must equal the
SERVER copy's `game.cfg` `game_name` byte for byte (`"Untitled "`, with the
trailing space), and `server_game_cfg_sha256`, the SERVER `game.cfg` pin.

No game window holds real focus in a cell. A focused retail window centres the
cursor every frame and reads the offset as mouse look (the focus gate
`Game_ProcessMainFrame` @0x526401..0x526458, `Input_PumpAndCenterCursor`
@0x7616D0). Every retail window also clips the cursor to its own rect when it
leaves a menu (`Game_ClipCursorToGameWindow` @0x4983C0). So with two windows on
one desktop, the clip one leaves can clamp the other's centring into one
constant delta every frame: the body's yaw steps every frame and its pitch pins
at the clamp (i1's and c1's R1 rows). The runner therefore:

- runs a desktop guard job for the whole cell, which hands the foreground to the
  shell's desktop window and, when Windows keeps a game window in front, posts
  `WM_KILLFOCUS` to it (`desktop-guard.log`);
- frees any cursor clip as the steady window opens and posts `WM_KILLFOCUS` to
  every retail window;
- records the desktop at both ends of the steady window (`desktop_witness`: the
  foreground, the clip, each window's rect and centring point, and `clean`, which
  is false when a focused retail window could not centre).

Scripted input keeps its own virtual focus.

Both hosts run in the SERVER copy: retail's `Jointops.exe` in its install, the
OpenNova host with `--working-dir` on it (`host_opennova.ps1 -WorkingDir`).
Each reads the copy's `game.cfg` and `player.sav` at its start and writes them
(and `weapon.sav`, the root's and the expansion's) at its save points; retail
saves at its exit. Every cell refuses a copy whose `game.cfg` is not the pin
(`-ServerGameCfgSha256`; the suite passes the table's) and, once every process
has stopped, writes each of those files' start bytes back (deleting one the cell
created). The summary's `server_copy_files` records each file's start hash,
whether the cell's processes wrote it, and the restore; a reference is reused
only when it started from the pinned `game.cfg`.

The coverage extensions keep their cases in `parity-extension-cases.json`
(same schema; run with `-CasesPath scripts\net\parity-extension-cases.json`),
so the default suite is unchanged. An extension case may add:

- `rotation`: E2's missions after `mission` in the host's rotation, in play
  order. The runner passes them as `-Rotation` and the retail host takes them
  through `onhook_host_lan`'s `rotation` (an onHook pair with host rotation).
  The OpenNova listen host has no rotation launch flag yet, so a rotation case
  runs RR, RR-aa and RO only.
- `time_limit_minutes`: E2's round limit (`-TimeLimitMinutes`). Both hosts read
  it from the SERVER copy's `game.cfg` `time_limit` (retail starts a round
  that is not co-op with `time_limit` x 3720 ticks on its round timer;
  `koth_limit` is the king-of-the-hill hold), so the runner writes it there
  for the cell; the cell's restore (above) takes it back. The summary
  records each `game.cfg` write and the restore in
  `server_game_cfg_overrides`.
- `permanent_death`: E5's permanent death (`-PermanentDeath`): the SERVER
  copy's `game.cfg` `mp_permanent_death` is 1 for the cell (a second entry in
  `server_game_cfg_overrides`), restored like `time_limit`. Retail's session
  takes it on the authority (`g_SessionPermanentDeath @0x5522AA`) and the
  OpenNova host reads the same word (`host_config.cpp`), so every topology
  runs it.
- `host_player_class`: E5's medic (`-HostPlayerClass`, 5..9): the host
  player's class for the cell. The class is the current `weapon.sav` record's
  side byte (+0 blue, +0x8006 red, each record 0x1080C bytes after a 16-byte
  header), which the session start clamps into `g_CharClassTeam1/2`
  (`Game_ApplySessionSettingsToGlobals @0x5516BB..0x551720`) and
  `Game_StartMission` gives the local player (`PlayerSession_InitFromProfile`,
  the slot's class @0x50CB7C); `player.sav` holds no class. The runner writes
  both side bytes of record `player_index` in the SERVER copy's active
  `weapon.sav` (the expansion's), as the PLAYER_INFO dialog does, and the
  cell's restore writes the file back (`server_player_class_override`
  records it). A retail host only: whether the OpenNova listen host takes its
  class from that record is unchecked, so OR and OO refuse it.
- `late_joiner`: E3's second joiner (`-LateJoiner`). It is the first joiner's
  kind and joins during the steady window, after the first joiner's scripted
  input. In RR and OR it is retail (`onhook_join_lan`, callsign `ParityLate`,
  from the CLIENT copy or `-RetailLateClientRoot`, its own capture under the
  run's `late\`). An OpenNova late joiner is not wired yet, so a late-joiner
  case runs RR, RR-aa and OR. The summary's `late_joiner` records its run,
  capture and `client_port`, the session the late join's verdict selects: the
  suite judges such a case twice, the second time with `opennova-parity
  --evidence late` (each run's late-joiner capture bound to that port, its steady
  window from its readiness to the cell's steady end) into
  `<case>.late.verdict.json`.
- `topologies`: the cells the case has (default all five; RR and RR-aa always).
  The other cells report `not_applicable` and are not counted as missing.

`01tr-coop-selfnade` is the dying joiner's case: 01TR co-op playing
`self_nade` (the joiner frags its own feet), with an 80 s steady window over
the death screen. It is what judges a dead joiner's send pump (D-NET-529),
which the main table's 03TR walk no longer reaches since its joiner stands.

E5's gameplay cases each play one scenario (the table below) where the
standard cases play the input exercise: `tdh-i3a-tdm-hostkill` (the host
shoots the joiner dead), `tdh-i3a-tdm-fragrespawn` (the joiner's frag kills
the staged host and itself, then the joiner respawns), `01tr-coop-aikill` (the
staged joiner shoots a co-op AI), `01tr-coop-drive` (`drive`) and
`ash-i5a-aas-capture` (`capture_base`). A case whose scenario stages players
runs RR, RR-aa and RO only, and the suite refuses one that lists OR or OO.
E5's widening adds three: `tdh-i3a-tdm-swap` (`team_switch`: the joiner
dies and clicks the death screen's SWAP TEAMS; RR, RR-aa and OR, the click
being onHook's), `tdh-i3a-tdm-permdeath` (`permanent_death`, with
`permanent_death`; all five) and `01tr-coop-revive` (`medic_revive`, with
`host_player_class` 5; RR, RR-aa and RO). A case whose scenario types or
clicks (`char`, `click`) keeps that actor retail, and the suite refuses a
cell where it would be OpenNova's: the `scenario_play` driver plays neither
yet.

The spectator convert itself has no case. A dead player's SPACE under
permanent death sends C2S 0x51 only while the session allows spectators
(S2C 0x08 bit 13, `CNapiServerConfig_BuildFlags @0x4C4E82`, from the host's
`mpnumspectatorsmax`), and the convert also needs that quota
(`Server_ProcessClientRequestSpectatorRespawn @0x51C840`). `onhook_host_lan`
sets the retail host's quota to 0 for the whole session (a nonzero quota
sends the host into the menu's player or spectator choice, which a direct
launch cannot answer), and the pinned `game.cfg` holds 0 for the OpenNova
host, so `permanent_death` judges the other leg: the dead joiner's SPACE
sends nothing and the joiner stays dead (`Input_HandleActionBinding` case 12,
@0x49B024..0x49B03C), which `count_kinds` pins.

The summary records `rotation`, `time_limit_minutes`, `late_joiner`,
`permanent_death` and `host_player_class`, and a reference is reused only for
a case with the same values.

### Comparing two suites

`diff_verdicts.py` (standard library only) compares two suites' verdict
directories (`.scratch\parity-suites\<prefix>\` or its evidence copy) row by
row, each row keyed by case, topology, direction, metric and key, within the
cases both suites judged:

```powershell
python scripts\net\diff_verdicts.py --old <old suite dir> --new <new suite dir> `
    --out <triage dir> --attribution scripts\net\parity-attribution.json `
    [--fixed D-NET-505,D-NET-520] [--lanes <lane table>]
```

A row is FIXED (failed, now passes), REGRESSED (passed, now fails), PERSISTS
(fails in both; the distance delta is reported), NEW (fails, absent before),
GONE (failed, now absent) or SKIP-CHANGE (skip or short on one side). The
widening report lists every A/A row widened in either suite with its floor,
both A/A distances and each new subject row's distance, flagging a subject
that passes only on the widened threshold, and counts the keys widened per
case. `--attribution` labels each failing or fixed row with the D-NET rows
whose entries match it (`parity-attribution.json`: topologies, direction,
metric and key patterns, cases; each entry's `basis` says whether it is a
Findings row, a consequence the record lists, the record's 03TR / CP04
attribution by metric, or an inferred mechanical consequence); a failing row
nothing matches joins the minting queue. `--fixed` checks rows claimed fixed:
the earlier failing rows attributed to each, and which still fail. Every
failing row is written as a triage line (case, topology, direction, metric,
key, code, distance / floor / A/A / threshold, subject, RR, class,
attribution, lane, capture references, note) into `verdict-diff.md`, with
`verdict-diff.json` beside it; stdout carries `VERDICT_DIFF` lines and
`VERDICT_DIFF_REPORT`.

## Scenario parity (ADR 0050 R5)

A scenario (`scenarios/*.json`, `opennova.scenario.v1`) is one scripted play in
retail's action vocabulary, by its `actor` (the joiner or the host): an
optional `prelude` stage, a setup pose, then steps on logic ticks relative to
the stage's start (`down`/`up`/`press` an action code, `look_px` raw mouse
pixels, `char` one typed character, `click` a named menu widget, `end`), then
a settle. `run_parity_topology.ps1 -Scenario <file>`
plays it right after the steady window starts, on `in_match` and on
`deploy_hold` runs (a prelude can press the deploy key). A retail actor plays
through onHook's virtual keyboard (`onhook_play_input`, bridge protocol 1.10,
on both of retail's input-frame paths), an OpenNova actor through the
`scenario_play` probe, whose scripted keys also arrive as the bound key's own
events (the deploy keys read those). The summary's `scenario_witness` records
each step's applied logic tick, and the run root keeps the exact script it
played.

A setup pose sticks only on the host's own player. A client snaps its players,
its own included, to the host's replicated pose once they are more than two
units apart (`Entity_UpdateInfantryPlayerBody @0x4B42C1..0x4B434A`, skipped on
the authority), so a joiner pose sets only the heading. A scenario that needs
a place uses the host as its actor.

A scenario may also list `staging` (E5): `[{"slot": n, "position_bms": [x, y,
z]}]`, positions the host gives players' slots after the prelude and before
the actor's setup, so the snap above carries them to the joiner. A retail host
applies them through `onhook_load_debug_snapshot`'s `target_slot` (an onHook
pair with `apply_pose_slot`, opennova-int #74); the OpenNova host has no slot
pose yet, so a staged scenario runs RR and RO. The scenario witness records
each staging result, and `compare_scenario.py` requires every run to have
played the same staging. The E5 probe (two TDH RR cells) showed what sticks:

- a position more than two units from where that player stands sticks (its
  client snaps to it); a nearer one is pulled back by the player's own next
  uplink. A staging position is at least 3 units from the slot's position
  when it is staged.
- z settles onto the ground, so a staging z is on or near the ground.
- the heading never sticks: the host's next frame carries the player's own
  heading again. A staging entry has no heading (the runner writes yaw 0 and
  pitch 0, which the snapshot format needs); a joiner actor's heading comes
  from its own setup pose.

A scenario whose play ends the joiner's session (E4's `kick`) says
`"ends_joiner_session": true`. The runner then reads the joiner's closing
state instead of waiting for it in match (RR), or skips the OpenNova joiner's
closing readiness (RO). It takes a host actor on an in-match RR or RO run
without a late joiner: the OpenNova host's `scenario_play` driver types no
`char` steps yet, and OR and OO keep their steady-end checks.

A `click` step (`{"tick": n, "click": {"screen": "DEATH", "control":
"SWAP_TEAMS"}}`) left-clicks a menu widget by retail's own names
(`UI_FindScreenControl @0x63AE80`). onHook presses the left button at the
widget's centre on its tick and lets it go there on the next, and the menu's
own per-frame pump clicks it (`CWnd_ProcessMouseEvent @0x647A00`), so the
control's callbacks run (SWAP TEAMS sends C2S 0x4D,
`DeathScreen_OnSwapTeams @0x5535B0`). Nothing on that path reads focus, so it
plays under the desktop guard; it does need the system cursor to stay off the
retail windows for those two ticks (an unattended run). A click whose widget
is not on screen on its tick presses nothing and fails the cell. It needs an
onHook pair with `scenario_click` (opennova-int #75, bridge protocol 1.10; a
hook and companion from different builds report the skew).

```powershell
pwsh -File scripts\net\run_parity_topology.ps1 -Topology RR ... -Scenario scripts\net\scenarios\self_nade.json
python scripts/net/compare_scenario.py .scratch/runs/<rr>/run-summary.json .scratch/runs/<ro>/run-summary.json ...
```

`compare_scenario.py` decodes each evidence capture with
`opennova-wire --scenario-events`, keeps the scenario's window, and names the actor: a
joiner by the capture's first C2S 0x0C uplink, the host by roster slot 0
(S2C 0x46). Each run must match the scenario's
`expect.sequence` in order. Each run's matched fields and `expect.count_kinds`
counts must also equal the reference (RR when given). Capture-local fields
(frame, time, session, positions) and the scenario's `expect.mask` fields are
excluded.

A session-level disconnect prints as `kind=disconnect` (full tag 0x103, the
connection-description record), with `class`, `reason` (DPC, the code the
client's exit-reason switch reads) and the two strings in hex. A host sends it to
the player it kicks, reason 33 (`Server_NotifyPlayerKicked` ->
`CNapiNPConnection_QueueDisconnectEvent @0x4C7EF0`). That is what a kick scenario
expects.

| Scenario | Mission / mode | Play | Notes |
| --- | --- | --- | --- |
| `self_nade` | 01TR co-op | select the frag, look down, 40-tick throw at the feet | `game_event.type` is masked: retail draws the suicide message type 1..3 from its PRNG (`GameEvent_PlayerDeath @0x5170ed`). Reload requests are sequence-only: a retail client repeats C2S 0x25 until the 0x49 echo refills the slot, so the count follows the round trip. |
| `drive` | 01TR co-op | facing the spawn buggy: USE takes its 50cal, USE drops onto the deck, USE takes the driver seat, drive, USE | The joiner's setup pose sets only its heading (above). Seat swaps are held-USE digit keys, not action codes, so the seats come from retail's nearest-seat scan. |
| `frag_kill` | 01TR deathmatch | the `self_nade` play with the host on the shared spawn | The frag kills the host (a kill, 0x1E type 4..6 from the PRNG, `GameEvent_PlayerDeath @0x517237`), then the thrower (a suicide). `game_event.type` is masked. |
| `chat` | TDH_I3A team deathmatch | the joiner opens the command prompt (action 1), types `hello` one `char` a step, return | E4. The line goes out as C2S 0x04 and the host echoes it to its sender alone as S2C 0x06, the line a NETLOG handler did not take (`NapiNPServerMsg_HandleConsoleLine @0x5199D0`; `opennova-wire --scenario-events` prints both as `console_line` / `console_echo` with the text in hex). A retail actor needs an onHook pair with `char` steps (opennova-int #73); an OpenNova actor needs the `scenario_play` driver to type them. |
| `kick` | TDH_I3A team deathmatch | the host opens the command prompt (action 1), types `PUNT ALL`, return | E4. A host's typed line goes to the admin commands, not the binding names, in session (`Client_ProcessConsoleCommand @0x520E80`; the binding-name match in `Client_ProcessLocalCommand @0x520BF0` is offline only). `PUNT ALL` sends every connected non-host player the session disconnect with an empty reason, code 33 and the tag `S.C:SPA` (`Server_HandleBanPuntCommand @0x50B380`, the send @0x50B951; `PUNT <slot>` tags `S.C:SP#`, @0x50B9C6). `BAN <slot>` only adds the player's PCID to `banlist.txt` and disconnects no one. The joiner's capture (RR) or the host's (RO) carries it as `kind=disconnect`. A retail host needs an onHook pair with `char` steps (opennova-int #73). |
| `capture_base` | ASH_I5A Advance & Secure, `deploy_hold` | the host deploys with SPACE (auto team spawn), is posed inside zone 2's bunker (`0x1032`) and holds it | The zone drains (mode 0, -2730/s), turns (0x1E 60, 0x50 to team 1, 0x1E 52 and 56) and secures (mode 1, +1820/s, 0x1E 59) in about a minute. The first timer sample's `value` is masked: it depends on where the 1 Hz block falls after the pose. |
| `tdm_host_kill` | TDH_I3A team deathmatch (`tdh-i3a-tdm-hostkill`) | the host, posed 4 units east of the joiner's spawn (33.4, -16.0, 16.8) facing it and aiming 10 degrees down, taps fire eight times | An enemy kill of a remote client: S2C 0x13, the victim's zero 0x61 and its 0x52, then 0x1E type 4..6 (10..12 on a head hit) (`GameEvent_PlayerDeath @0x516DD0`). `game_event.type` is masked (the PRNG draw). The joiner stays dead (no client respawns by itself). No staging, so it runs on every topology. |
| `tdm_frag_respawn` | TDH_I3A team deathmatch (`tdh-i3a-tdm-fragrespawn`) | the host is staged 1.6 units east of the joiner's spawn; the joiner throws a frag at its feet (the `self_nade` play), then presses SPACE twice once its respawn delay has run | The frag kills the host (0x1E 4..6) and the joiner (1..3), as `frag_kill` does in deathmatch. A dead player's SPACE is the death screen's auto team pick, C2S 0x0E 0xFFFE (`Input_HandleSpecialKeys @0x49CA06..0x49CA0D`); the host drops picks while the `timeout` delay (5 s in the pinned `game.cfg`) runs, and a client never retries, hence the second press. The deploy's S2C 0x61 carries a nonzero seed (`Server_DeployPlayer @0x517740`), masked; TDH has no frontier zone, so no 0x1E 58 follows (@0x5179D9..0x5179E0). |
| `coop_ai_kill` | 01TR co-op (`01tr-coop-aikill`) | the joiner is staged 4 units behind the standing AI 0x0000 at (241.9, 840.8, 43.0) and faces its back, aiming 6 degrees down, and taps fire eight times | The client's fire (C2S 0x06) and the host's AI death (S2C 0x13 for 0x0000). Nearby AI may answer the fire, so nothing after the death is counted. |
| `team_switch` | TDH_I3A team deathmatch (`tdh-i3a-tdm-swap`) | the joiner frags its own feet (the `self_nade` play) and, on the death screen, clicks SWAP TEAMS | The host's answer (`NapiNPServerMsg_0x04D_ChangeTeam @0x518F70`): S2C 0x50 for the joiner (`Server_ChangeEntityTeam`), the S2C 0x14 system line (C2Blue / C2Red), then the killer-less death inline (S2C 0x13 again and its 0x1E), its hold the 60 s `mpchangeteam_penalty`. C2S 0x4D and S2C 0x14 have no scenario kind yet. The pinned `game.cfg` passes every gate: TeamChoose (`mpattrib` bit 4, which also shows the button, `DeathScreen_UpdateUI @0x553443..0x5534AA`), no autobalance, no earlier switch inside `mpchangeteam_interval`. `game_event.type` is masked (the PRNG draws). |
| `permanent_death` | TDH_I3A team deathmatch (`tdh-i3a-tdm-permdeath`, `permanent_death`) | the joiner frags its own feet and presses SPACE twice | No spectator quota (above), so SPACE sends nothing and the joiner stays dead; `count_kinds` pins one death and one 0x61. |
| `medic_revive` | 01TR co-op (`01tr-coop-revive`, `host_player_class` 5) | the joiner is staged 4 units in front of the standing AI 0x0000, which kills it; the host, a medic posed 4 units behind the AI and facing it, shoots the AI, selects the medpack (209), walks to the body, looks down and fires the medpack | The revive (`GameEvent_RevivePlayer @0x517CD0`) needs a class-5 healer, the victim's revive window open (a death by another player or AI opens it), the victim's auto-medic on (both copies' profiles have it) and a last attacker that is neither the healer nor the victim. Its wire to the joiner: the reroll S2C 0x61 (masked) and S2C 0x1E event 38 [victim, healer, 0xFF]; its S2C 0x54 goes to same-team medics only and its S2C 0x3A has no kind. Co-op 01TR with the AI as the killer: the victim must be on the medic's team and killed by neither, which two players in co-op with an AI give, where TDM would need a third player the runner cannot script. Nothing is counted (nearby AI may join in). |
| `dm_host_walk` | 03TR deathmatch (the `03tr-dm` suite case) | the host, posed on the shared spawn (0, 0, 62.4) facing +y, walks the runner's input exercise legs (forward, strafe, back, each with its mouse turn) | The case's play. The joiner exercise walked a retail host's joiner (spawned facing yaw 0x00) off the spawn ledge to its death on some runs and not others, so the 03TR A/A floor was a coin flip; the posed host stays on the raised side. `expect` is empty: the suite judges the run by its parity metrics. |
