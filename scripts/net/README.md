# scripts/net — live net capture + iteration harness

Tooling so an agent can capture and decode real wire traffic for the four
retail-interop scenarios (`.agents/interop.md` packet-diff matrix), then diff
against a golden. The repo only *reads* pcaps; live capture here uses an external
Npcap + dumpcap install.

All output goes to `<repo>\.scratch\` (gitignored). Never commit raw captures,
`.sph` logs, account names, local install paths, or machine IPs.

## Scripts

| Script | Purpose |
| --- | --- |
| `lib.ps1` | Shared helpers (repo/.scratch paths, run stamps, Godot + dumpcap discovery). Dot-sourced by the rest. |
| `detect_capture.ps1` | Verify Npcap + dumpcap and a loopback adapter. Run this first. |
| `capture.ps1` | `-Action start\|stop -Tag <t>` — dumpcap to `.scratch\<tag>-<stamp>.pcapng`. Defaults to the loopback adapter; `-Iface` for a real NIC. |
| `decode.ps1` | Decode a `.pcapng`/`.sph` with `nw_pp` (+ optional `nw_replay --print-roles`). Finds the tool in Release or Debug. |
| `launch_retail.ps1` | Phase 0: launch stock `Jointops.exe` with `/connectlog` + `/profile` oracle flags (host/join still manual). Needs `-GameDir` or `$env:JO_GAME_DIR`. |
| `host_opennova.ps1` | Launch a visible OpenNova listen host for a mission over the existing `NW_LAN_*` contract. |
| `join_opennova.ps1` | Launch a visible OpenNova joiner for a host/IP, port, and callsign. |
| `run_lan_pair.ps1` | Launch a host, wait briefly, then launch a joiner; defaults to a two-instance localhost session. |
| `diff_vs_golden.ps1` | `-Ours <our.pcapng> -Golden <golden.pcapng> [-Items <items.def>]` -- decode both with `nw_pp --histogram` and report per-(direction, wire tag) coverage: GAP (retail emits it, we don't), SPURIOUS (we emit an S2C tag retail doesn't), plus any messages in ours that did not decode cleanly. Writes `<our>.vs-golden.txt`; exits non-zero on any gap/spurious/decode-failure. |

(Phase 2 adds `tools/retail_driver` to drive retail host/join unattended.)

## OpenNova LAN host/join (Phase 3)

These helpers launch the Godot game project at `<repo>\godot`. They find Godot
through `$env:GODOT_BIN` or the repo's `.godot-bin` convention used by the other
network scripts.

**First-run prerequisite:** launch OpenNova normally, select the Joint Operations
resource directory, and close it before using these helpers. The selection is
persistent user state; the scripts deliberately do not select or copy game data.
Both instances must be able to load the same mission locally.

Start the two roles separately (host first):

```powershell
pwsh -File scripts\net\host_opennova.ps1 `
    -Mission ASH_I5A.BMS -Name Host -Port 32768

pwsh -File scripts\net\join_opennova.ps1 `
    -Host 127.0.0.1 -Name Joiner -Port 32768
```

Or launch a two-instance localhost pair in one command:

```powershell
pwsh -File scripts\net\run_lan_pair.ps1 -Mission ASH_I5A.BMS
```

`run_lan_pair.ps1` starts the host, waits 1500 ms, then starts the joiner. Use
`-JoinDelayMs` to adjust that bounded 0-10000 ms delay. Pass a machine's LAN IPv4
address or hostname with `-Host` when the joiner is not dialing localhost, and
allow inbound UDP on the selected port (32768 by default).

The normal join launch contract contains only the endpoint and callsign; the LAN
session supplies its own mission metadata. For debugging an older direct-load
runtime that still requires the pre-session environment hook, opt in explicitly
with `join_opennova.ps1 -MissionOverride ASH_I5A.BMS` or
`run_lan_pair.ps1 -JoinMissionOverride ASH_I5A.BMS`. The override sets
`NW_LAN_MISSION` solely for that compatibility path and is not part of normal LAN
discovery.

Each launcher clears inherited `NW_LAN_*` variables before starting its child and
restores the caller's environment immediately afterward, so a stale host variable
cannot turn the joiner into a second host. The scripts print their child PID(s).
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
  `apps/common/pcap_reader`). Build the tools with `scripts/build.sh`; binaries land
  in `build\apps\{nw_pp,nw_replay}\{Release|Debug}\`.
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

Two retail↔retail LAN goldens captured 2026-06-26 (single box, own LAN IP, host
`:32768` <-> joiner `:32769`, captured on the loopback adapter). Decode with
`--items <ITEMS.DEF>` (use `~/Desktop/JOX/ITEMS.DEF`) — without it the `0x0a`
per-frame-update records can't be length-resolved and print `(DECODE INCOMPLETE)`.

| File | Contents |
| --- | --- |
| `retail-lan-host-join.pcapng` | Join reference — 494 dgrams: LAN discovery + handshake (`0x00/0x02/0x61`) + world-load (`0x10/0x0d/0x45/0x20`) + early in-match. Mission TDH_I5A. |
| `retail-lan-host-join-session.pcapng` | Same join, session-only (374, no discovery). |
| `retail-gameplay-session.pcapng` | In-match reference — ~8 min full co-op, mission ASH_G3D ("AS - Palu Cut Rice Paddies"): `0x0a`x2361 / `0x0c`x2351 replication loop, fired-round `0x06`, kills `0x26`, objective `0x1e`, deployed-item `0x59`, capture-zone `0x40`. Zero S2C `0x25` (no pre-deploy GameReset). |

Each has a `.pcapng.txt` decode (regenerate with `decode.ps1 -Items <ITEMS.DEF>`).
These are the oracle to diff OpenNova-host-vs-retail-joiner against when chasing the
join "floating / no map entities" bug.

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
tags retail sends that ours doesn't), **SPURIOUS** rows are wire-illegal regressions,
and **DECODE FAILURES** are our own malformed emissions. Pass `--items` so the per
-frame `0x0A` records length-resolve (without it they print `DECODE INCOMPLETE` and
inflate the failure count).

To pin a result in CI, point the env-gated `nw_golden_diff` ctest at the captured
file (it skips clean when unset, like the other capture-gated tests):

```bash
NW_GOLDEN_OURS=.scratch/ov-host-<stamp>.pcapng \
  ctest --test-dir build -C Release -R nw_golden_diff --output-on-failure
```

It fails the build on any non-deferred GAP or any spurious S2C tag; knowingly
deferred tags live in `kDeferredGaps` (with a `D-NET-*` reference) in
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
