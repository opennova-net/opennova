# Retail validation hook

This is a research-only, 32-bit Windows tool for checking OpenNova's reverse-
engineered Joint Operations layouts against a running retail process. The hook
reads process memory through the declarations in `libs/retail_abi`; it does not
carry a second set of private struct definitions.

## Supported executable

The only accepted build is the patched Joint Operations 1.7.5.7 executable
distributed by the original onHook tooling, with this SHA-256:

```text
9a1035440a53af2057ce0995ac42dced840d3b9fd53c04dc86041a962b84fe57
```

The injector checks the file before launch, and the injected DLL checks the
running executable again before opening a validation session. Any mismatch is
rejected; adding another retail build requires a separately witnessed profile.

## One-command parity workflow

Use Git Bash (or another Bash running on Windows) from the repository root:

```bash
bash scripts/retail_parity.sh setup \
  --game-dir '<folder containing Jointops.exe>' \
  --godot-bin '<Godot 4.6.1 executable>'
bash scripts/retail_parity.sh --dry-run validate
bash scripts/retail_parity.sh validate
```

`setup` creates `.scratch/retail-parity.env` without overwriting an existing
file. The entire `.scratch/` tree is gitignored. `JO_GAME_DIR` is the folder
containing the user-owned `Jointops.exe`; either `C:\...` or Git Bash
`/c/...` syntax is accepted. `GODOT_BIN` follows the same convention as the
other Godot scripts. `OPENNOVA_RESOURCE_DIR` is optional and defaults to
`JO_GAME_DIR`. Re-running setup preserves the operator-owned file; use
`setup --force` to replace it deliberately. With no subcommand, the wrapper
defaults to `validate`.

The default `validate` run is the `player-combat-loop` scenario on
`ASH_G3D.bms`. It guides two comparable legs:

1. Baseline: retail host plus hooked retail client.
2. Candidate: OpenNova host plus the same hooked retail client.

Both retail roles are launched with exactly `/w /exp jox01 /MANY`.
`/MANY` is the pinned executable's built-in bypass for its `FindWindow`
singleton check, which is required to run the retail host and client together.
It is not process patching: the wrapper and injector pass the witnessed switch
through unchanged. For the candidate leg the OpenNova process deliberately
remains `/w /exp jox01`, without the retail-only switch. The wrapper sets
`OPENNOVA_RESOURCE_DIR`, `OPENNOVA_PARITY_PIPE`,
`OPENNOVA_PARITY_ROLE`, `OPENNOVA_PARITY_STREAM_ID`,
`OPENNOVA_PARITY_RUN_ID`, `OPENNOVA_PARITY_BUILD_ID`,
`OPENNOVA_PARITY_SCENARIO`, `OPENNOVA_PARITY_TITLE`,
`OPENNOVA_PARITY_EXPANSION`, `OPENNOVA_PARITY_MISSION`,
`OPENNOVA_PARITY_TRACE`, and `NW_LAN_HOST`, then starts
`$GODOT_BIN --path godot -- /w /exp jox01`. An empty build-id override uses
the wrapper's commit-derived `opennova-<short-sha>` value (or
`opennova-worktree` when Git provenance is unavailable); the runtime's
`opennova-godot-worktree` fallback is only for direct invocation outside the
wrapper. The Git Bash boundary disables MSYS argument rewriting only for the
native injector and Godot launches, preserving those slash-prefixed arguments
verbatim. The wrapper pauses after each leg so
the operator can perform the scenario and close both game windows before the
trace is finalized. Override the defaults with `--scenario` and `--mission`.

`OPENNOVA_PARITY_TRACE` enables OpenNova capture. When
`OPENNOVA_PARITY_PIPE` is nonempty, OpenNova writes only through the queued
named-pipe producer owned by the inspector; it does not also open the trace
path. When the pipe variable is empty, the same recorder falls back to the
bounded append-only `FileEventSink` at `OPENNOVA_PARITY_TRACE`. This pipe-first
rule prevents two writers from corrupting one bundle while keeping direct-file
capture useful for isolated debugging.

Dry-run is safe before setup: unresolved paths appear as placeholders and no
games, tools, directories, or config files are created. It is also the easiest
way to audit the exact commands before a live run.

`doctor` fails closed before a live launch. It hashes the configured
`Jointops.exe` and requires the supported SHA-256 above, verifies
`expansion/jox01/jox01.pff`, and resolves the selected mission through the
packed retail mount order: `jox01L.pff`, `jox01.pff`, then the applicable base
`language.pff`, `localres.pff`, and `resource.pff`. Successful mission output
names the source archive and path. Because the exact launch omits `/d`, a loose
BMS that is absent from those archives is reported missing with
`DETAIL=loose-only-requires-/d`; doctor never demands a loose copy when the
mission is packed.

### Commands

| Command | Behavior |
| --- | --- |
| `doctor` | Check tools, configured paths, retail SHA-256, jox01, and packed mission reachability. |
| `setup` | Create the local environment template, once. |
| `build` | Build the Win32 hook/injector/inspector, parity tool, and Godot extension. |
| `test` | Run Bash syntax/CLI tests plus focused native parity tests. |
| `retail` | Launch only the baseline leg. |
| `opennova` | Launch only the candidate leg. |
| `compare [reference] [candidate]` | Compare two `.ontrace` bundles. |
| `dump [trace] [output.txt]` | Render a readable event dump. |
| `export-pcapng [trace] [output.pcapng]` | Export captured packet events for Wireshark. |
| `validate` | Prepare, collect both guided legs, validate both traces, and compare them. |

The last baseline and candidate bundle paths are remembered under
`.scratch/retail-parity/`, so `compare`, `dump`, and `export-pcapng` can omit
their paths. `opennova_parity_tool` returns 0 for clean evidence, 2 for a parity
mismatch, 3 for invalid or incomplete evidence, and 64 for usage or I/O errors.
Set `RETAIL_PARITY_SKIP_PREP=1` to skip the build/test preflight on a subsequent
live iteration.

## Guided evidence and verdict policy

The wrapper prints the same four manual phases for each leg: settle at spawn
with a ready weapon; walk/run/turn/crouch/prone/jump; aim or scope, fire, and
reload; then take damage if practical and settle again. Capture producers emit
`capture-start` and `capture-end` automatically and record typed snapshots at a
nominal 60 Hz. The operator does not need to stamp checkpoints by hand.

`validate` first checks the ONPT structure, then exercises the core parity
validator. Metadata must include run, producer, and build provenance; all
producers in one bundle must share run ID, title, expansion, mission, and
scenario. A guided two-producer bundle is exactly one host plus one client, and
`player-combat-loop` requires raw network evidence from both. Every capture
checkpoint must select typed player and weapon state. Continuous presented-lane
frames for the guided retail-client actor must prove a real local-player
excursion of at least 0.25 world units; host and authoritative producers remain
valid while their own local player is idle. This prevents two idle single-player
recordings from masquerading as a guided network run.

State comparison keeps the `capture-start` local-player transform as the strict
spawn anchor, using only named fixed-point/BAM quantization allowances. Manual
path length and final position are operator-controlled: each leg's guided client
must prove its own excursion, after which final player/camera/entity transforms and dynamic
entity transforms are excluded from absolute endpoint equality. The remaining
typed state is still compared, including pool coverage, player health/team/class
and equipped ADM, weapon identity/poses/FOV/action/ammo, camera, and input.

Raw datagrams remain evidence and export input, never equality keys. The CLI
decodes each stream with `npwire` keyed by producer identity plus its port pair,
then compares direction, semantic tag/name vocabulary, and phase ordering inside
matching capture intervals. Consecutive repetitions are collapsed and a shorter
equivalent sequence may be a subsequence of a longer run, so capture timing,
packet counts, repeated per-frame traffic, endpoint addresses/ports, timestamps,
capture-frame fields, payload sizes, and payload bytes cannot create a mismatch.
Raw-but-undecodable streams are reported as coverage warnings. Producer warning
diagnostics (for example queue drops or incomplete hooks) also make the status
`warnings` with exit 0; error or fatal diagnostics make the evidence invalid
with exit 3.

When client semantic decode is available, `validate` also prints whether the
guided interval contains C2S stance-change (`0x1D`), fired-round (`0x06`), and
weapon-reload-request (`0x25`). Missing actions are warnings so an imperfect
manual pass is visible and repeatable; when semantic decode itself is
unavailable the action summary says `unavailable` instead of inventing a
mismatch.

`dump` is payload-safe but state-rich: it prints pool topology and completeness,
entity counts, local-player identities and transforms, weapon/camera/input state,
decoded message metadata, diagnostic context, and mutation before/after audits.
Byte strings are rendered only as `<bytes:N>`. `export-pcapng` writes the raw
captured datagrams as IPv4/UDP Enhanced Packet Blocks with nanosecond timestamps,
real endpoints and directions, and correct captured-versus-original lengths for
truncated packets, ready for Wireshark.

## Native contracts

The Bash entry point keeps binary discovery isolated and delegates these stable
interfaces verbatim:

```text
retail_parity_inspector.exe --pipe <name> --trace <bundle.ontrace> --scenario <name> --role-set <baseline|candidate>
retail_hook_injector.exe --game-dir <dir> --role <retail-host|retail-client> --pipe <name> --run-id <id> --stream-id <id> --scenario <name> --mission <name> [--allow-writes] -- /w /exp jox01 /MANY
opennova_parity_tool validate <bundle.ontrace>
opennova_parity_tool dump <bundle.ontrace> [output.txt]
opennova_parity_tool export-pcapng <bundle.ontrace> <output.pcapng>
opennova_parity_tool compare <reference.ontrace> <candidate.ontrace>
```

The injector starts each retail role under a controlled launch, waits for the
Windows loader to reach input-idle, then loads the DLL and starts its worker.
On success it emits exactly one machine-readable `JO_PROCESS_ID=<pid>` record;
the Bash wrapper uses only that record to track processes it owns. Standalone
`retail` and `opennova` commands wait for the operator and inspector, and an
interrupt or later launch failure terminates only the recorded game processes
and wrapper-owned inspector/OpenNova children.
The x86 hook producer captures parity state at a nominal 60 Hz; the validation
window only refreshes its human-readable view every 750 ms. Press F5 to refresh
immediately, use the mouse wheel or vertical scrollbar to scroll, and press Esc
to close it. The inspector waits up to 180 seconds for both expected producers,
enforces the baseline/candidate topology selected by `--role-set`, validates and
serializes only complete chunks, and caps a merged trace at 256 MiB. Its final
status includes connected/completed producer, invalid/dropped-event, checkpoint,
event, and byte counts. Direct-file capture uses the same 256 MiB default bound.

The DLL and injector are necessarily x86. The reusable
`opennova_parity_windows` named-pipe producer library builds for both x86 and x64;
the DLL exports only undecorated `OpenNovaRetailHook_Start`.

## Safety and evidence policy

The Windows hook is read-only by default: mutation access is disabled and the
process-memory adapter rejects writes. `--allow-writes` is an explicit research
opt-in and is forwarded to both retail launches; it is never implied by another
command. In that opt-in mode only, F6 decrements health, F7 toggles team, F8
increments the equipped ADM, F9 decrements render FOV, F10 offsets the hip-pose
X value, and Shift+F10 offsets the aimed-pose X value. Every attempt passes
identity/address preconditions and emits a typed mutation audit containing its
before value, after value, result, and detail; rejected attempts are audited too.
If the write call succeeds but read-back verification fails, the result is
`unverified`: the window warns `WRITE MAY HAVE OCCURRED` and explicitly says not
to blindly retry, because replaying the mutation could apply it twice.

Keep retail executables, DLLs, memory dumps, captures, `.ontrace` bundles, and
other proprietary retail bytes out of this repository. All live output belongs
under `.scratch/`; tracked tests use synthetic evidence only.
