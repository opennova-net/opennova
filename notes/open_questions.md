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
7. **`flags` & `move` enums. RESOLVED 2026-04-27.** Flags table @ 0x846A18: 26 named bits in 29 declared slots (3 trailing entries are zero-padded). Move table @ 0x848800: 5 entries (`NORMAL`, `GRAVITATE`, `WANDER`, `BUBBLE`, `ORBIT`). Both stored as `uint32_t` bitfields plus the raw string for round-trip; constants live in `particle::particle_flag::*` and `particle::move_flag::*`.

## Engine bugs preserved or worked-around

- **Per-graphic color rewrites.** `CParticleDef_ParseProperties @ 0x5ea320` collapses `g2_color1` → `g2_color2` handler, `g3_color1` + `g3_color2` + `g3_color3` all → `g3_color3` handler, and `g4_color1` + `g4_color2` → `g4_color2` handler. So if an editor writes distinct color1/2/3 to graphics 2/3/4, the engine corrupts them on read. Our parser does the *correct* mapping (g{N}_color{M} → graphics[N-1].color[M]); corpus files generally write all four colors identically per layer so the bug isn't visible at runtime.
- **`bumpadd` blend mode.** Engine's chained `strstr` matches `bump` first, so `graphic{N} = ..., bumpadd;` resolves to BlendMode::Bump (3), not Bumpadd (6). Our parser mirrors the engine.
8. **Curve LUT semantics.** `[tabledef]` rows are `uint8_t × 8`, 32 rows = 256 B. IDA suggests "playback flags" buffers per `_func` are 72 B — likely a different consumer (per-instance interpolator state, not the LUT itself). Confirm sampling rule (linear by particle age 0..1 → 0..255 row index?) when wiring runtime.

## Runtime / out-of-scope (deferred worktree)

9. **`child_id` semantics.** Field exists at `+132` on the def. Spawning logic lives in `CParticleEmitter_AdvanceFrame @ 0x5e6570` and `CParticleEmitter_SpawnNewParticle @ 0x5f35b0`. Open: does the parent emit the child periodically, on death, on collide, or all three?
10. **Texture name resolution.** `graphic1 = dirtpuf.tga, blend;` — parser stores the name verbatim. Runtime resolution likely calls into PFF lookup → texture atlas (`CParticleManager_BuildTextureAtlases @ 0x5e8db0`). Out of scope.
11. **Effect spawn site enumeration.** `CEffectWorld_SpawnEmitterAtPosition @ 0x5f6df0` is the top-level entry. Find every call site (weapon fire, explosion, vehicle exhaust, water splash, footstep) for the next worktree.
12. **Round-trip parity with `CParticleDef_SaveToFile @ 0x5e4d70`.** Once we have a writer, we can verify our parser by feeding fixtures through `parse → write → diff` against the engine writer's output. Defer until the writer lands.

## Repo-hygiene

13. **`godot/game/assets/terrains/Dvxi5/Dvxi5.trn`** shows as modified at worktree open. `git diff` reveals a whole-file CRLF↔LF flip (line endings normalised). This is a `.gitattributes`-level repo issue and unrelated to particle work — flagged here so it's not mistaken for our churn.
