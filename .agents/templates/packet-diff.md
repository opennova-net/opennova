# Packet-Diff Task Template

## Goal

Prove an exact byte/field mismatch or confirm parity for one protocol surface.

## Inputs

- Capture or fixture path:
- Scenario:
- Direction:
- Opcode/tag/container:
- Expected retail behavior:

## Steps

1. Decode with existing tools first: `nw_pp`, `nw_replay --print-roles`, or the
   nearest CTest fixture.
2. Isolate the frame/message range.
3. Compare retail vs OpenNova by decoded tag order, length, and field values.
4. Map every differing byte to a known field in `docs/net/novaworld-net-re.md`.
5. Classify the result: code bug, docs bug, fixture gap, missing capture, or IDA
   witness needed.

## Output

- Offset/field table.
- Cited RE section or `[orig: ...]` witness.
- Affected tests or missing test.
- Proposed D-NET note if the finding is new.

