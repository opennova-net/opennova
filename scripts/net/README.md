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

(Phase 2 adds `tools/retail_driver` to drive retail host/join unattended; Phase 3
adds `host_opennova.ps1`/`join_opennova.ps1` and the `run_*` orchestrators.)

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
`--items <ITEMS.DEF>` (e.g. `~/Desktop/REVX02/ITEMS.DEF`) — without it the `0x0a`
per-frame-update records can't be length-resolved and print `(DECODE INCOMPLETE)`.

| File | Contents |
| --- | --- |
| `retail-lan-host-join.pcapng` | Join reference — 494 dgrams: LAN discovery + handshake (`0x00/0x02/0x61`) + world-load (`0x10/0x0d/0x45/0x20`) + early in-match. Mission TDH_I5A. |
| `retail-lan-host-join-session.pcapng` | Same join, session-only (374, no discovery). |
| `retail-gameplay-session.pcapng` | In-match reference — ~8 min full co-op, mission ASH_G3D ("AS - Palu Cut Rice Paddies"): `0x0a`x2361 / `0x0c`x2351 replication loop, fired-round `0x06`, kills `0x26`, objective `0x1e`, deployed-item `0x59`, capture-zone `0x40`. Zero S2C `0x25` (no pre-deploy GameReset). |

Each has a `.pcapng.txt` decode (regenerate with `decode.ps1 -Items <ITEMS.DEF>`).
These are the oracle to diff OpenNova-host-vs-retail-joiner against when chasing the
join "floating / no map entities" bug.
