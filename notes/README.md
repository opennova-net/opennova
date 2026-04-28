# particles worktree — notes

Reverse engineering and documentation for the NovaLogic Joint Operations
particle system (`.ptl` files) in the opennova-godot port.

**Truth source**: retail `Jointops.exe` IDB at
`C:\Users\taylor\Development\opennova-godot\RE\Jointops.exe.i64`.

**Worktree scope (this round)**: text-format parser only. C++ static lib
(`libs/particle`) + tests against curated corpus fixtures. Godot runtime,
emitter simulator, texture/material resolution and effect spawn API are
deferred to follow-up worktrees.

## Index

| File | Purpose |
| --- | --- |
| [`ida_particle_witness.md`](ida_particle_witness.md) | RE matrix: every parser/runtime function and struct, with IDA addresses and verdict column |
| [`ptl_format.md`](ptl_format.md) | Narrative spec of the `.ptl` text grammar |
| [`ptl_corpus_catalog.md`](ptl_corpus_catalog.md) | Auto-generated table of every `.ptl` file in `~/Desktop/JO_ASSETS_t/` |
| [`open_questions.md`](open_questions.md) | Things still unverified after this RE pass |
| [`scripts/catalog_corpus.py`](scripts/catalog_corpus.py) | Regenerates `ptl_corpus_catalog.md` against the corpus |

## Key findings

1. `.ptl` files are **plain text**, INI-like with `{}` blocks. No binary loader.
2. Four section types in the corpus — all dispatched by a single function:
   `CEffectWorld_ParseSectionCallback @ 0x5ecb40`.
3. Per-section parsers and the field-by-field hydrator
   (`CParticleDef_ParseFromConfigMap @ 0x5ed210`, ~80 keys, ~5204 byte struct)
   are all named in IDB and match the corpus content.
4. The engine ships **writer functions** (`CParticleDef_SaveToFile @ 0x5e4d70`
   et al.) — opens the door to round-trip parity testing in a later round.
5. **Smoke result (2026-04-27)**: parser passes **77 / 77** `.ptl` files in
   `~/Desktop/JO_ASSETS_t/` clean. Zero parse errors, zero table-row-shape
   warnings. Confirms the section/key dispatch is exhaustive across the full
   shipped asset set.

## Running tests

The 5 focused tests run by default:

```bash
cmake -S . -B build
cmake --build build --config Release --target opennova_particle particle_smallest_test particle_minimal_effect_test particle_multi_section_test particle_multi_layer_test particle_table_handles_test particle_smoke_all_fixtures_test
ctest --test-dir build -C Release -R '^particle_' --output-on-failure
```

The smoke test (`particle_smoke_all_fixtures`) is **disabled by default** in
CTest because the 5 hand-curated fixtures committed under `fixtures/particle/`
are a subset of the 77-file corpus the engine ships. To re-run smoke against
the full corpus locally:

```bash
cp ~/Desktop/JO_ASSETS_t/*.ptl fixtures/particle/
build/tests/Release/particle_smoke_all_fixtures_test.exe
```

Don't commit the extra 72 files — they are NovaLogic IP and only need to be
present locally for the smoke regression run.
