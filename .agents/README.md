# OpenNova Agent Guide

This branch is focused on retail-compatible OpenNova networking. The goal is not
to invent a new multiplayer architecture. The goal is to make retail clients,
retail hosts, OpenNova clients, OpenNova listen hosts, and future dedicated
hosts speak the same in-match protocol.

Read these first for any networking task:

- `CLAUDE.md`, `CONTEXT.md`, and `GOALS.md`
- `docs/README.md` and `docs/engine-primer.md`
- `docs/adr/0009-in-match-net-seam.md`
- `docs/adr/0010-novaworld-client-completion.md`
- `docs/adr/0011-single-player-in-process-listen-server.md`
- `docs/adr/0012-player-is-host-side-server-entity.md`
- `docs/net/novaworld-net-re.md`
- `.agents/network.md`, `.agents/interop.md`, `.agents/ida.md`, and
  `.agents/debug.md`

## Working Rules

- Retail wire compatibility is the target. Original binary witnesses, retail
  captures, and tracked RE docs outrank guesses.
- Do not create a second gameplay network path. LAN, NovaWorld-routed joins,
  and future dedicated hosting must converge on the same in-match seam.
- Unknown packets and mismatches are tracked, not silently ignored.
- Do not commit raw decompiled code, raw retail captures, secrets, account data,
  local install paths, or machine-specific IP addresses.
- Keep protocol logic in Godot-free libraries. Apps and Godot bindings own
  process, sockets, UI, and presentation.
- Do not use raw passthrough blobs to make a writer or encoder pass parity.
  Model the fields structurally unless a tracked ADR explicitly says otherwise.
- Treat `plan/status.md` and source as newer than old phase notes when they
  disagree.

## Task Routing

- Packet mismatch or retail interop failure: start with
  `.agents/templates/packet-diff.md`.
- Unwitnessed original behavior or suspected divergence from retail:
  start with `.agents/templates/ida-witness.md`.
- Local, retail, or live-service reproduction:
  start with `.agents/templates/live-repro.md`.
- Cleanup or deduplication:
  start with `.agents/templates/safe-refactor.md`.

## Report Format

Every networking report should include:

- Target path: OpenNova host, retail host, OpenNova joiner, retail joiner, or
  dedicated/headless host.
- Evidence: test names, capture names, decoded packet tags, IDA addresses, or
  source lines.
- Verdict: matching, divergent, unknown, blocked, or docs-only.
- Next action: narrow code fix, capture needed, IDA witness needed, test gap, or
  no change.

