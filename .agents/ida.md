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

## Analysis and Curation

The point of an IDA session is to leave the database more correct than you found
it. Read deeply, then write your understanding back. The detailed write rules
(confidence gates, dry-run, shared-state proposals, save cadence) live in
`.claude/skills/grill-ida/IDA-WORKFLOW.md` — this is the analysis discipline that
feeds them.

1. **Decompilation analysis.** Inspect the decompiler output thoroughly. Work out
   the actual functionality and purpose of each component from the code itself —
   do not trust old or pre-existing comments and names; they are frequently
   wrong. Record what you find with `set_comments` / `append_comments` as you go.

2. **Improve readability in the database.** As understanding crystallizes, write
   it back (per the IDA-WORKFLOW confidence gates):
   - Rename variables and locals to sensible, descriptive names.
   - Correct variable and argument types where wrong — especially pointers and
     array types, where a bad type hides the real access pattern.
   - Update function names to describe their actual purpose, matching the IDB's
     `Subsystem_Action` style.

3. **Deep dive when needed.** When the decompilation is insufficient or looks
   wrong, drop to the disassembly (`disasm`, `insn_query`, `basic_blocks`) and
   comment the findings at their address. Document low-level behavior that isn't
   visible in the pseudocode (inlined `rep movsb`/`stosb`, register-passed args,
   fixed-point scaling, table indexing). Use sub-agents for detailed,
   self-contained analysis passes.

4. **Constraints.**
   - NEVER convert number bases by hand. Use the `int_convert` MCP tool for any
     hex/decimal/binary conversion — a mis-typed conversion poisons every
     downstream conclusion.
   - Use the MCP tools to retrieve information; do not recall addresses,
     offsets, or constants from memory.
   - Derive every conclusion from actual analysis of the binary, never from
     assumptions or from the existing (possibly wrong) names and comments.

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

