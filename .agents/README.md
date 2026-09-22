# Networking agent runbooks

These guides are for retail-compatible OpenNova matchmaking and in-match
work. Start with [the network RE record](../docs/net/novaworld-net-re.md)
for witnessed behavior and the [divergence ledger](../docs/divergence-ledger.md)
for open parity gaps. [ADR 0043](../docs/adr/0043-canonical-cpp-and-godot-hard-cut.md)
owns the current architecture; the
[in-match roadmap](../engine/runtime/inmatch/ROADMAP.md) records its
completed build. [network.md](network.md) is a short redirect for older links.

## Choose a runbook

| Task | Guide |
| --- | --- |
| Capture, packet mismatch, or retail interop | [Interop](interop.md) and the [packet-diff template](templates/packet-diff.md) |
| Unwitnessed retail behavior or suspected divergence | [IDA](ida.md) and the [witness template](templates/ida-witness.md) |
| Local, retail, or service reproduction | [Debugging](debug.md) and the [live-repro template](templates/live-repro.md) |
| Retail/OpenNova LAN topology comparison | [Retail LAN parity](retail-lan-parity.md) |
| S2C 0x0A emit work | [0x0A witness and porting record](porting-0a-emit.md); its status table is historical |
| Cleanup without behavior changes | [Safe-refactor template](templates/safe-refactor.md) |

The [game MCP guide](../docs/mcp.md) covers runtime probes. Retail
automation uses the external onHook executable described in the LAN guide.

## Standing rules

- Retail packets and captures outrank guesses. Keep protocol and crypto
  in Godot-free engine code; apps and Godot own sockets, UI, and devices.
- Use the one in-match session and wire codec. Do not add a second gameplay
  network path or hide an unknown packet.
- Keep raw retail captures, decompiled code, credentials, account data,
  local paths, and machine-specific addresses out of tracked files.
- For a networking report, name the topology, evidence (test, capture,
  packet tag, or IDA address), verdict, and next action. Call a missing
  witness *unknown* or *blocked*, not matching.
