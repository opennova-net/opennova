# OpenNova documentation

Tracked golden docs: architecture maps, decision records (ADRs), and
reverse-engineering records. Scratch RE work lives in the untracked `notes/`
directory and graduates here once it is durable; `old_docs/` is a gitignored
read-only archive of retired material.

Conventions: original-engine functions are cited inline as
`[orig: Name @ 0xADDR]` (addresses are `Jointops.exe` retail unless a doc says
otherwise). RE records carry correspondence tables, divergence lists
(`D-XXX-n`), and per-claim verdicts. No raw decompiled code is ever committed;
behavior is summarized and cited.

## Architecture

| Doc | What it covers |
|---|---|
| [`runtime-architecture.md`](runtime-architecture.md) | How a mission runs: the consolidated logic tick, present pass, and audio pass, mapped onto the original main loop |
| [`correspondence.md`](correspondence.md) | The cross-system parity matrix: which original function each reimplementation corresponds to, with verdicts |

## Decision records

| ADR | Decision |
|---|---|
| [0001](adr/0001-mnu-action-command-boundary.md) | MNU: Actions live in the file, Commands come from the host by control name |
| [0002](adr/0002-mnu-round-trip-preserves-superset.md) | MNU: round-trip preserves the format superset |
| [0003](adr/0003-no-raw-passthrough-create-from-scratch.md) | MNU: no raw byte passthrough; documents are created from scratch |
| [0004](adr/0004-audio-selection-pushdown.md) | Audio: member selection pushed down into portable `libs/audio` |
| [0005](adr/0005-mnu-var-expansion-policy.md) | MNU: `%VAR%` expansion policy |
| [0006](adr/0006-unified-mission-runtime-present-pass.md) | One mission runtime, one present pass, one entity index for game and editor |
| [0007](adr/0007-skeletal-runtime-and-entity-visual.md) | Skeletal `.bad`/`.adm` runtime and the `NovaEntityVisual` contract |
| [0008](adr/0008-pff-writer-policy.md) | PFF writer: zero timestamp/checksum for new entries, verbatim for retained |
| [0009](adr/0009-in-match-net-seam.md) | In-match networking enters the world tick through one seam (NetSystem + INetCommandSink) |

## RE records by domain

| Domain | Doc | Status |
|---|---|---|
| Audio | [`audio/lwf-dbf-sound-re.md`](audio/lwf-dbf-sound-re.md) | landed |
| Audio | [`audio/mus-sbf-re.md`](audio/mus-sbf-re.md) | landed |
| Environment | [`env/env-tod-re.md`](env/env-tod-re.md) | landed |
| Interface | [`interface/rtxt-strings-re.md`](interface/rtxt-strings-re.md) | landed |
| Menus | [`mnu/menu-re.md`](mnu/menu-re.md) | landed |
| Menus | [`mnu/menu-wiring.md`](mnu/menu-wiring.md) | landed (host wiring, architectural) |
| Mission | [`mission/bms-event-runtime-re.md`](mission/bms-event-runtime-re.md) | landed |
| Net | [`net/novaworld-net-re.md`](net/novaworld-net-re.md) | relanded on the `web-nw-for-real-master` integration branch (see `plan/`) |
| Particles | [`particles/ptl-format-re.md`](particles/ptl-format-re.md) | redesign in flight in a worktree |
| 3DI | [`threedi/3di-gp-format-re.md`](threedi/3di-gp-format-re.md) | landed (`libs/threedi`) |
| 3DI | [`threedi/3di-lw-format-re.md`](threedi/3di-lw-format-re.md) | unlanded: Land Warrior import, PR #45 closed |
| World | [`world/world-wac-ai-re.md`](world/world-wac-ai-re.md) | landed |

Systems documented mainly by code and tests so far (no dedicated RE record
yet): terrain, foliage, tiles, fonts, credits, the importer pipeline, and the
VFS/PFF mount stack (the PFF write side is ADR 0008). The project glossary
lives at the repo root in [`CONTEXT.md`](../CONTEXT.md); the project vision is
[`GOALS.md`](../GOALS.md).
