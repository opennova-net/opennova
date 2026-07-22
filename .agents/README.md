# OpenNova Agent Guide

These runbooks cover retail-compatible OpenNova networking. The goal is not
to invent a new multiplayer architecture. The goal is to make retail clients,
retail hosts, OpenNova clients, OpenNova listen hosts, and future dedicated
hosts speak the same in-match protocol.

Orient from these for any networking task — consult what the task needs rather than
reading end to end (the net RE record is grep-navigated: §5 index at its top,
divergences in its §8 catalog):

- `CLAUDE.md`, `CONTEXT.md`, and `GOALS.md`
- `docs/README.md` and `docs/engine-primer.md`
- `docs/adr/0009-in-match-net-seam.md`
- `docs/adr/0010-novaworld-client-completion.md`
- `docs/adr/0011-single-player-in-process-listen-server.md`
- `docs/adr/0012-player-is-host-side-server-entity.md`
- `docs/net/novaworld-net-re.md`
- `.agents/interop.md`, `.agents/ida.md`, and `.agents/debug.md`
  (`.agents/network.md` is now a redirect to the current architecture owners:
  `libs/npruntime/ROADMAP.md`, ADR 0013, ADR 0019)
- `docs/divergence-ledger.md` — the `PAR-NET` slice is the live open-work list for
  in-match networking; `libs/npruntime/ROADMAP.md` is the completed build record
  behind it, not current status
- `.agents/porting-0a-emit.md` — runbook for porting the per-frame S2C 0x0A emit
  from the witnessed retail chain (phase counter + sub-blocks + priority/budget
  entity loop), with the verify loop (`scripts/net/diff_0a.py` + the golden) and
  the current ported-vs-not state.

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
- Treat the source and `docs/net/novaworld-net-re.md` as live truth; `plan/` holds
  completed-effort records (the NovaWorld-integration status tables), not current state.

## Consolidated net core (ADR 0013)

- **Message catalog is the single source of truth**:
  `libs/npwire/include/npwire/ingame_message_catalog.h` maps `(dir, tag) → name →
  coverage → decoder → doc §`, shared by `nw_pp` and the `nw_message_coverage` gate. Add a
  message there first; `nw_pp --coverage <capture>` ranks the undecoded backlog by volume.
- **Capture → validate-vs-golden loop**: host from the Godot game, `dumpcap`, then
  `scripts/net/diff_vs_golden.ps1 -Ours <cap> -Golden .scratch/golden/retail-gameplay-session.pcapng`
  (GAP = worklist, SPURIOUS = regression). Pin in CI with the env-gated `nw_golden_diff`
  ctest. Full loop in `scripts/net/README.md`.
- **Server state authority is `world::EntityRegistry`** — the net layer reads handle/pose/
  team *through* it, never a parallel cache. Host bring-up is the one shared
  `np::start_host_session` helper. See `docs/adr/0013-consolidated-net-core.md`.

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

