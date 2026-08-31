# ADR 0043: Modern presentation, witnessed intent

- **Status**: accepted (2026-08-31; maintainer directive — the Godot-native rendering
  exploration)
- **Owners**: rendering, runtime architecture
- **Supersedes/updates**: supersedes [ADR 0023](0023-render-visual-parity.md)
  decision 1 **for presentation technique** and re-scopes its decisions 2 and 4 as
  described below; ADR 0023's diagnosis, witness maps, and evidence tiers remain the
  historical record. [ADR 0042](0042-godot-permanent-shell-one-mission-kernel.md)
  (one backend, the boundary rule) and [ADR 0022](0022-divergence-burn-down.md)
  (the ledger model) stand unchanged and this ADR leans on both. ADR 0003 (writers
  from scratch) and every wire/format/simulation parity rule are untouched.

## Context

The renderer OpenNova ported is a faithful reproduction of 2004 techniques:
silhouette-drape entity shadows rendered into twelve capture targets and projected
onto terrain, CPU-rasterized static sun shadows baked into the terrain tile-cache
alpha page, 155 hand-written `unshaded` fixed-function shaders carrying their own
per-vertex lighting, a three-lights-per-strip selection cap with instance-uniform
delivery, hand-written fog in every shader family, and a RenderingDevice particle
compositor reproducing retail's cross-emitter sort. Each of these exists because the
original hardware demanded it, not because the game's authors wanted that mechanism —
they wanted the *result*: a sun and sky of particular colors, buildings and people
grounded by shadows, fires that glow, smoke that drifts.

The maintainer's 2026-08-31 directive closes the question ADR 0023 left open: the
techniques are twenty years old, and the project should switch to their modern
canonical Godot equivalents **without losing the original intent behind the
visuals** — accepting visual change where it is a modern improvement. The evidence
base for the decision was a full breadth study of the three domains (particles,
shadows, lighting/environment) plus a blend-mode census of the retail `.ptl` corpus
(2,332 graphic declarations: `mod`/`mod2x`/`bumpadd` have zero authored uses — the
hardest-to-host pipelines defend content that does not exist).

## Decision

1. **The intent contract.** The engine remains the witnessed authority for **what**
   the scene contains: the `.env` TOD palette and its integer interpolation, fog
   colors and distances, `.ptl` effect definitions and their motion semantics, LGHT
   placements/colors/radii and the flicker law, WAC environment commands, the
   modulator/iris and NVG state machines, and the `.def` caster-admission flags.
   That decode stays structurally ported and cited (`[orig:]`) exactly as before.
   Godot is the sole authority for **how** it renders: one lit Forward+ scene with
   real `DirectionalLight3D`/`OmniLight3D` nodes, cascaded shadow maps, Environment
   fog and glow, and scene-graph particles. "Visual parity" is superseded by
   **intent parity**: the authored data must drive the scene and remain
   recognizable; the rendering technique is canonical Godot and no longer
   constrained by retail's device semantics.

2. **The replaced subsystems.** Under this contract the following ported techniques
   are **deleted and replaced**, hard, with no dual path (ADR 0042's one-backend
   doctrine): the render-slot silhouette/drape shadow system and the static
   CPU shadow rasterizer (both replaced by one CSM `DirectionalLight3D`); the
   fixed-function surface lighting in the object/terrain shaders, the
   three-per-strip light selection, the terrain projected light pass, and the
   instance-uniform light delivery (replaced by the lit pipeline, a gradient-sky
   hemisphere ambient, and clustered `OmniLight3D` nodes); the per-shader hand fog
   (replaced by Environment depth fog, with the additive fade-to-black island);
   the sun-visibility raycast system (replaced by per-pixel shadow reception);
   the environment cube capture (replaced by a `ReflectionProbe`); the particle
   frame compiler, skyline atlas, and RenderingDevice compositor (replaced by
   MultiMesh presentation of the retained CPU `.ptl` simulation); and the FrameFX
   bloom bracket — the focused Q3 re-render, its typed compiler/registry/cache,
   and the capture/blur/composite kernel (replaced by `Environment` glow; since
   the linear-scene amendment below, no terminal compositor remains). Settings
   toggles are reserved for one-property taste knobs (shadow distance, foliage
   casters, glow, volumetric fog, soft-particle fade); a toggle never selects
   between two implementations.

3. **What does not change.** Everything a headless ctest reproduces without a
   renderer keeps full parity: simulation, networking (wire bytes), formats
   (read/write), the mission/WAC/AI runtimes, the `.ptl` emitter simulation, the
   TOD/modulator/weather math, and the intent-layer decode above. The authored
   *content* presentation islands stay: the sky dome and celestial `.3di` ladder,
   the water planar mirror, weather/precipitation, coronas, MatchTerrain, NVG
   post, underwater murk, the terrain splat chain.

4. **ADR 0023 re-scoped.** Decision 1 (the fixed-function look as target) is
   superseded for presentation by decision 1 above. Decision 2 survives re-scoped:
   the D3D device layer remains the **witness source for decoding intent** — what a
   color, radius, curve, or flag meant — never a port target. Decision 4's
   instrument survives re-purposed: T1 state vectors keep pinning the *kept* math
   (the world-lighting block builder, TOD interpolation, the modulator chain,
   material classification against the collapsed technique set); vector families
   that pinned deleted technique retire with it, in the same commit, and
   tolerances on surviving vectors still never widen. T2 swatches and T3 scene
   comparisons re-baseline: the reference becomes attested **pre-change OpenNova
   captures** (the acceptance scenes), compared for intent recognizability, not
   retail byte-parity.

5. **Ledger treatment.** Divergence rows whose subject system is deleted by
   decision 2 close as `PERMANENT` citing this ADR, in a "Modern presentation
   (ADR 0043)" section of the permanent register — one row per replaced subsystem,
   not per historical defect. Rows that were open defects *of* a deleted system
   (the 03TR low-sun drape, D-RLIT-10, D-RORD-7, D-TERRAIN-7's tile-shadow half)
   close as moot with it. Each closure rides the commit that deletes its system,
   never ahead of it. The RE records stay untouched as knowledge; each affected
   record gains a header note that the witnessed presentation technique is retired
   in OpenNova by this ADR.

## Amendment (2026-08-31): the linear scene

The gamma-domain scene contract — spatial shaders writing retail gamma-byte
values into the float scene target, framebuffer blending in gamma space, and one
terminal `DisplayDecodeEffect` compositor per 3D view performing the sole display
decode — was the last retail *device semantic* constraining the Godot scene. It
is retired under decision 1 (Godot is the sole authority for **how** it renders):

- The scene holds linear light. Color textures decode through `source_color`;
  every witnessed palette byte (Light3D colors, ambient, material color
  uniforms, vertex color streams) is set raw or converted exactly once at the
  Godot seam — the engine keeps the witnessed bytes; nothing pre-encodes against
  a downstream decode. Framebuffer blending, tonemap (LINEAR) and the output
  transfer are stock Godot.
- The terminal compositor, its water-mirror decode copy, the Light3D/ambient
  pre-encodes, and the calibrate-mode byte proofs are deleted (register row
  MP-7). `color.gdshaderinc` keeps only the two byte-domain helpers for passes
  that still evaluate witnessed math on gamma bytes (the NVG canvas post, the
  water strip's byte-encoded vertex colors).
- Witnessed shader arithmetic (the terrain splat chain, MODULATE2X folds, the
  water ps.1.1 chain, the sky TSS cloud combine) keeps its text and constants,
  now over linear inputs. The appearance shift on blends and combine chains is
  the accepted divergence of this amendment, judged by the acceptance-scene
  eyeball pass; the sanctioned taste knobs are the glow threshold and, if
  re-judged, the MODULATE2X energy constant — never a re-encode.
- D-RMAT-7/-8 (the gamma pipeline rows) close as superseded by this amendment.

## Consequences

- The presentation layer stops being a parity liability and becomes ordinary Godot
  code: real lights, real shadows, scene-graph particles — reviewable and tunable
  by Godot idiom rather than against a binary.
- Roughly 23–25k lines of ported technique (and ~10k lines of tests pinning it)
  are deleted for ~2k lines of canonical replacement; the `adapter_cpp_orig_cites`
  and shader-provenance instruments shrink accordingly (deletions only — the
  ratchets never rise).
- The game visibly diverges from retail: geometric entity/vehicle shadows, tree
  and terrain self-shadowing, everything receiving shadows, unlimited point
  lights, bloom on additive effects, soft particles, reflected particles in
  water. Retail comparison screenshots stop being the acceptance gate for these
  domains; the attested acceptance-scene set is.
- The one structural regression accepted up front: baked static shadows were
  visible at unlimited distance; CSM ends at its max distance (default 400 u).
  If the acceptance pass rejects the horizon, the designed fallback is a far-field
  terrain-only bake layer — a new decision, not a silent revert.
- Gamma-era assets meet a linear-light pipeline: the energy calibration against
  reference captures (the town-map TOD grid acceptance scene) is the make-or-break
  gate of the migration, and lands before any deletion it justifies.
