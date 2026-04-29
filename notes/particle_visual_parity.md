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
- **Atlas packing**
  (`CParticleManager_BuildTextureAtlases @ 0x5e8db0`):
  `opennova::particle::bake_atlas_layout` packs all present graphic
  layers' source textures into one combined `Image` per emitter using a
  horizontal shelf pack (layers laid left-to-right, atlas height = max
  layer height). Each layer's `baked_uv_rects` are updated to atlas
  coordinates so the renderer reads atlas-relative UVs without any
  further plumbing. `NovaParticleEmitter::_rebuild_atlas_texture` builds
  a Godot `ImageTexture` via `Image::blit_rect` and binds it to all 4
  layer materials' `albedo_tex`. Cache-keyed on the per-layer
  (width, height, present) signature so identical inputs skip rebuild
  work. `inset` (bleed-prevention padding) is not yet applied — see
  Bounded Deviations.
- Finite preview emitters no longer auto-repeat after all spawned particles
  expire; `FOREVEREMIT` keeps the emitter eligible for continuous spawning.
- Loose texture lookup remains routed through the shared texture path resolver;
  PFF lookup is intentionally out of scope.

## Bounded Deviations

- `CParticleManager_RenderBatch @ 0x5e9890`: Godot submits one mesh batch per
  graphic layer rather than reproducing the exact Direct3D batch state.
- `mod2x` (BlendMode 5) approximates engine SrcBlend = DESTCOLOR + DestBlend
  = SRCCOLOR by pre-multiplying the source RGB by 2.0 in fragment and using
  Godot's `blend_mul` render mode. The "darken-or-brighten around 0.5"
  midpoint of the engine's modulate2x is preserved; the gamma curve is not
  byte-exact.
- `bump`, `bumpadd`, `distort`: **lit-color implemented; combiner is
  fixed-function modulate (RE 2026-04-28)**. RE confirmed the engine's
  vertex format (FVF 450 = `D3DFVF_XYZ | DIFFUSE | SPECULAR | TEX1`)
  and the per-blend-mode state-binding chain (`PlaySample → sub_683190
  → GfxBlend_ApplyToDevice + RenderState_ApplyToDevice + sub_680760`
  for blend / texture-stage / texture binding respectively).
  **Confirmed via immediate search**: `D3DTOP_BUMPENVMAP`/`_LUMINANCE`
  and `D3DRS_SPECULARENABLE` are never set on the particle render
  path — so bump / bumpadd use plain `D3DTOP_MODULATE` combiners with
  DIFFUSE carrying the encoded `lit_color`. Our shaders
  (`particle_blend_bump.gdshader` does `texture × COLOR × lit_color`;
  `particle_blend_bumpadd.gdshader` does `texture + lit_color` with
  framebuffer-additive) match this combiner topology. **Remaining
  bounded deviation**: per-particle rotation transform on the lit-color
  direction. The engine multiplies the light direction
  `(-1/√3, -1/√3, +1/√3)` by the particle's inverse rotation matrix
  (D3DXMatrixTranspose @ 0x68bf4a) before encoding to `(value+1)*0.5`,
  giving direction-dependent shading. We skip the rotation, so
  `lit_color` is a uniform `bump_scale`-derived tint. Closing this gap
  needs a per-vertex CUSTOM1 with the rotation angle plus shader-side
  Rodrigues rotation. `distort` still falls back to a fixed-strength
  screen-tex UV offset; the engine's exact stage-1 combiner is in
  the index-8 static struct at `~0x7e7858` (sub-struct ptr `0x7e94ec`)
  but the byte layout is undecoded. Adjacent kong-rename corrections
  and full call-chain witness in `notes/ida_particle_witness.md`.
- `CParticleManager_BuildTextureAtlases @ 0x5e8db0` exact pack layout:
  the engine's algorithm (shelf vs row vs binary tree) was not decoded.
  Our portable form uses a horizontal shelf packer (layers laid
  left-to-right, atlas height = max layer height); the resulting UV
  rect data shape matches the engine. `inset` defaults to 0 (texel
  padding for bleed prevention isn't applied yet — would set to
  `0.5 / atlas_dim` once we observe artifacts in dense scenes).

## Open RE Work

- Confirm `CParticleManager_BuildTextureAtlases @ 0x5e8db0` exact pack
  algorithm (shelf vs row vs binary tree). Our portable form uses a
  horizontal shelf packer; the resulting UV rect data shape matches.
- Per-particle rotation matrix for `lit_color` direction: the engine
  applies `D3DXMatrixTranspose(particle_rotation)` to the light vector
  before encoding into DIFFUSE. Our portable form skips this so the
  lit-color is a uniform tint. Closing the gap needs a per-vertex
  CUSTOM1 (rotation angle) + shader-side Rodrigues rotation. Visual
  impact only on bump/bumpadd particles; rare in corpus.
- Decode `distort` mode's stage-1 screen-texture sampler setup
  (specifically what UV offset / scale the engine applies relative to
  the alpha-tex sample). The relevant data lives in the index-8 struct
  at ~`0x7e7858` (sub-struct ptr `0x7e94ec`) but byte layout is
  undecoded.
- `WANDER` (move & 0x08) and `BUBBLE` (move & 0x10) are engine-vestigial in
  JO retail: zero code xrefs to their flag table rows at 0x848A10 / 0x848B18,
  and the corpus never authors them (only NORMAL / GRAVITATE / NORMAL+ORBIT
  appear in the 5 reference fixtures). Preserved for round-trip parsing;
  rendered as NORMAL.
