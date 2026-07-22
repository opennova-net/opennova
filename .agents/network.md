# Network Architecture Guardrails

Redirect. The pre-NET-2 runbook that lived here named modules and classes the
npruntime rebuild retired; the maturity program's NET track re-homed its
content. Current owners:

- `libs/npruntime/ROADMAP.md` — the in-match runtime (server + client): the
  module map, frame order, what P8 retired, and the test-harness design. It is
  the completed P0–P8.2 build record, not live status.
- `docs/divergence-ledger.md` (`PAR-NET` slice) + `docs/net/novaworld-net-re.md`
  §8 — what is actually open in the in-match protocol today. Start here.
- `docs/adr/0013-consolidated-net-core.md` — the matchmaking/in-match split
  (`apps/novaworld_server` vs the in-match host), the message catalog, and
  `world::EntityRegistry` as server state authority.
- `docs/adr/0019-npwire-game-wire-lib.md` — `libs/npwire`, home of the in-game
  wire codec, NWU session framing, and the capture/replay chain.
- `.agents/interop.md` — capture, packet-diff, and debugging runbook, including
  the Initial-Load Failure Triage and Paths Not To Confuse sections that moved
  from this file.

The standing rules still hold and live in `.agents/README.md` Working Rules:
one in-match seam, no second gameplay network path, protocol/crypto/framing in
Godot-free libs. Hosting is a serve mode of `opennova.exe`, never a third
server product (`docs/adr/0015-two-products-serve-mode.md`).
