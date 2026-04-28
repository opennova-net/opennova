# Particle Visual Parity Checklist

This note tracks renderer alignment against the RE particle render chain. It is
not a struct-offset witness; exact layout claims still belong in
`notes/ida_particle_witness.md`.

## Implemented

- `CParticleEmitter_BuildBillboardQuads @ 0x5e6d60`: preview/runtime emitter
  builds explicit camera-facing quads with per-particle rotation, depth-sorted
  back-to-front per emitter.
- `CParticleEmitter_RenderStaticBillboards @ 0x5f4e10`: YAWANDPITCH bit (8)
  routes the renderer onto a non-rotating quad path
  (`effective_rotation = 0` regardless of `roll_rot`).
- `CParticleEmitter_ComputeViewDepths @ 0x5e7580`: live particles compute
  camera-space depth before batching and are sorted back-to-front.
- `CParticleDefEntry_ParseBlendMode @ 0x5e29f0`: parsed blend mode values feed
  per-layer `ShaderMaterial` selection from one of 8 dedicated
  `particle_blend_*.gdshader` files (additive / blend / premult / bump /
  mod / mod2x / bumpadd / distort) — see Bounded Deviations for bump /
  bumpadd / distort approximations.
- `CEffectDef_ResolveTblDefReference @ 0x5e9630`: per-graphic 256-byte LUT
  bake from the tabledef's 32 × 8 byte buffer (read row-major). Renderer
  samples via `lut[(int)(t * 256) & 0xFF]` — no interpolation. `reverse` /
  `inverse` modifiers from `CParticleDef_ParseProperties` are baked into the
  LUT so the runtime read path is a single byte lookup.
- `CParticleEmitter_SpawnParticle @ 0x5e7640` per-particle flag bits
  (`Particle::flags`, particle+4): bits 0x01..0x100 reflect per-graphic LUT
  presence + flipbook frame count + blend-mode-derived render path. The
  renderer gates curve modulation on these bits so each particle samples only
  its actually-resolved curves.
- `CParticleEmitter_SpawnParticle @ 0x5e7640` emit-shape geometry: annular
  per-axis sphere `lerp(skip, size, rand)`; cone half-angle around
  `Emitter::forward` (with annular per-axis magnitude); box pick-one-axis
  ±size with OneFrame sign clamp.
- `CParticleEmitter_UpdateParticles @ 0x5e6980` GRAVITATE move-mode: when
  `move & Gravitate` is set, `delta = pos − emitter.pos` is masked by
  `gravity_mask`, normalized via `D3DXVec3Normalize` (verified live from
  the `sub_68B032 → off_85072C` thunk), and applied as a constant-magnitude
  repulsive force `vel += unit_delta * spring * dt`. Direction is AWAY from
  emitter — authors enable attractive behaviour via negative gravity_mask
  components. Spring const approximated with `def.gravity` until the
  manager-set `emitter+0x308` scalar is exposed.
- `bake_particle_def_curves` is now wired into the Godot wrapper's
  `_refresh_emitter` (was previously test-only). Fixes a latent bug where
  simulator spawn flags (per-particle curve presence bits 0x01..0x100)
  silently stayed 0x00 in the editor preview because no caller invoked the
  bake. Now every NovaParticleEmitter pre-bakes the per-graphic LUTs +
  per-frame UV rects from its `tables` array before initialising the
  simulator.
- **Kill-plane** (`def.flags & 0x08000000` / `& 0x10000000` in
  `CParticleEmitter_UpdateParticles @ 0x5e6980`): exposed as
  `Emitter::kill_plane_mode` + `Emitter::kill_plane_y` (mode 0=Disabled,
  1=KillAbove, 2=KillAtOrBelow). `integrate_particle` writes
  `p.age = 0` when the threshold is crossed; the existing `expire_dead`
  pass removes the particle next tick. Bits 27/28 are engine-internal —
  outside the 26-name flag table at 0x846A18 — so neither parser nor
  corpus carries them; manager populates per spawn site (e.g. rain
  effects get mode 2 + threshold = ground y at the spawn point).
- **LOD decimation** (`v65 % v82` in `BuildBillboardQuads @ 0x5e6d60`):
  exposed as `Emitter::lod_divisor` (uint32) and `NovaParticleEmitter::lod_divisor`
  (Int property, range 1..16). Engine computes the divisor each frame from
  a manager-set perf budget at `emitter+8 + 0x3F4`; we expose the divisor
  directly. Renderer skips particles where `(serial % divisor) != 0` when
  divisor > 1 — simulator state untouched (physics divisor-agnostic),
  matching the engine's render-only check.
- **Cross-emitter spatial sort**
  (`CParticleManager_TransformToViewSpace @ 0x5ecc50` +
  `CParticleManager_RecursiveSortAndRender @ 0x5ec980`): each
  `NovaParticleEmitter`'s 4 layer meshes are `set_as_top_level(true)` with
  vertex data in world coordinates, so Godot's transparent renderer
  auto-sorts every mesh instance back-to-front by world-space AABB-center
  view-space depth. This achieves the same rendering order as the engine's
  manager-level recursive divide-and-conquer sort without needing a
  separate coordinator Node3D. The recursive split is a perf optimisation
  for many emitters; Godot's QuickSort is fine for typical scenes.
- `CParticleEmitter_TranslatePosition @ 0x5efe90` per-emitter translation:
  exposed as `opennova::particle::emitter_translate(Emitter&, Vec3)` plus
  `last_translation_delta` and `cumulative_translation` accumulators on the
  `Emitter` struct. `NovaParticleEmitter` calls into it from
  `NOTIFICATION_TRANSFORM_CHANGED` so a parented Node3D's world origin drives
  the delta tracking. The engine's parallel role for these accumulators (AABB
  min/max consumed by the cross-emitter manager sort) is documented but
  unimplemented in our portable simulator.
- **World-space rendering** (engine default): `MeshInstance3D::set_as_top_level(true)`
  on the per-graphic layer meshes makes them ignore the NovaParticleEmitter's
  parent transform; the simulator stores particle positions in world coords
  (seeded from `get_global_transform().origin` in `_refresh_emitter`,
  maintained via `emitter_translate`); the renderer reads camera basis in
  world space directly. Particles are "left behind" when the emitter moves,
  matching the engine default. **PositionRelative flag** (bit 18 = 0x40000)
  opts back into local-space behaviour — `emitter_translate` shifts every
  alive particle by the emitter delta so they travel with the emitter.
- `CParticleEmitter_BuildBillboardQuads @ 0x5e6d60` manager-level RGB tint:
  exposed as `NovaParticleEmitter::color_tint` (Color property, default
  white = neutral). Engine reads emitter+200..+202 as 3 bytes and applies
  `(byte * channel) >> 7` per channel — our portable form stores Vec3 in
  [0..2] (with 1.0 = neutral) and clamps each rendered channel at 1.0.
  Used for screen-flash tints (explosions, screen-fill effects).
- `CParticleEmitter_UpdateAllParticles @ 0x5f3be0` ORBIT modifier: when
  `move & Orbit` (= `0x04`, per the engine bitmask reorder at 0x848800), the
  ballistic / spring step is followed by a Rodrigues rotation of both
  `(pos − emitter.pos)` and velocity around `def.orbital_axis` by
  `def.orbitalspeed * dt` per frame. Returns near-starting offset after a
  full 2π orbit; preserves axis-aligned components (rotation around y leaves
  y unchanged).
- Graphic flipbooks use `flip_frames` and `flip_rate` to select a horizontal
  UV frame from elapsed particle lifetime.
- Finite preview emitters no longer auto-repeat after all spawned particles
  expire; `FOREVEREMIT` keeps the emitter eligible for continuous spawning.
- Loose texture lookup remains routed through the shared texture path resolver;
  PFF lookup is intentionally out of scope.

## Bounded Deviations

- `CParticleManager_RenderBatch @ 0x5e9890`: Godot submits one mesh batch per
  graphic layer rather than reproducing the exact Direct3D batch state.
- `CParticleManager_BuildTextureAtlases @ 0x5e8db0`: **UV rect array implemented**
  via `GraphicLayer::baked_uv_rects` (filled by `bake_graphic_uv_rects` with
  horizontal-strip defaults to match the renderer's prior on-the-fly UV
  math). Renderer reads from the baked array, applying `inset` as symmetric
  texel padding. **Atlas packing itself remains deferred** — current batches
  bind resolved loose textures directly per-graphic, so `inset` defaults to 0
  and the rect range is always (0..1, 0..1) split by frame count. Future
  atlas-bake support can replace the defaults with explicit per-frame rects
  without renderer changes.
- `bump`, `bumpadd`, and `distort` shader variants approximate the engine's
  Direct3D combiner: `bump` falls back to standard alpha blend (no normal-map
  Phong), `bumpadd` to additive blend, `distort` to screen-texture sample
  with a fixed-strength UV offset. Engine combiner state is not yet
  witnessed; the visual deviation is most visible for heat-haze and bump-lit
  particle fixtures.
- `mod2x` (BlendMode 5) approximates engine SrcBlend = DESTCOLOR + DestBlend
  = SRCCOLOR by pre-multiplying the source RGB by 2.0 in fragment and using
  Godot's `blend_mul` render mode. The "darken-or-brighten around 0.5"
  midpoint of the engine's modulate2x is preserved; the gamma curve is not
  byte-exact.

## Open RE Work

- Confirm `CParticleManager_BuildTextureAtlases @ 0x5e8db0` UV-rect array
  layout (engine reads `graphic+724` as a 4-float-per-frame buffer).
- Witness the engine's `bump` / `bumpadd` D3D texture-stage state to replace
  the current shader approximations.
- `WANDER` (move & 0x08) and `BUBBLE` (move & 0x10) are engine-vestigial in
  JO retail: zero code xrefs to their flag table rows at 0x848A10 / 0x848B18,
  and the corpus never authors them (only NORMAL / GRAVITATE / NORMAL+ORBIT
  appear in the 5 reference fixtures). Preserved for round-trip parsing;
  rendered as NORMAL.
