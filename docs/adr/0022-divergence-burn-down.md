# ADR 0022 - Divergence burn-down and the permanent register

Status: accepted (maintainer, 2026-07-05). Establishes the PAR (parity burn-down) track
of the maturity program and ratifies the permanent divergences listed below. The living
dashboard is [docs/divergence-ledger.md](../divergence-ledger.md); the program that
schedules the slices is [docs/maturity-program.md](../maturity-program.md).

## Amendment (2026-08-06) — closed-row retention written down

The previously unwritten retention convention is now the ledger's Standing rule
6: a closed row is CONDENSED to a dated prose closure line once its full detail
lives in its record's catalog (the record is the authoritative content home per
Consequence 1; the ledger stays the authoritative disposition dashboard). The
scoreboard's "Closed rows still tabled" column counts down to zero, and a fully
burned-and-pruned domain drops off the scoreboard. Ids remain stable and
greppable in the closure lines; `cite_census.py --audit-range <range>` (ADR
0043's census, which absorbed the retired `host_lint.py --frozen-audit`)
verifies every citation and id still resolves under docs/ after a move. Nothing in the
original decision changes — the zero-OPEN target, the vocabulary, and the
PERMANENT register are as written below.

## Amendment (2026-08-22) — D-RMAT-8 removed from the permanent register

The renderer now exposes the required gamma-domain scene target by construction:
retail shaders write and blend raw gamma numeric values, then a terminal
`FrameFxCompositorEffect` performs the sole display decode after all 3D and
particle draws. A live D3D12 probe pins all 256 transfer bytes and the exact
source-over/additive blend results. D-RMAT-8 is therefore `FIXED`, not a
structural exception. The superseded register entry was removed below; this
amendment preserves why the original 2026-07-06 decision changed.

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
grants a **per-slice freeze exemption for PAR slices** (2026-07-05). *(Status note
2026-08-04: the freeze LIFTED 2026-07-12 at program close — the exemption language is
retained as this decision's original context; PAR slices now run unexempted.)* The exemption is
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
- **D-RORD-2** — retail's per-frame CPU opaque quicksort (alpha-test bit → depth slabs →
  effect index → fine depth) is a device-era mechanism; the reimpl's internal opaque
  ordering serves the same intent, with the key semantics preserved as T1-pinned pure
  functions. (Ratified at REN-3; entry back-filled here 2026-07-06.)
- **D-THROW-2** — the bounce-kick and claymore-fan draws keep world-local streams of
  retail's generator shape instead of the shared process globals at `0x31BFBB0/B8`:
  distribution-faithful, every coupled value authority-drawn-and-shipped, so no draw
  order is observable — the D-NET-115 ratification applied to the throwable family.
  (Ratified 2026-08-29.)
- **D-COL-7** — the vertical ground probe reads the bilinear column height where retail
  marches + bisects (`Terrain_RaycastHeightmapHiRes_0 @ 0x60e710`); equal for vertical rays
  on a heightfield, and oblique rays already take `terrain_raycast_refined`. (Ratified 2026-08-29.)
- **D-MNU-1 / D-MNU-2 / D-MNU-3 / D-MNU-10** — the menu-layer structural choices tabled
  2026-08-04: per-field `%VAR%` expansion (rendered output identical, ADR 0005), the
  un-modelled per-play sound jitter draws, the authoring-superset format strictness
  (ADR 0002), and the offline PLAYER_CLASS spin (deliberate 2026-07-11: the runtime hosts a
  listen session even for SP, ADR 0009). (Ratified 2026-08-29.)
- **D-SND-1 / D-SND-3 / D-SND-4 / D-SND-10 / D-SND-13** — the sound-layer structural
  choices tabled 2026-08-04: the merged bank chain (a collision-free superset), the
  authoring-side parser strictness, the dialog FIFO (no overlap either way), the
  ChuteFlap/FreeFall voice coalescing (audibly equivalent), and the per-load SndProf.def
  parse (same file, same table). (Ratified 2026-08-29. D-SND-10 was superseded 2026-09-11:
  the coalescing is gone and the ledger closes it `FIXED` on retail's own-channel score-0
  reuse in `audio_find_and_open_channel @0x766E80`.)
- **D-LOADSCR-1 / D-LOADSCR-6 / D-LOADSCR-7** — the loading-screen structural choices
  tabled 2026-08-04: the coarser progress granularity (same values, same pump), the
  unmodulated background (MODULATE2X-neutral, net-identical), and the uninterruptible
  synchronous SP/host map load (no reachable poll window; the joiner waits honour ESC).
  (Ratified 2026-08-29.)
**Original-bug / garbage class** (basis:
[ADR 0003](0003-no-raw-passthrough-create-from-scratch.md)).

- **D-VEH-2** — water-ring removal clears the vacated tail of the compacted
  128-slot bank. Retail's `sub_5DDDB0 @0x5DDDB0` zeroes the removed slot and
  shifts the suffix down without ever clearing slot 127 (`@0x5DDDCB..0x5DDDFC`),
  and `sub_5DDE10 @0x5DDE10` steps its cursor back onto the removed index
  (`@0x5DDEAD..0x5DDEC0`), so a full bank whose duplicated last row expires is
  re-copied and re-expired forever. Clearing the tail preserves all surviving
  rings and bounds retirement; the saturation regression covers all 128 slots.
  **Proposed in PR #640 (2026-09-07) as the bounded implementation of the
  existing lifetime contract; requires maintainer ratification at merge — no
  sign-off has been recorded yet.**

- **D-WAC-1** — invalid IDIV operands (division by zero, INT_MIN/-1) stop the current WAC
  pass with a diagnostic; retail's `WacScript_ExecuteBytecode @0x4F58B0` executes the x86
  IDIV and faults the process. Malformed mission input must not crash a host.
  **Proposed in PR #642 (2026-09-09); requires maintainer ratification at merge — no
  sign-off has been recorded yet.**
- **D-WAC-2** — `pisvar`/`psetvar` indices outside the authored 0..16 byte bank return 0
  and write nothing; retail (`WacCmd_PlayerIsVar @0x4F0BD0`, `WacCmd_PlayerSetVar
  @0x4F0CB0`) checks only `index <= 16`, so a negative index reads or writes unrelated
  player-slot memory. **Proposed in PR #642 (2026-09-09); requires maintainer
  ratification at merge.**
- **D-WAC-3** — `weaponfired`/`blockfire` and the fire-request stamp refuse negative weapon
  categories; retail bounds only the high side (`WacCmd_WeaponFired @0x4ED360`,
  `WacCmd_BlockFire @0x4EE140`, `Input_HandleActionBinding_0 @0x4E0420`) and indexes the
  BSS before `dword_C6EA44` / `dword_C6EA6C` for a negative category. **Proposed in PR #642
  (2026-09-09); requires maintainer ratification at merge.**
- **D-GRM-1** — the GRM facial-rig parser rejects unsafe indices, excessive row/parameter
  counts, non-finite coordinates and field-overflow names, and treats names as data;
  retail's `FaceAnimConfig_ParseProperty @0x5886A0` writes unbounded indices and
  sprintf-format names into fixed fields. **Proposed in PR #642 (2026-09-09); requires
  maintainer ratification at merge.**
- **D-TMATE-1** — the teammate pickup/flyover operation initializes its helicopter reference
  before treatment and ends the operation on a failed helper allocation or a destroyed
  helicopter/teammate entity; retail's `HeliLift_SpawnPickup @0x4525E0` never initializes
  the pointer that `HeliLift_UpdateSlotState @0x451730` dereferences on treatment expiry
  (`@0x451e09`; `@0x451e4c`; `HeliLift_UpdateAll @0x451FA0` only compacts the slots and calls it).
  **Proposed in PR #642 (2026-09-09); requires maintainer ratification at merge.**
- **D-EVT-7** — the BMS loadout-record sanitizer bounds an incomplete tail to empty
  strings and retains oversized typed records; retail's `AIProfile_SanitizeConfigData
  @0x40cfe0` walks three verbatim strings plus the optional fourth field past the chunk
  and can overflow its 2048-byte temporary before capping the final copy. Reading past
  the record and overflowing a fixed temporary is memory corruption whose outcome depends
  on the adjacent bytes; well-formed records never reach the boundary
  (mission/bms-event-runtime-re.md §6.3a). **Proposed in PR #646 (2026-09-11) as a
  bounded format projection; requires maintainer ratification at merge — no sign-off has
  been recorded yet.**
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
