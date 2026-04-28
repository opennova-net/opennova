# Open questions — particle system

Items that this RE / parser pass touched but did not fully close. Tagged by
how blocking they are for follow-up work.

## Parser-adjacent (resolve before/with parser merge)

1. **`emit_dur` appears twice in many `[particledef]` blocks.** Observed in `30MM.ptl:36`+`:43`, `buildup.ptl:16`+`:21`, and likely most others. The engine is last-wins; the C++ parser is also last-wins. Verify whether values ever disagree (smoke test the diff during the full-corpus pass) — if they always agree, this is just an editor quirk; if they disagree, the editor may have intent we don't capture. Update 2026-04-27: smoke test runs clean across all 77 files but does not cross-check the duplicate values; future enhancement.
2. **`lod` key.** Present in many `[particledef]` blocks (e.g. `30MM.ptl:34 lod = 0.000;`) but NOT in `CParticleDef_ParseFromConfigMap`'s dispatch switch. Likely consumed by the line-level tokenizer `CParticleDef_ParseProperties @ 0x5ea320` before the config map is built. Captured on the C++ struct as a plain float; confirm by decompiling `0x5ea320` in detail.
3. **`tabledef` row width invariant.** All 7 sample-inspected files use 32 × 8. Smoke test asserts row width = 8 across all 77 files. **Resolved 2026-04-27**: 77/77 clean, zero row-shape warnings. The 32×8 shape holds across the full corpus.
4. **Other section types we missed.** Only 4 strings exist in the binary (`[effectdef]`, `[particledef]`, `[tabledef]`, `[tabledef_edithandles]`). Smoke test will fail loud if any corpus file declares a section we don't handle. **Resolved 2026-04-27**: 77/77 clean, no unknown section headers across the full corpus.

## RE follow-ups (next worktree)

5. **`CParticleDef_ParseProperties @ 0x5ea320` (size 0x2525)** — large; not yet decompiled in detail. Decoding it will tell us:
   - Whether `lod` is consumed there.
   - Exact tokenizer error model (do unmatched braces fault, or get tolerated?)
   - Comment syntax (currently observed: none).
6. **`CParticleDefEntry_ParseBlendMode @ 0x5e29f0` (size 0xc8)** — small; decompile to enumerate the full blend-mode set. We've seen `additive`, `blend`, `distort`. Verify nothing else.
7. **`flags` & `move` enums.** Engine resolves via `FlagTable_ParseFromString` against tables at `0x846A18` (flags) and `0x848800` (move). Read the tables to enumerate the bit names. Stored raw in C++ for now.
8. **Curve LUT semantics.** `[tabledef]` rows are `uint8_t × 8`, 32 rows = 256 B. IDA suggests "playback flags" buffers per `_func` are 72 B — likely a different consumer (per-instance interpolator state, not the LUT itself). Confirm sampling rule (linear by particle age 0..1 → 0..255 row index?) when wiring runtime.

## Runtime / out-of-scope (deferred worktree)

9. **`child_id` semantics.** Field exists at `+132` on the def. Spawning logic lives in `CParticleEmitter_AdvanceFrame @ 0x5e6570` and `CParticleEmitter_SpawnNewParticle @ 0x5f35b0`. Open: does the parent emit the child periodically, on death, on collide, or all three?
10. **Texture name resolution.** `graphic1 = dirtpuf.tga, blend;` — parser stores the name verbatim. Runtime resolution likely calls into PFF lookup → texture atlas (`CParticleManager_BuildTextureAtlases @ 0x5e8db0`). Out of scope.
11. **Effect spawn site enumeration.** `CEffectWorld_SpawnEmitterAtPosition @ 0x5f6df0` is the top-level entry. Find every call site (weapon fire, explosion, vehicle exhaust, water splash, footstep) for the next worktree.
12. **Round-trip parity with `CParticleDef_SaveToFile @ 0x5e4d70`.** Once we have a writer, we can verify our parser by feeding fixtures through `parse → write → diff` against the engine writer's output. Defer until the writer lands.

## Repo-hygiene

13. **`godot/game/assets/terrains/Dvxi5/Dvxi5.trn`** shows as modified at worktree open. `git diff` reveals a whole-file CRLF↔LF flip (line endings normalised). This is a `.gitattributes`-level repo issue and unrelated to particle work — flagged here so it's not mistaken for our churn.
