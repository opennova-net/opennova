# ADR 0031: the adapter composition contract — what earns C++ in godot/adapter/

- **Status**: accepted (2026-08-08; the adapter-composition round,
  maintainer-approved). **Superseded as the standing contract by
  [ADR 0033](0033-engine-owned-loops-device-shells.md) (2026-08-09)** — one
  seam, the device boundary. The census, the §3 dispositions, and the
  push-down record remain valid history; the five bands are no longer a live
  rule, and the citation ratchet survives only as ADR 0033's transition gauge.
- **Owners**: shell adapter layout
- **Supersedes/updates**: nothing becomes false. Extends ADR 0028 decision 5
  (whose standing rule is scoped to *GDScript* in the adapter) to the
  adapter's C++; gives ADR 0016's "bindings and thin wrappers" its first
  instrument.

## Context

The maintainer asked why `godot/adapter/` holds so much C++. Measured at this
round: **68,233 lines of C++ across 247 files and 116 GDExtension classes**
(every one registered), beside 37,218 lines of GDScript — and the two
languages occupy an almost disjoint directory seam (`world/` and `debug/` are
pure GDScript; `simulation/`, `mnu/`, `terrain/`, `object/`, `particle/`,
`audio/`, `network/`, `cbin/` are pure C++). The four largest C++ clusters
(simulation 13.7k, mnu 11.7k, terrain 8.5k, object 8.3k) are 62% of the
total.

The structural finding is not that the adapter is too thick — most of it is
exactly what an adapter is — but that **nothing said so anywhere**. The only
written contract was "thin wrappers only" (adapter CLAUDE.md; ADR 0016's
"bindings and thin wrappers"), with no instrument measuring it. Both
push-down programs targeted GDScript: ADR 0028's standing rule reads "GDScript
in `godot/adapter/` wires, adapts, and presents", and the S16–S24 slices plus
the #437 residue audit were GDScript-thinness audits. Only two slices ever
audited adapter C++ directly — #434 (mnu: the witnessed layout math moved to
`engine/formats/mnu`, the Control-tree writes stayed) and #435 (simulation:
the loadout/weapon TUs judged a world↔net↔dict bridge, one inline predicate
moved) — so roughly 25k of the 68k had a written rationale and the rest had
none. Meanwhile half of the adapter's 630 `[orig:]` citations sit in
`simulation/` alone.

## Decision

### 1. The five bands — what adapter C++ is allowed to be

Every C++ file under `godot/adapter/` belongs to exactly one band, and the
band names the reason it is C++ in the adapter rather than engine code or
GDScript:

| Band | Share today | Why it lives here |
|---|---|---|
| **Binding glue** — ClassDB registration, type marshalling, class declarations | 22–26% | The GDExtension tax: every engine API surfaced to scripts needs a C++ wrapper. This band GROWS with each push-down — thinning GDScript mints binding C++. That is the intended direction, not a failure. |
| **ONED document surface** — `NovaMnuDocument`, `NovaTerrainData`, `NovaMissionData`, the object-data family | 22–26% | Godot-Resource-shaped edit/undo surfaces the editor drives. Godot-typed by design (ADR 0016-era decisions); their *format semantics* still belong engine-side (ADR 0030 — the kda push-down #440 is the model). |
| **Presentation** — particle renderer/compositor, foliage dispatcher, terrain mesh build, the present appliers, the mnu builder | 18–22% | RenderingServer / RenderingDevice / MultiMesh / Skeleton3D bulk paths: must be Godot-typed AND fast. Only possible home. |
| **res:// loaders/savers** — 24 registered format objects | 3–4% | Godot resource-system integration over engine parsers. *(Band deleted by ADR 0032 — the census found the resource system structurally unused; the layer is gone and Nova formats use direct document I/O.)* |
| **Documented seam bridges** — the simulation pump/bridge legs | (within simulation/) | What a portable bridge cannot own (the S7b shape): sockets (deposit/send), device input application, the dict/profile conversion seams, the F3 clocks. Each leg carries its disposition in a header note; its `[orig:]` cites document *sequencing contracts* at the seam, not live math. |

What no band covers — engine-grade logic (witnessed math, gameplay rules,
state machines over engine types) — does not belong in adapter C++ at all.
**ADR 0028's standing rule now applies to the adapter's C++ exactly as it
applies to its GDScript**: new engine logic starts in `engine/`; the adapter
wires, adapts, marshals, and presents.

### 2. The instrument

`scripts/lint/ratchet_counts.py` gains `adapter_cpp_orig_cites` (baseline
630, landing with this round): the count of `[orig:` citations across
`godot/adapter` C++. A witnessed behavior cited in the adapter is either a
documented seam contract or a push-down candidate — so a NEW citation
requires a deliberate, logged baseline bump, and push-down slices bank their
decreases. "Thin wrappers only" is measurable for the first time.

### 3. The simulation/ dispositions — file by file

`simulation/` is the one directory where the bands genuinely interleave, so
its verdicts are recorded here (state at this round; 317 citations — half
the adapter's total):

| TU | Verdict |
|---|---|
| `client_replica_present_projection.*` | **Moved** to `engine/net/npruntime` with its composition helpers (`client_replica_present.h`); the `PF_*` row layout became the engine contract `world/present_rows.h` and `NovaSimulation`'s bound enum is a value-assigned re-export. |
| `nova_simulation_assets.cpp` | **Re-opened** (maintainer decision 2026-08-08, overriding the S7b "render-coupled asset resolution" leg): the collision/occlusion resolution loops and the seat-spec install family move to `engine/runtime/simassets` beside the receptacles that already exist; the adapter keeps thin model-loading hooks. The shape-C2 round also moved the mounted-pose CTRL-bus composition and PANM clock into `simassets/mounted_pose`. |
| `nova_simulation_player_loadout.cpp` | **Split re-opened** (same decision): the joiner 0x2F kit pushes and session-kit seed/reseed move engine-side; the dict conversion + playersav profile READING stay, per the S7b seam shape. The shape-C2 round moved the remaining live math: the mount-toggle gate/candidate (`world/vehicle_attach`), the mission chunk-tuple stash (`mission/promote`), and the 0x5A grant→kit conversion (`npruntime/loadout_submit`). |
| `nova_simulation_net.cpp` | Stays: socket + signal pumps over the npruntime state machines. Its cites document bring-up sequencing at the seam. |
| `nova_simulation.cpp` | Stays: lifecycle binding — construct/reset, the mission-boot step wiring (the ORDER contract lives engine-side in `mission/runtime_boot`), WAC install, mission vars. |
| `nova_simulation_occlusion.cpp` | Stays: verified a binding over `world::occlusion` — camera marshalling in, `occlusion_world_` calls through, debug dictionaries out. Its cites document call order at the binding site. |
| `nova_simulation_player*.cpp` (player / view / weapon) | Stay: the local-player pumps delegating to `world::local_*` (the S6a/S7a/S22 close-outs); dict/device legs per the seam band. |
| `nova_simulation_present.cpp` | Stays: presentation reads — packed snapshot assembly plus role/world enrichment over the engine projection. |
| `nova_present_applier*.cpp` | Stay: presentation — packed rows onto `Node3D`/`Skeleton3D` via cached StringName dispatch. |
| `nova_simulation_bind.cpp` | Stays: pure ClassDB registration. |
| `nova_simulation_internal.h` | Stays: the family's Godot-type packers and the using-declaration re-exports over moved engine helpers (the S4b/S10a house pattern). |

### 4. The growth mechanism, stated once

Pushing logic out of GDScript *increases* adapter C++ (each new engine API
needs binding surface), and pushing logic out of adapter C++ *decreases* the
citation count but not necessarily the line count. Line count is therefore
NOT the health metric for this layer; band conformance and the citation
ratchet are. Nobody should re-derive this from scratch again: when the
adapter looks "too thick", the question is which band the thickness sits in.

## Consequences

- The remaining engine-grade residue in `simulation/` moves in this round's
  slices (the projection landed first; the assets sweep and the loadout 0x2F
  family follow), each rewriting the S7b note in
  `npruntime/joiner_world_bridge.h` in the same commit so the disposition
  never goes stale.
- New adapter C++ is reviewed against the bands: a PR adding witnessed math
  or engine-typed algorithms to `godot/adapter/` either moves it engine-side
  or documents a seam disposition and bumps the citation baseline, logged in
  docs/maturity-program.md.
- The mnu slab keeps its #434 disposition (layout math engine-side, widget
  and Control-tree code adapter-side); the ONED document surfaces keep
  delegating format semantics engine-side per ADR 0030.
