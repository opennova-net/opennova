# IDA and original-code reconstruction

Start with the [engine primer](../docs/engine-primer.md), owning RE record,
and [correspondence table](../docs/correspondence.md). If behavior is already
witnessed, cite the record. Use
[engine-research](../.claude/skills/engine-research/SKILL.md) for a new
retail question and [grill-ida](../.claude/skills/grill-ida/SKILL.md) to
compare an implementation with retail. The
[IDA workflow](../.claude/skills/grill-ida/IDA-WORKFLOW.md) owns detailed
writeback, confidence gates, and save cadence.

Before relying on a live IDA result, verify the MCP connection, active
binary, image base, and function identity. The usual witness is retail
`Jointops.exe` at image base `0x400000`; qualify another binary
explicitly. Prefer the curated `Jointops.exe.kong.i64` database when
available. Retrieve addresses and values from the binary, not memory.

An implementation claim needs an address-backed citation:
`[orig: Name @ 0xADDR]`. Distinguish anchored findings from probable
ones; a guessed pairing is exploratory, not a matching or divergent
verdict. Compare the axis the task needs: data layout, rounding,
state transitions, call order, wire bytes, or presentation.

Do not commit raw decompiled code or patch the retail binary. Record
confirmed corrections in the IDB and owning RE record under the workflow's
write rules. If IDA is unavailable, describe the work as a docs-only
review or an unwitnessed gap.

For NovaWorld encoder or session changes, require a retail capture or an
existing [network RE witness](../docs/net/novaworld-net-re.md). Classify a
missing witness before changing protocol behavior.
