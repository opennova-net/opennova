# ADR 0029: engine target topology — five group archives, per-lib targets retired

- **Status**: accepted (2026-08-08; the 2026-08-07 flattening assessment's
  "Shape A", maintainer-approved)
- **Owners**: build/link topology
- **Supersedes/updates**: ADR 0024 decision 1's per-format-CMake-target clause
  and decisions 2–4 (the family link groups) — its one-directory-per-format
  layout, the decision-5 renderer reversal, and the decision-6 Model A/B
  consumption models stand. ADR 0028 §3's target-name clause
  (`opennova_<domain>` per-lib targets). ADR 0020's terrain seam survives,
  re-homed at include level (decision 5).

## Context

ADR 0028 grouped the engine tree but left the build enumerating it: ~53 STATIC
and 3 INTERFACE per-domain targets (ADR 0024 decision 1), the two family
INTERFACE groups whose entire purpose was shortening consumer link lists
(ADR 0024 decision 2), and an `opennova_novaworld` INTERFACE umbrella doing the
same for the net stack. Consumers never wanted the granularity — the
GDExtension linked families, tests and apps hand-kept member lists,
`opennova_shared` whole-archived a curated target list — and the granularity's
one live product was the target-level seam enforcement (ADRs 0019/0020) in
`link_graph_check.py`, which does not need it (decisions 4–5).

The 2026-08-07 five-agent flattening assessment proposed collapsing the layer
("Shape A"); the maintainer approved it 2026-08-08. The honest cost — coarser
incremental relinks — was named up front and is accepted (Consequences).

## Decision

1. **Five buildable (STATIC) group targets, two INTERFACE.**
   - `opennova_formats` — every `engine/formats/<domain>` lib's sources, plus
     the mission-format sources from `engine/runtime/mission` (decision 2).
   - `opennova_base` — vfs, resource_index, gameprofile, pcapio, refs.
   - `opennova_runtime` — every `engine/runtime` lib, minus mission's format
     half.
   - `opennova_net` — novacrypto, napi, npwire, netsim, npruntime, plus
     novaworld's session and gate legs.
   - `opennova_novaworld_service` — the novaworld service leg, deliberately
     separate (decision 4).
   `opennova_io` and `opennova_oned_edit` stay header-only INTERFACE targets,
   unchanged. Everything else — the per-lib targets, both family groups
   (`engine/families.cmake`), the `opennova_novaworld` umbrella — is deleted.
2. **`mission_format` builds inside `opennova_formats` (guardrail 1).** Not
   taste: without the fold there is NO acyclic four-group partition. Two
   witnessed edges collide: `engine/base/refs` consumes the mission document
   model (base → runtime, had mission_format ridden with runtime) and
   `engine/runtime/simassets` consumes `resource_index` (runtime → base) —
   base and runtime would cycle. With the mission format codec in
   `opennova_formats`, below base, both edges point down.
3. **One PUBLIC dependency chain, and base sits ABOVE formats.** formats links
   io; base links formats; runtime links base; net links runtime; the service
   links net — each edge PUBLIC. The inversion is deliberate and witnessed in
   the edges, not a naming accident: `base/` is repo plumbing that PARSES
   formats — vfs → pff/scr/bfc1 (mounting archives), refs →
   env/cbin/def/avatars/threedi/mnu (+ mission_format, decision 2). Only the
   header-only `io` sits below the formats.
4. **The NovaWorld service keeps its own target (guardrail 2)** so sqlite
   containment stays linker-enforced: `opennova_novaworld_service` alone links
   `opennova_sqlite`; `opennova_net` never does; the godot adapter links
   `opennova_net` and never the service. Folded into `opennova_net`,
   persistence would ride into every net consumer including the game client.
   `link_graph_check.py` keeps the surviving target-level rules — exactly this
   containment.
5. **ADR 0020's terrain seam moves to include level (guardrail 3), in the same
   PR.** At group granularity the seam is invisible: wac, mission, and terrain
   build inside `opennova_runtime`, the terrain formats
   (cpt/til/trn/tpj/foliage) inside `opennova_formats` — which runtime links
   PUBLIC — and `opennova_net` links `opennova_runtime` wholesale; no target
   edge is left to forbid.
   `scripts/lint/include_graph_check.py` (CI, hard-fail) enforces the same rule
   on includes: files under `engine/net/**`, `engine/runtime/wac/**`, and
   `engine/runtime/mission/**` may not include `cpt/`, `til/`, `trn/`, `tpj/`,
   or `foliage/` headers, nor `terrain/` headers other than terrain_query's
   exactly four — `terrain/{coords,height_field,surface_type_map,terrain_raycast}.h`.
   (The terrain and terrain_query libs expose DISJOINT header sets under the
   same `terrain/` prefix, so the allowlist is per exact header, never per
   directory.)
6. **No compatibility aliases.** Every consumer is rewritten in the same
   change — ≈280 test executables, the apps, and the GDExtension now link
   group targets. Pre-1.0 policy as usual: update every caller, no shims.

## Consequences

- **The accepted cost is incremental relink fan-out.** An edit under
  `engine/formats/` or `engine/runtime/` re-archives its whole group and
  relinks every test executable linking it, where a per-lib edit used to
  relink only the member archive's dependents.
- **The C ABI is unaffected.** `opennova_shared`'s whole-archive list
  (`OPENNOVA_CORE_TARGETS`) names group targets now; the previously
  whole-archived libs export only `OPENNOVA_API`-annotated symbols, so the
  exported surface does not move — the `abi_export_identity` baseline is
  byte-identical, no bump. `pyopennova` loads by symbol only and never sees
  targets.
- **Path-keyed instruments are unaffected.** The ratchets and maturity lints
  key on file paths, not targets; no baseline moves.
- The family groups (ADR 0024 decision 2) are deleted as subsumed, not
  replaced: a group target IS the family, with sources.

## What this does not change

- The directory tree: ADR 0028's four groups and ADR 0024's
  one-directory-per-format layout stand exactly —
  `engine/<group>/<domain>/{include/<domain>/, src/}` with fixtures and tests
  in place; `engine/runtime/mission` keeps its directory even though its
  format half builds in `opennova_formats`.
- The `opennova` namespace, the `<domain>/...` include paths, and every source
  file.
- The flat C ABI surface, its export conventions, and its baseline.
