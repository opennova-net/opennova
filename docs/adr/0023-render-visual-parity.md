# ADR 0023 - Render visual parity (the REN track)

Status: accepted (maintainer, 2026-07-05). Establishes the REN (render visual parity)
track of the maturity program — grill and reimplement the original renderer's
materials, batching/draw order, fixed-function shaders, and lighting so our output
looks identical to retail. The program that schedules the slices is
[docs/maturity-program.md](../maturity-program.md); the divergence dashboard is
[docs/divergence-ledger.md](../divergence-ledger.md); the RE records land under
`docs/render/`.

## Context

The 2026-07-05 planning grill found the runtime render path in a unique state: it is
the one substantially **reimplemented but unwitnessed** subsystem. The 45-entry
shader-tag table (`libs/oed/include/oed/material_descriptor.h`, static_assert-locked
to the raw `gMaterialInfoTable` dump in `oed/types.h`) and its consumption chain —
`libs/renderer` `classify_object_material()` → `ObjectMaterialClassification` →
`compose_object_shader_glsl()` → `NovaObjectShaderCache` → the ShaderMaterials
`nova_object_model.gd` builds — carry only ModSuperOed-side `[orig]` citations. Not
one Jointops runtime render address is cited anywhere in `libs/` outside `libs/env`,
no RE record covers the runtime material path, batching/draw order, the runtime TSS
stage tables, or lighting application, and draw order has no reimplementation at all
(the celestial priority ladder is the only ordering machinery in the tree; world
objects all render at priority 0).

Three islands ARE witnessed and are reused, never re-grilled: the env record's sky
TSS tables + vs_1_1 sources + dome→bodies→world order, the ptl record's
`RenderState_ApplyToDevice @ 0x681920` struct decode, and world §13's org-callback
6-draw order + `0x10000000` pass flag.

Two policy questions needed settling before slice 1: how render reimplementation
interacts with the program freeze, and what "visual parity" means as a testable gate
when there is no clean vector equivalent like ENG-1's env grid.

## Decision

**1. The original fixed-function look is the target — no PBR reinterpretation.**
Rendering ports reproduce the witnessed fixed-function pipeline (TSS combiner
semantics, per-vertex lighting, the modulator scales, fog and blend behavior) as
structural translations with `[orig]` citations. Godot's PBR machinery is host
plumbing, not a look: materials that need fixed-function semantics get composed
shaders (the existing generated-GLSL path) or cited `.gdshader` ports (the sky C7
pattern), never `BaseMaterial3D` approximations of a different lighting model.

**2. The D3D device layer is the WITNESS SOURCE, never a port target.** D3D is an
excluded platform primitive (CLAUDE.md): the state-setting call sites
(`RenderState_ApplyToDevice @ 0x681920`, `GfxBlend_ApplyToDevice @ 0x6817d0`, the
`CGfxDevice`/`CD3DDevice` families, the state/texture-format permutation caches
`@ 0x681d00`/`@ 0x6820c0`) are the DECODER KEY for what a material/stage table
means — what gets ported is the semantics decoded AT that boundary (blend factors,
alpha-test refs, cull modes, stage ops, sort keys, lighting scalars), mapped onto
the host renderer's equivalents. Device/driver plumbing — swapchain, caps handling,
device-reset, buffer management, format selection — is out of scope as a port
target, permanently.

**3. REN runs on the PAR model — freeze-exempt as ledger burn-down.** Grills are
research and were always exempt. REN port slices are divergence closures under
[ADR 0022](0022-divergence-burn-down.md)'s model: each grill mints tracked rows
(D-RMAT / D-RORD / D-RLIT, plus rows in the grown terrain/env records), each port
slice closes rows libs-first, and the maintainer extends the PAR per-slice freeze
exemption to REN slices (2026-07-05). The rationale mirrors ADR 0022 §3: render
parity pays down divergence between an already-rendering reimplementation and the
witnessed original — it does not add feature surface. The alternative (gating ports
to Wave 3 like ONED-W2) was considered and rejected: REN contends with no ONED
foundation work, and stalling witnessed closures buys nothing. The freeze row in
[docs/maturity-program.md](../maturity-program.md) reads "ON for non-PAR/non-REN
work" from this decision on. REN absorbs the open env render rows: env #17
transfers from PAR-ENV to REN-5 (the modulator chain is its consumer); env
#27/#29/#30/#33 fold into REN-6.

**4. The parity instrument is three-tiered, and its tolerances never widen.**
Because materials/lighting have no single clean vector equivalent, REN-1 lands a
dedicated instrument BEFORE any behavior change:

- **T1 — render-state vectors (the CI gate).** A ctest walks the full material
  input matrix (shader tags × 3DI flag bytes × emissive/glass × alpha refs) through
  the classification/composition chain — later extended with sort-key/pass
  classification and lighting scalars — against committed golden tables. ENG-1's
  discipline applies verbatim: goldens are dumped once from the current
  implementation (pinned-current), converged to witnessed-expected rows during the
  grills, every re-dump carries its citation in the same commit, a divergence
  triggers a re-grill against the binary, and tolerances are never widened. A
  forced one-bit sensitivity proof accompanies the instrument's landing.
- **T2 — swatch A/B (local, mandatory per slice).** A capture probe (generalizing
  `env_visual_baseline_probe.gd`) renders a deterministic swatch grid per material
  configuration plus asset-gated world composites; baselines are captured before
  the first behavior change and re-captured per slice, attested in the PR. Visual
  bytes are machine-local: baselines live under `.scratch/golden/render/`, never
  committed; CI hard-gating stays on T1.
- **T3 — retail side-by-side (the headline gate).** A named scene list (water
  horizon across the TOD grid, alpha-test foliage, glass/env-map, transparents
  composite, night lightmap terrain, first-person viewmodel) compared against the
  retail client at close-out, attested scene-by-scene.

Where the host cannot express a witnessed state exactly (TSS combiner corner cases,
reverse-Z — env #20 is the precedent), the divergence lands as a tracked class-C
row and, if ratified, in ADR 0022's permanent register — never as a silent
tolerance bump.

**5. `libs/renderer` grows into the witnessed render library.** LIBS-2's clause
that `libs/renderer` (4 files dodging one oed header) "folds" is REVERSED
(maintainer, 2026-07-05): REN lands its ported semantics — the material→state
tables, sort-key/pass ordering rules, lighting scalar math — in `libs/renderer`,
Godot-agnostic, C++-static-link only (Model B, like `libs/env`; no C ABI additions
planned). REN-2 also subsumes ENG-4's `MATERIAL_FLAG_*` single-sourcing leg for the
engine-side flag spaces (`nova_object_model.gd` ↔ `threedi` ↔ `oed`/`renderer`);
`OED_UPDATE_*` and the Python/DCC mirrors stay with ENG-4.

## Consequences

- **The unwitnessed-reimpl gap becomes tracked work.** The ledger's audit track
  reopens with three render systems (materials/state, draw order, lighting); the
  REN grills convert them to records with catalogs, raising open counts before the
  burn-down lowers them — expected and sanctioned (ADR 0022's audit precedent).
- **Freeze semantics stay coherent.** One sentence governs both exemptions: parity
  burn-down (PAR, REN) proceeds; new-feature reimplementation waits for ONED-W2.
- **The instrument outlives the track.** T1 vectors become permanent regression
  tests (like ENG-1's), and T2/T3 become the standing recipe for any future
  visual-parity claim.
- **Draw-order semantics get a portable home.** The ordering port is data + rules
  in `libs/renderer` applied as Godot priorities/passes in the engine layer — the
  queue itself (a device-era artifact) is not reproduced.
- **Misnomer risk is priced in.** ENG-2 hit ~14 IDB misnomers and 5 dead render
  variants in adjacent regions; REN verifies every IDB name against decompiled
  behavior before citing it, and each record carries its IDB-edits table.
