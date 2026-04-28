# Open questions — particle system

Items that this RE / parser pass touched but did not fully close. Tagged by
how blocking they are for follow-up work.

## Parser-adjacent (resolve before/with parser merge)

1. **`emit_dur` appears twice in many `[particledef]` blocks.** Observed in `30MM.ptl:36`+`:43`, `buildup.ptl:16`+`:21`, and likely most others. The engine is last-wins; the C++ parser is also last-wins. Verify whether values ever disagree (smoke test the diff during the full-corpus pass) — if they always agree, this is just an editor quirk; if they disagree, the editor may have intent we don't capture. Update 2026-04-27: smoke test runs clean across all 77 files but does not cross-check the duplicate values; future enhancement.
2. **`lod` key. RESOLVED 2026-04-27.** Decompiled `CParticleDef_ParseProperties @ 0x5ea320`: `lod` is **NOT** in either parser's dispatch (neither ParseProperties nor ParseFromConfigMap). The engine ignores `lod` on parse but `CParticleDef_SaveToFile @ 0x5e4d70` writes it from struct offset +38, which never gets populated by parsing — so the engine always emits `lod = 0.000;`. Our parser keeps `lod` as a stored field for round-trip fidelity, but the engine itself never reads it.
3. **`tabledef` row width invariant.** All 7 sample-inspected files use 32 × 8. Smoke test asserts row width = 8 across all 77 files. **Resolved 2026-04-27**: 77/77 clean, zero row-shape warnings. The 32×8 shape holds across the full corpus.
4. **Other section types we missed.** Only 4 strings exist in the binary (`[effectdef]`, `[particledef]`, `[tabledef]`, `[tabledef_edithandles]`). Smoke test will fail loud if any corpus file declares a section we don't handle. **Resolved 2026-04-27**: 77/77 clean, no unknown section headers across the full corpus.

## RE follow-ups (next worktree)

5. **`CParticleDef_ParseProperties @ 0x5ea320` (size 0x2525). RESOLVED 2026-04-27.** Full decomp landed. Confirmed: no comment syntax; tokenizer is loose (silently ignores unknown keys via the unmatched-stricmp fall-through); `[tabledef_edithandles]` @ 0x5ea346 is detected via `strstr` and returns 1 to skip the section. Documented engine bugs around per-graphic colors below.
6. **`CParticleDefEntry_ParseBlendMode @ 0x5e29f0`. RESOLVED 2026-04-27.** Full enum: `additive`=1, `blend`=0, `premult`=2, `bump`=3, `mod`=4 (string at `off_7DCBA8`), `mod2x`=5, `bumpadd`=6, `distort`=7. **Engine quirk:** chained strstr matches `bump` before `bumpadd`, so the latter resolves to 3 in practice. Our parser mirrors this.
7. **`flags` & `move` enums. RESOLVED 2026-04-27, REVISED 2026-04-28.** Flags table @ 0x846A18: 26 named bits in 29 declared slots (3 trailing entries are zero-padded). Move table @ 0x848800: 5 entries (`NORMAL`, `GRAVITATE`, `WANDER`, `BUBBLE`, `ORBIT`).
   - **Engine quirk: bitmask reorder.** Verified live from raw bytes — the move table's `bitmask` fields are NOT sequential. Memory order is `[NORMAL, GRAVITATE, WANDER, BUBBLE, ORBIT]` but bitmasks are `[1, 2, 8, 16, 4]`. So `def.move & 4` ⇒ `ORBIT` (not WANDER). `move_flag::*` constants in `libs/particle/include/particle/particle.h` reflect this: `Orbit = 0x04`, `Wander = 0x08`, `Bubble = 0x10`. The renderer code path `(v15 & 4)` in `UpdateAllParticles @ 0x5f3be0` triggers axis rotation around `def.orbital_axis` — orbital motion, confirming ORBIT semantics.
   - Both stored as `uint32_t` bitfields plus the raw string for round-trip.

## Particle editor MVP follow-ups (Phase 3+, same worktree)

- **Bump / bumpadd / distort shader fidelity (2026-04-28).** The 8-mode shader
  split landed (`particle_blend_*.gdshader`); however `bump` falls back to
  alpha blend (no normal-map Phong), `bumpadd` to additive, and `distort` to
  a fixed-strength screen-tex offset. Engine combiner state for these three
  modes is not yet witnessed. Tracked under Bounded Deviations in
  `particle_visual_parity.md`.
- **`mod2x` gamma curve.** Approximated via `blend_mul + ALBEDO *= 2.0`;
  engine state SrcBlend = DESTCOLOR / DestBlend = SRCCOLOR is not byte-exact
  reproducible without a custom render pass. Visual deviation is mild for
  typical fixtures.
- **Texture loading from PFF.** Per-graphic `texture` field is parsed but the
  shader uses a generated soft-circle fallback when the loose texture file is
  missing. Needs `libs/pff` resolution pipeline so `dirtpuf.tga` etc. render
  correctly under the JO retail asset layout.
- **Curve animation sampling — runtime LUT (closed 2026-04-28).** Per-graphic
  256-byte LUTs are now baked from the 32 × 8 tabledef rows row-major and
  sampled via `lut[(int)(t*256) & 0xFF]`. `NovaParticleTable::sample(t)` keeps
  its linear-interpolation behavior for editor curve visualisation.
- **Child particles.** `child_id` field is present on the def; spawning
  child emitters on death (or per ONMYDEATH flag) is unimplemented in the
  Godot wrapper.
- **Effect-level driver.** Selecting an effect previews one Emitter per pdef
  the effect references — closed 2026-04-27 (`particle_preview.gd::set_effect`
  iterates `effect.pdefs` and instantiates one `NovaParticleEmitter` per).
- **Multi-graphic preview.** The Node3D wrapper renders all 4 graphic layers
  per particle simultaneously via 4 `MeshInstance3D`s with per-layer
  `ShaderMaterial`s — closed 2026-04-28 alongside the 8-mode shader split.

## Engine bugs preserved or worked-around

- **Per-graphic color rewrites.** `CParticleDef_ParseProperties @ 0x5ea320` collapses `g2_color1` → `g2_color2` handler, `g3_color1` + `g3_color2` + `g3_color3` all → `g3_color3` handler, and `g4_color1` + `g4_color2` → `g4_color2` handler. So if an editor writes distinct color1/2/3 to graphics 2/3/4, the engine corrupts them on read. Our parser does the *correct* mapping (g{N}_color{M} → graphics[N-1].color[M]); corpus files generally write all four colors identically per layer so the bug isn't visible at runtime.
- **`bumpadd` blend mode.** Engine's chained `strstr` matches `bump` first, so `graphic{N} = ..., bumpadd;` resolves to BlendMode::Bump (3), not Bumpadd (6). Our parser mirrors the engine.
8. **Curve LUT semantics.** `[tabledef]` rows are `uint8_t × 8`, 32 rows = 256 B. IDA suggests "playback flags" buffers per `_func` are 72 B — likely a different consumer (per-instance interpolator state, not the LUT itself). Confirm sampling rule (linear by particle age 0..1 → 0..255 row index?) when wiring runtime.

## Runtime / out-of-scope (deferred worktree)

8b. **Move-mode integrators (2026-04-28).** Status:
   - `NORMAL` (move & 1): ✅ ported. Ballistic pos/vel + drag.
   - `GRAVITATE` (move & 2): ✅ revised — engine path B verified to
     normalize delta (`sub_68B032 → off_85072C` is `D3DXVec3Normalize`)
     and direction is AWAY from emitter origin (constant-magnitude
     repulsion; authors flip via negative `gravity_mask`).
   - `ORBIT` (move & 4): ✅ ported. Rodrigues rotation of
     `(pos − emitter.pos)` and velocity around `def.orbital_axis` by
     `def.orbitalspeed * dt` per frame.
   - `WANDER` (move & 8): **engine-vestigial.** `mcp__ida-pro-mcp__find` for
     "WANDER" string returns only `0x848A18` (the table entry name);
     `xrefs_to 0x848A10` (the table row) returns 0 code refs. The
     `FlagTable_ParseFromString @ 0x5df970` iterator reads it sequentially
     so the bit is parseable, but no other code path tests the bit. Across
     the corpus (`grep -ah "move" fixtures/particle/*.ptl | sort -u`) only
     NORMAL / GRAVITATE / NORMAL+ORBIT appear — WANDER is never authored.
     **Action:** preserve for round-trip parsing only; render as NORMAL.
   - `BUBBLE` (move & 0x10): **engine-vestigial.** Same status as WANDER —
     `find "BUBBLE"` returns only `0x848B20` (the name in the table) and
     `xrefs_to 0x848B18` returns 0. Corpus never authors it.
8c. **Spring const for GRAVITATE — RESOLVED 2026-04-28.** Engine reads
   `*((float*)emitter+77)` = emitter+0x308 as the spring strength, set by
   `CEffectEmitter_Initialize @ 0x5e6020` at spawn time. Our portable
   simulator now exposes `Emitter::spring_const` (default 0 = fall back to
   `def.gravity` so stand-alone callers without a manager still work).
   Godot wrapper exposes it as a `spring_const` Float property. Pinned by
   `test_gravitate_spring_const_overrides_def_gravity`.
8d. **Kill-plane — RESOLVED 2026-04-28.** Engine `UpdateParticles @
   0x5e6980` reads `*((_DWORD*)this+83)` as a `float*` to a height
   threshold; `def.flags & 0x8000000` (bit 27) kills if `particle.y >
   threshold`; `& 0x10000000` (bit 28) kills if `particle.y <= threshold`
   (engine writes `particle.age = 0`, no position clamp). Bits 27/28 are
   engine-internal — outside the 26-name flag table at 0x846A18 — so
   neither parser nor corpus carries them; manager populates per spawn
   site. Ported as runtime emitter scalars `Emitter::kill_plane_mode` +
   `Emitter::kill_plane_y` (mode 0=Disabled, 1=KillAbove, 2=KillAtOrBelow)
   instead of the def.flags bits. Pinned by 3 ctest cases + 1 GUT case.
   Tying the kill plane to terrain in the editor preview (driving
   `kill_plane_y` from a heightmap query at the emitter's xz position) is
   a follow-up — the API surface is ready.
8e. **Per-particle bit 0x40 (UseParentColor) (2026-04-28).** Engine spawn
   reads `def.flags & 0x40` (`UseParentColor`) and applies a parent-particle
   color override path that mutates particle+8 (packed_color). Distinct
   from the 0x01..0x100 LUT-presence bits. Not ported — particles always
   sample their own color1..4 slot.

9. **`child_id` + `ONMYDEATH` semantics — RESOLVED 2026-04-28.**
   - **`child_id` is engine-vestigial in JO retail.** Parsed at
     `CParticleDef_ParseFromConfigMap @ 0x5ed210` via
     `*(_DWORD *)(emitterDef + 132) = String;` (single dword storing the
     string ptr returned by `CConfigReader_GetString`). Serialised by
     `CParticleDef_SaveToFile @ 0x5e4d70` for round-trip preservation. No
     runtime consumer: `mcp__ida-pro-mcp__find "child_id"` returns only the
     two parser/writer key-string addresses (0x7dd354 + 0x7dd9b4); their
     xrefs point exclusively to `ParseProperties @ 0x5ea320`,
     `ParseFromConfigMap @ 0x5ed210`, and `SaveToFile @ 0x5e4d70`. JO retail
     evidently stripped the spawn-on-death dispatch but kept the .ptl
     parser/writer for asset round-trip.
   - **`CEffectEmitter_SpawnBetweenPositions @ 0x5ea0a0` confirmed** to walk
     a linked list of sub-emitters at `parent_emitter + 52`, calling
     `vtable[+4]` (spawn callback) on each inactive child. This is the
     **`[effectdef].pdefs` sub-effect mechanism** (already supported in our
     editor preview via `particle_preview.gd::set_effect`), NOT a
     `child_id`-by-name resolver.
   - **`ONMYDEATH` (particle_flag bit 4 = 0x10) IS active** in
     `UpdateAllParticles @ 0x5f3be0`. The bit triggers a write to a per-
     particle marker at `secondaryBuffer + idx * stride` when the emitter
     has its `_ESI[60]` (emitter+240, `secondaryBuffer`) set. This is
     parent-particle coordination machinery (not child_id resolution); the
     `secondaryBuffer` is wired by a higher-level emitter-pool allocator
     (`sub_5E46B0`, called from `sub_5EA200`).
   - **Action:** no further child_id work needed — our parser stores it,
     our writer round-trips it, and the engine itself doesn't consume it
     at runtime.

10. **Texture name resolution.** `graphic1 = dirtpuf.tga, blend;` — parser stores the name verbatim. Runtime resolution likely calls into PFF lookup → texture atlas (`CParticleManager_BuildTextureAtlases @ 0x5e8db0`). Out of scope.
10. **Texture name resolution.** `graphic1 = dirtpuf.tga, blend;` — parser stores the name verbatim. Runtime resolution likely calls into PFF lookup → texture atlas (`CParticleManager_BuildTextureAtlases @ 0x5e8db0`). Out of scope.
11. **Effect spawn site enumeration.** `CEffectWorld_SpawnEmitterAtPosition @ 0x5f6df0` is the top-level entry — verified 2026-04-28 to be a wrapper that calls `sub_5EA200(world, def, pos, dir, ...)` with a struct buffer (def-by-name lookup or pre-resolved index, position, optional direction, layer-visibility flag, sound channel params). Caller maps to weapon fire, explosion, vehicle exhaust, water splash, footstep — full call-site enumeration deferred to a later worktree.
12. **Round-trip parity with `CParticleDef_SaveToFile @ 0x5e4d70`.** Once we have a writer, we can verify our parser by feeding fixtures through `parse → write → diff` against the engine writer's output. Defer until the writer lands.

## World-space vs local-space rendering — RESOLVED 2026-04-28

The renderer now defaults to engine-faithful world-space rendering:
- Simulator runs in world space (seeded from
  `get_global_transform().origin` in `_refresh_emitter`)
- `NOTIFICATION_TRANSFORM_CHANGED` keeps the simulator origin in sync with
  the Node3D's world origin via `emitter_translate`
- `MeshInstance3D::set_as_top_level(true)` on the per-graphic layer meshes
  makes them ignore parent transform; vertices interpreted as world coords
- Camera basis read in world space directly
- `PositionRelative` flag (bit 18 = 0x40000) opts back into local-space:
  `emitter_translate` shifts every alive particle by the emitter delta

Engine corpus survey confirmed PositionRelative is never authored across
the 5 reference fixtures, so the world-space default matches typical
authoring intent. Pinned by 3 GUT tests
(`test_emitter_position_change_translates_simulator`,
`test_world_space_default_keeps_particles_when_emitter_moves`,
`test_position_relative_carries_particles_with_emitter`) + 6 ctest cases
in `particle_translate_test.cpp`.

## Repo-hygiene

13. **`godot/game/assets/terrains/Dvxi5/Dvxi5.trn`** shows as modified at worktree open. `git diff` reveals a whole-file CRLF↔LF flip (line endings normalised). This is a `.gitattributes`-level repo issue and unrelated to particle work — flagged here so it's not mistaken for our churn.
