---
name: grill-ida
description: Grilling session that challenges a reimplemented system against the original binary witnessed in IDA Pro via the ida-pro-mcp server. Finds the reimpl code and the corresponding original functions, decompiles the originals, and interrogates every behavioral divergence — constants, struct layout, control flow, fixed-point scaling, edge cases — updating IDA, the source, and the tracked RE record inline, ending in a per-system verdict landed via the re-doc skill. Use when verifying a reimplemented system matches the original, when re-verifying after a divergence fix, or when a TODO says something needs an IDA grill. For a new question about the original engine with no reimpl to verify, use engine-research instead.
argument-hint: "system name (e.g. 'bms loader', 'sound stack', 'terrain LOD')"
---

# Grill a reimplemented system against Jointops.exe

The original binary, seen through IDA Pro, is the source of truth. Interview
the user relentlessly about whether the reimplementation of the named system
matches the original, until a shared, evidence-backed understanding is
reached. Walk every function, constant, struct field, and control-flow
decision; provide your recommended answer with each question; ask one
question at a time and wait for feedback. When the decompilation can answer,
decompile instead of asking; when the reimpl source can answer, read it
instead of asking.

This is the evidence-producing half of this repo's RE loop: grill-ida
interrogates and fixes inline; the `re-doc` skill lands the tracked record.
Tool driving and write rules: [IDA-WORKFLOW.md](IDA-WORKFLOW.md).
Cross-session procedures (resume, re-verify a fix, IDB rebuild, drift
check): [LIFECYCLE.md](LIFECYCLE.md).

## 0. Preconditions — IDA reachable, right image

The ida-pro-mcp MCP server is optional user-level tooling, present on the
maintainer's machine only. If `mcp__ida-pro-mcp__*` tools are not available,
or `server_health` fails: STOP and say a grill needs IDA running with the
ida-pro-mcp plugin and the project IDB loaded. The degraded path — the
LIFECYCLE.md drift check over docs/ markers plus the behavioral ctests — is
useful, but never call docs-only work a grill or emit verdicts from it.

When reachable: `list_instances` → `select_instance` → `survey_binary`
(minimal detail; this is a >10k-function binary), then verify identity
against the repo pin in `docs/correspondence.md`: retail `Jointops.exe`,
imagebase `0x400000`, IDB `Jointops.exe.kong.i64`. Every address in docs/
is absolute in that image. A mismatch means a demo/other IDB or a rebuilt
one — stop and resolve (Troubleshooting in IDA-WORKFLOW.md; rebuilds:
LIFECYCLE.md §3) before trusting any address.

## 1. Read the witnessed record before touching IDA

In order: the system's rows and tables in `docs/correspondence.md`; the
domain's `docs/<domain>/<system>-re.md`; `[orig:` markers in the reimpl
source. Prior grilled work is input, not
suspect: trust but spot-check (LIFECYCLE.md §1), never re-derive. Each grill
must end with more anchored pairings, more grilled axes, and higher
confidence than it started with.

Triage what is grillable at all: components verdicted `host code / not
grillable` (Godot-idiomatic glue, e.g. playback/bus routing) are proven by
behavioral tests, not address-by-address comparison — do not grill them;
name them as such in the verdict. `MATCHING (behavioral proof)` systems are
re-verified through their golden tests first, IDA second.

## 2. Establish correspondence, then challenge every axis

Pair each reimpl symbol to its original with a stated method and confidence
— **anchored / probable / guessed** (evidence ladder: IDA-WORKFLOW.md §2).
Guessed pairings are read-only and end `unknown`, never `divergent`.
Pairings confirmed in earlier sessions count as anchored — spot-check,
don't re-derive.

Pull each original (`analyze_function`; `define_func` first if the address
is not yet a function) and interrogate every axis against the reimpl,
citing an IDA address for every claim:

- **Constants** and their **fixed-point scale / units** (16.16 vs 8.8,
  per-tick vs per-second — the known engine scales are tabled in
  `docs/engine-primer.md`)
- **Struct fields** — offset, width, sign, type; validation functions are
  free struct documentation
- **Control flow and call order**
- **Edge cases** the original guards (bounds, sentinels, soft-error/EOF,
  CRC gates)
- **Rounding, truncation, signedness**, and the tick model
- **Data-driven behavior** — lookup tables, const arrays, enum/flag bits;
  divergence hides in data as often as in code
- **Compiler artifacts** — inlined memcpy/memset, MSVC ECX/EDX fastcall,
  strength-reduced math: these look divergent and usually aren't

Make every question earn its turn: hypothesis-driven ("the original divides
by 16384.0 @ 0x47d3a2; the reimpl uses 8192 — which is right?") beats
open-ended; never ask what the decompilation or the source already answers.
Large systems: triage order is loaders/init → core state machines →
validators → support code. The user may defer a function — record it as
`unknown` and move on; deferral is documentation, not loss.

## 3. Update inline — three targets

Capture each crystallized understanding immediately (never batch the
understanding), announcing every edit on its own line:
`target ← tool addr old→new (confidence: reason)`.

1. **IDA** — names, types, comments per IDA-WORKFLOW.md §4. The shared-state
   rule: auto-names (`sub_`, `dword_`, `loc_`) rename directly at anchored
   confidence (dry_run first, stop_on_error, never overwrite); changing a
   human-curated name, or any probable-confidence type edit, is proposed
   first and applied only on the user's OK. Log every applied IDB change in
   the session notes — the RE record carries an "IDB changes made during
   the session" section. `idb_save` at checkpoints, not per edit.
2. **Source** — on confirmed correspondence, insert the marker above the
   definition, address-first: `// [orig: Name @ 0xADDR]` (GDScript: `##`;
   one marker per definition). Fix confirmed divergences in code when the
   fix is unambiguous; otherwise record them for the verdict. Divergence
   comments cite the record: `docs/<domain>/<system>-re.md (D-...)`.
3. **The tracked record** — landed via `re-doc` before the session ends:
   correspondence rows in the docs/correspondence.md column shape, each
   divergence with its axis + address, the IDB-change log, and open
   questions as explicit unknown/follow-up entries. `docs/` stays pristine
   — it is the best current understanding of the original engine. Raw
   decompilation never lands in any file; quote it in the conversation
   only (docs/README.md rule).

## 4. Verdict and handoff

End with a per-system verdict in the repo vocabulary. Per row: `matching |
divergent | unknown`. Per component, for the record: **MATCHING**,
**MATCHING (behavioral proof)**, **MATCHING (read-only grill)**, **host
code / not grillable**, or **divergent** with every divergence cited to an
address. Kept divergences get stable `D-<DOMAIN>-n` ids when the record
lands — existing ids are never renumbered; gaps are documented, not closed.

Close the audit trail (markers inserted, IDB changes, final `idb_save`),
then invoke `re-doc` to land the golden record, the `docs/correspondence.md`
rows, and the cross-file updates — in the same session. Done = verdict
stated with addresses, the record landed via re-doc, and the IDB saved —
a grill that ends without landing is not done.
