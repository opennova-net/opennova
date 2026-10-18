# Network interoperability helpers

These PowerShell scripts support local LAN, capture, and retail-interoperability
work. They are operator tools, not release inputs or CI gates. Captures and
retail data stay machine-local and gitignored and must never be
committed without the sanitization review described in
[`docs/asset-gated-tests.md`](../../docs/asset-gated-tests.md).

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
| `host_opennova.ps1` | Start an OpenNova LAN host on launch flags, await LAN discovery and its MCP endpoint. |
| `join_opennova.ps1` | Start an OpenNova LAN joiner on launch flags with its MCP endpoint. |
| `run_lan_pair.ps1` | Start a local OpenNova host/joiner pair (MCP ports 8975/8976). |
| `run_parity_topology.ps1` | Run one explicitly configured live topology, optionally playing a scenario (`-Scenario`). |
| `run_parity_suite.ps1` | Run every case of `parity-cases.json` as RR, RR-aa, RO, OR and OO cells (resumable), then judge each case with `opennova-parity`. |
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
pwsh -File scripts\net\decode.ps1 -Capture <capture.pcapng> -Stream
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
suite hashes each mission artifact for the runner, and a case may pin it with
`mission_sha256`. A cell whose run root already holds `run-summary.json` is
reused, so rerunning with the same `-Prefix` resumes a suite. A run root
without one is a failed attempt, moved aside to `<RunId>.failed-<stamp>` before
the cell runs again. `-Cases` and `-Topologies` choose which cells run; each
case's verdict judges every successful cell it has. PASS needs every selected
cell to succeed and every selected case judged PASS with RO, OR and OO all
present. The verdicts and `suite-summary.json` land in
`.scratch\parity-suites\<prefix>\`. `-Plan` prints the cells and their runner
arguments and runs nothing.

A case (`parity-cases.json`, `opennova.parity-cases.v1`) gives `id`,
`mission`, `game_type`, `port` (inside the SERVER copy's `mplanserverportmin`..`max`, stock 32768-32787), `readiness` (`in_match` or
`deploy_hold`), `steady_seconds`, `exercise_input` and `auto_deploy`. The table
fixes `expansion` (`jox01`) and `host_session_name`, which must equal the
SERVER copy's `game.cfg` `game_name` byte for byte (`"Untitled "`, with the
trailing space).

## Scenario parity (ADR 0050 R5)

A scenario (`scenarios/*.json`, `opennova.scenario.v1`) is one scripted play in
retail's action vocabulary, by its `actor` (the joiner or the host): an
optional `prelude` stage, a setup pose, then steps on logic ticks relative to
the stage's start (`down`/`up`/`press` an action code, `look_px` raw mouse
pixels, `end`), then a settle. `run_parity_topology.ps1 -Scenario <file>`
plays it right after the steady window starts, on `in_match` and on
`deploy_hold` runs (a prelude can press the deploy key). A retail actor plays
through onHook's virtual keyboard (`onhook_play_input`, bridge protocol 1.9,
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

| Scenario | Mission / mode | Play | Notes |
| --- | --- | --- | --- |
| `self_nade` | 01TR co-op | select the frag, look down, 40-tick throw at the feet | `game_event.type` is masked: retail draws the suicide message type 1..3 from its PRNG (`GameEvent_PlayerDeath @0x5170ed`). Reload requests are sequence-only: a retail client repeats C2S 0x25 until the 0x49 echo refills the slot, so the count follows the round trip. |
| `drive` | 01TR co-op | facing the spawn buggy: USE takes its 50cal, USE drops onto the deck, USE takes the driver seat, drive, USE | The joiner's setup pose sets only its heading (above). Seat swaps are held-USE digit keys, not action codes, so the seats come from retail's nearest-seat scan. |
| `frag_kill` | 01TR deathmatch | the `self_nade` play with the host on the shared spawn | The frag kills the host (a kill, 0x1E type 4..6 from the PRNG, `GameEvent_PlayerDeath @0x517237`), then the thrower (a suicide). `game_event.type` is masked. |
| `capture_base` | ASH_I5A Advance & Secure, `deploy_hold` | the host deploys with SPACE (auto team spawn), is posed inside zone 2's bunker (`0x1032`) and holds it | The zone drains (mode 0, -2730/s), turns (0x1E 60, 0x50 to team 1, 0x1E 52 and 56) and secures (mode 1, +1820/s, 0x1E 59) in about a minute. The first timer sample's `value` is masked: it depends on where the 1 Hz block falls after the pose. |
