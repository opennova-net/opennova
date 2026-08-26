# Network interoperability helpers

These PowerShell scripts support local LAN, capture, and retail-interoperability
work. They are operator tools, not release inputs or CI gates. Captures and
retail data stay under the gitignored `.scratch/` tree and must never be
committed without the sanitization review described in
[`docs/asset-gated-tests.md`](../../docs/asset-gated-tests.md).

The former generated parity matrix, corpus exporter, manifest verifier, and
field-shape analysis pipeline have been removed. Do not treat these remaining
helpers as a hash-bound parity verdict.

## Tools

| Script | Purpose |
| --- | --- |
| `detect_capture.ps1` | Locate `dumpcap` and list usable capture interfaces. |
| `capture.ps1` | Start or stop a tagged local packet capture. |
| `decode.ps1` | Decode a capture with the native `nw_pp` application. |
| `diff_vs_golden.ps1` | Compare native decoder tag coverage against a local golden capture. |
| `launch_retail.ps1` | Launch a stock retail client for a manual host or join run. |
| `launch_retail_cfg.ps1` | Launch retail from a role-specific onHook configuration. |
| `exercise_retail_input.ps1` | Drive the bounded Windows input trajectory used by live probes. |
| `host_opennova.ps1` | Start an OpenNova LAN host and wait for protocol readiness. |
| `join_opennova.ps1` | Start an OpenNova LAN joiner. |
| `run_lan_pair.ps1` | Start a local OpenNova host/joiner pair. |
| `run_parity_topology.ps1` | Run one explicitly configured live topology. |
| `wait_parity_wire_ready.ps1` | Wait for and validate the topology's live wire-ready state. |

Shared process and path helpers live in `lib.ps1`; the two tracked cfg examples
live in `configs/`.

## Build native helpers

```powershell
cmake --build build --config Release --target nw_pp opennova_nw_lan_probe
```

The scripts locate binaries in the Release or Debug output directories.

## Capture and decode

```powershell
pwsh -File scripts\net\detect_capture.ps1
pwsh -File scripts\net\capture.ps1 -Action start -Tag retail-ref
# Run the session, then stop using the tag printed by the start command.
pwsh -File scripts\net\capture.ps1 -Action stop -Tag retail-ref
pwsh -File scripts\net\decode.ps1 -Capture <capture.pcapng> -Roles
```

NovaWorld traffic can contain credentials, account identity, and machine paths.
Treat every `.pcap`, `.pcapng`, and `.sph` file as sensitive until reviewed.

## OpenNova LAN host/join

The launchers find Godot through `GODOT_BIN` or the repo's `.godot-bin`
convention. On first use, launch OpenNova normally, choose the retail resource
directory, and close it so that setting exists in persistent user state.

```powershell
pwsh -File scripts\net\host_opennova.ps1 `
    -Mission ASH_I5A.BMS -Name Host -Port 32768 `
    -Windowed -Resolution 1920x1080

pwsh -File scripts\net\join_opennova.ps1 `
    -Host 127.0.0.1 -Name Joiner -Port 32768 `
    -Windowed -Resolution 1920x1080
```

Or start a local pair:

```powershell
pwsh -File scripts\net\run_lan_pair.ps1 -Mission ASH_I5A.BMS `
    -Windowed -Resolution 1920x1080
```

Use a reachable LAN address instead of `127.0.0.1` for a second machine, and
allow inbound UDP on the selected port. The host waits for a valid discovery
reply before reporting ready; process creation or a bound socket alone is not
sufficient.

## Golden coverage check

`diff_vs_golden.ps1` is an exploratory per-tag comparison. It is useful for
finding missing or unexpected traffic, but it does not prove field-level or
session-level parity.

```powershell
pwsh -File scripts\net\diff_vs_golden.ps1 `
    -Ours .scratch\ours.pcapng `
    -Golden .scratch\golden\retail-gameplay-session.pcapng `
    -Items C:\path\to\ITEMS.DEF
```

For automated native coverage, run the relevant `nw_*`, `npruntime_*`, and
`netsim_*` CTests. Asset-gated cases skip successfully when their environment
variables or local captures are absent, so read their output as well as the
exit code.
