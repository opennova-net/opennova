# ADR 0022 - Divergence burn-down and the permanent register

Status: accepted (maintainer, 2026-07-05). Establishes the PAR (parity burn-down) track
of the maturity program and ratifies the permanent divergences listed below. The living
dashboard is [docs/divergence-ledger.md](../divergence-ledger.md); the program that
schedules the slices is [docs/maturity-program.md](../maturity-program.md).

## Context

OpenNova is a faithful reimplementation — parity, not reinterpretation
([GOALS.md](../../GOALS.md)). The documentation rule is *a divergence is a tracked decision,
never an accident* ([docs/README.md](../README.md)). But "tracked" had drifted into
"documented and left alone": ~250 tracked entries across 13 `D-`prefixes accumulated in
the RE records, in three different dialects (env's Disposition column, the D-NET
`[SEVERITY, STATUS]` tags, and prose "accepted/intentional" notes), with no single view
of what was still open, and no policy that every open item must eventually close.

Three problems followed:

1. **No target.** An open divergence could sit indefinitely. There was no commitment that
   every one either gets ported-and-closed or ratified permanent.
2. **The freeze would block the cleanup.** The maturity program's freeze
   ([docs/maturity-program.md](../maturity-program.md)) holds new reimplementation work
   for its foundation phase. Divergence closures are reimplementation work, so the freeze
   would defer the whole burn-down to late waves.
3. **Seven systems have no RE record** (terrain, foliage, tiles, fonts, credits, the
   importer pipeline, the VFS/PFF mount stack). Their divergences, if any, are *untracked*
   — invisible to any ledger built only from existing records.

An inventory sweep (2026-07-05) confirmed the shape: D-NET dominates (~160 entries, mostly
closed); env carried a bare `#1..#21` list; several records (3di-gp, 3di-lw, ptl, mis)
tracked divergences in prose with no ID catalog at all.

## Decision

**1. Zero-OPEN target.** Every tracked divergence is driven to one of two terminal
states: `FIXED` (behavior matches, closing commit cites the witness) or `PERMANENT` (a
ratified deliberate divergence, citing this ADR's register or a domain ADR). `OPEN`,
`NEEDS-RE`, and `WITNESSED-READY-DEFERRED` all count as open and all must resolve.

**2. The canonical disposition vocabulary is normative.** The six terms in
[docs/divergence-ledger.md](../divergence-ledger.md) — `OPEN`, `NEEDS-RE`,
`WITNESSED-READY-DEFERRED`, `FIXED`, `PERMANENT`, `UNAUDITED` — replace the three legacy
dialects. New RE-record catalog entries use them; existing catalogs adopt them on their
next touch. The faithful-vs-open axis from
[env-honored-matrix.md](../env/env-honored-matrix.md) holds: *unconsumed-in-retail-too* is
legitimately closed; *awaiting-a-ported-consumer* is `OPEN` and must not be faked.

**3. The burn-down starts NOW, in parallel with Wave 1 — freeze-exempt.** The maintainer
grants a **per-slice freeze exemption for PAR slices** (2026-07-05). The exemption is
narrow: it covers the parity burn-down (closing tracked divergences and auditing the
unaudited systems), not general new-feature reimplementation. The freeze otherwise stands
for non-PAR work.

**4. Env closures go libs/env-first.** Environment divergences (ledger env #14–#21) are
implemented in `libs/env` first, so the ENG-2 port (env GDScript → `libs/env`) inherits
them rather than paying for the same cited math twice.

**5. The seven unaudited systems get research audits (PAR-R1..R7).** Each is an
engine-research / grill-ida session that lands an RE record **with a D-catalog**, so the
system's divergences become tracked rows rather than unknown unknowns.

**6. The permanent register (below) is ratified.** Each listed entry is a deliberate,
closed-as-`PERMANENT` divergence, verified against its record, with the rationale for why
porting it would be *wrong*. [ADR 0003](0003-no-raw-passthrough-create-from-scratch.md)
(no raw passthrough; create from scratch) is the standing basis for the garbage-class
entries: reproducing an original's stale-memory or UB bytes to force a match is
prohibited, so deliberately *not* reproducing them is the faithful choice.

### Permanent register

**Platform / reimpl-structural.** The reimpl cannot or should not reproduce the original's
substrate; the divergence is wire/visually equivalent or reimpl-internal.

- **D-3DI-1** — byte-exact MTRX output requires OED's x87 `_PC_24` (24-bit) precision; a
  64-bit SSE2 build diverges in low FP bits. The record documents the
  `_controlfp(_PC_24, _MCW_PC)` parity sub-build for when byte-exactness is needed.
- **D-MNU-4** — the original truncates each scaled quad rect to int per element; the
  reimpl applies one float `CanvasItem` scale. A sub-pixel cosmetic difference.
- **D-NET-140** — the listen host's own loopback gets the full 0x0A record set (retail
  sends its local player header-only frames); the frame never leaves the process
  ([ADR 0011](0011-single-player-in-process-listen-server.md)).
- **D-RORD-2** — retail's per-frame CPU opaque quicksort (alpha-test bit → depth slabs →
  effect index → fine depth) is a device-era mechanism; the reimpl's internal opaque
  ordering serves the same intent, with the key semantics preserved as T1-pinned pure
  functions. (Ratified at REN-3; entry back-filled here 2026-07-06.)
- **D-RMAT-8** — framebuffer blending runs on the reimpl's blit-encoded (linear) values;
  retail blends gamma bytes. Under D-RMAT-7's gamma-space convention, opaque and
  alpha-tested surfaces display byte-exact; translucent composites diverge boundedly
  (alpha midtone shift, additive accumulates dimmer). Blending in gamma space would
  require a gamma framebuffer the reimpl does not expose; reopen only if a T3 scene shows
  an objectionable composite.

**Original-bug / garbage class** (basis:
[ADR 0003](0003-no-raw-passthrough-create-from-scratch.md)).

- **D-RORD-6** — the two original sort-key quirks (opaque key bits 15+ carry residual
  stack garbage; the transparent key lags one strip within a render object) are not
  reproduced — reproducing either manufactures garbage. (Ratified at REN-3; entry
  back-filled here 2026-07-06.)

- **env #11** — the original packs negative color components as garbage (no lower clamp);
  the reimpl clamps to 0 (no UB replication).
- **D-NET-133 (empty-slot facet)** — an in-capacity EMPTY 0x18 slot replies a zeroed
  type-0 record; retail serializes the slot's raw (stale) memory. Observably identical (the
  client stops at the type gate), and reproducing stale bytes would be garbage.
- **D-MUS-7** — `op_callvl` resolves against uninitialised-BSS in Jointops, so the opcode
  is dead; the reimpl mirrors the dead stub (push 0).
- **D-MUS-5** — `inc_g`/`dec_g` operate on 1 byte and raise no globals-dirty notify,
  mirroring the original's silence.
- **D-PTL-1** — the engine's outer dispatcher remaps `g{N}_color{M}` into higher color
  slots (a parse bug); the reimpl maps them correctly. A recorded intentional divergence.
- **D-SCR-1 / D-SCR-2** — the SCR container codec accepts version bytes 0–2 and selects the
  key from the version byte + policy (each original call site fixes the key). A deliberate
  multi-title superset; load-bearing equivalence holds for all retail JO data.

## Consequences

- **One dashboard, one vocabulary.** [docs/divergence-ledger.md](../divergence-ledger.md)
  is the single view; the per-record `D-`catalogs are its authoritative sources. A
  disposition change updates the ledger in the same PR.
- **The burn-down can run alongside Wave 1** without waiting for the freeze to lift, but
  only for PAR slices; everything else honors the freeze.
- **`PERMANENT` is a real terminal state.** The register entries are closed for good unless
  a future need reopens one (each names its follow-up path). They do not count against the
  zero-OPEN target.
- **The unaudited systems stop being blind spots.** PAR-R1..R7 either produce a clean
  record (no divergences) or tracked rows — either way the ledger becomes complete.
- **A new divergence is born tracked.** The standing rules
  ([docs/divergence-ledger.md](../divergence-ledger.md)) make "found it" and "tracked it"
  the same act, so the ledger cannot silently fall behind the tree.
