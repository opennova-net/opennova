# Network interoperability helpers

These PowerShell scripts support local LAN, capture, and retail-interoperability
work. They are operator tools, not release inputs or CI gates. Captures and
retail data stay machine-local and gitignored and must never be
committed without the sanitization review described in
[`docs/asset-gated-tests.md`](../../docs/asset-gated-tests.md).

The former generated parity matrix, corpus exporter and manifest verifier have
been removed (they depended on the retired Python FFI); `diff_0a.py`, the S2C
0x0A field-shape diff, remains. Do not treat these remaining helpers as a
hash-bound parity verdict.

## Tools

| Script | Purpose |
| --- | --- |
| `detect_capture.ps1` | Locate `dumpcap` and list usable capture interfaces. |
| `capture.ps1` | Start or stop a tagged local packet capture. |
| `decode.ps1` | Decode a capture with the native `opennova-wire` application. |
| `diff_0a.py` | Per-FIELD shape diff of the S2C 0x0A stream vs a retail-host golden (sub-block cycle, record-class mix, field population); caches `<golden>.0a.json` beside the input. |
| `exercise_retail_input.ps1` | Drive the bounded Windows input trajectory used by live probes. |
| `host_opennova.ps1` | Start an OpenNova LAN host on launch flags, await LAN discovery and its MCP endpoint. |
| `join_opennova.ps1` | Start an OpenNova LAN joiner on launch flags with its MCP endpoint. |
| `run_lan_pair.ps1` | Start a local OpenNova host/joiner pair (MCP ports 8975/8976). |
| `run_parity_topology.ps1` | Run one explicitly configured live topology, optionally playing a scenario (`-Scenario`). |
| `compare_scenario.py` | Check scenario runs against their expectations and against the RR reference. |
| `wait_parity_wire_ready.ps1` | Wait for and validate the topology's live wire-ready state. |

Shared process and path helpers live in `lib.ps1` (which dot-sources the
opennova-game MCP client, `scripts/mcp/game_mcp.ps1`); retail instances are
launched only through onhook-mcp.

## Build native helpers

```powershell
cmake --build build --config Release --target opennova_wire opennova_lan_probe
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
of just the S2C `0x0A` stream — the two captures are different sessions, so it compares what
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
