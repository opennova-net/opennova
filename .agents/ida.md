# IDA and Original-Code Reconstruction

Use docs first. Use live IDA when behavior is unwitnessed, when a divergence is
suspected, or when a task explicitly asks for an IDA grill.

## Choose The Path

- Docs-only: behavior is already witnessed in an ADR, RE record, correspondence
  row, or existing `[orig: ...]` marker. Cite it and stop.
- `engine-research`: find how original retail behaved for new or unwitnessed
  behavior.
- `grill-ida`: verify an existing OpenNova implementation against retail or
  recheck a known divergence.

If live IDA is unavailable, do not call the result a grill. Report it as a
docs-only review or blocked IDA witness.

## Preflight

Before drawing conclusions from IDA:

- Confirm `ida-pro-mcp` is reachable.
- Confirm the active database is retail `Jointops.exe`, not demo or another
  title unless the task explicitly says otherwise.
- Confirm imagebase `0x400000`.
- Prefer the known `Jointops.exe.kong.i64` database when available.
- Run a minimal binary survey before relying on names or addresses.

## Evidence Contract

Every recreated-original-code claim needs an address-backed witness:

`[orig: Name @ 0xADDR]`

Record confidence:

- Anchored: function identity and behavior are directly tied to xrefs, strings,
  constants, call graph, captures, or tests.
- Probable: strong local evidence but one axis is still indirect.
- Guessed: exploratory only. Do not mark matching or divergent from guessed
  evidence.

Compare the exact axis relevant to the task: constants, struct offsets,
rounding, signedness, state transitions, call order, packet order, edge cases,
tables, and captures.

## Safety

- Do not commit raw decompiled code.
- Do not patch bytes or mutate the retail binary.
- Do not use IDA write tools unless the workflow and permissions explicitly
  allow them.
- If you rename, type, or comment IDB state, keep changes small, confidence
  gated, and saved according to the IDA workflow.

## NovaWorld-Specific Rule

Encoder or protocol-session changes require a retail capture or existing
`docs/net/novaworld-net-re.md` witness. If neither exists, stop and classify the
missing evidence before coding.

