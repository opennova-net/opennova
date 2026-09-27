# ADR 0038: Native runtime assets now; GLB at the future editor seam

- **Status**: accepted (2026-08-26; hard cut). **Superseded by
  [ADR 0047](0047-blender-3di-exporter.md) (2026-09-23)**, which restates the
  decisions that stand (its decision 10) and lifts decisions 1, 3 and 5 in part
  for its one authoring route, as marked inline below. This file stays as the
  record of the hard cut: the retired Python, Qt and DCC surfaces, the flat C
  ABI with `opennova_shared`, and the `abi_exports_check.py` gate.
- **Owners**: native formats, future editor
- **Supersedes/updates**: retires the Python importer, Qt application, Blender
  add-ons, ASE/OED/TDP authoring pipeline, and the flat shared-library model in
  ADR 0024. Updates ADRs 0027, 0029, 0034, and 0037 where they describe those
  retired surfaces. Their runtime and layering decisions remain in force.

## Context

The repository carried several overlapping object-authoring routes: a Qt
importer, Python FFI packages, Blender add-ons, and a native ASE/3DP/OED
pipeline. They duplicated scene knowledge, made release and CI depend on a
second language toolchain, and exposed editing state through the Godot runtime
adapter. None of those modules is required to run retail game data.

The editor will eventually use GLB/GLTF as its scene interchange format. That
work is deliberately separate from this cleanup and is not needed by the
current game runtime.

## Decision

1. The repository has no Python product or toolchain code: no Python packages,
   Qt importer, Blender add-on, DCC-specific package, `pyproject.toml`/lockfile,
   or Python test suite. Stdlib-only repository lint and maintainer scripts
   remain (`scripts/lint/`, the CI maturity gates; `scripts/ida/cite_sweep.py`;
   `scripts/net/diff_0a.py`; `tools/net/pcap_to_hexcap.py`; since ADR 0041 also
   `scripts/mcp/game_mcp.py` and `scripts/ci/test_suites.py`) and run on a stock
   `actions/setup-python` interpreter. For product code the cut is literal: no
   compatibility modules or deprecated entry points remain. *(Lifted in part by
   ADR 0047: its Blender add-on, `tools/blender/opennova_3di`, is the one Python
   product module.)*
2. Native ASE, TDP/3DP, and OED authoring modules are removed. Godot
   `ObjectData` loads and evaluates immutable 3DI runtime documents only.
3. ADM and BAD remain runtime input formats. Their readers remain; repository
   writer/export interfaces are removed. Native 3DI reading and writing remain
   format capabilities, but Godot exposes no object-export command yet.
   *(Lifted by ADR 0047 decisions 11 to 13: the `.bad`/`.adm` writers return,
   from scratch, under the `formats/bad/bad_build` construction seam.)*
4. A future editor will accept GLB/GLTF scenes and produce 3DI, and will read
   3DI and produce GLB. Neither direction is implemented by this decision.
5. The future exchange seam is the ordinary scene plus the stable naming
   convention in `docs/threedi/scene-naming-contract.md`. Conversion must not
   depend on another converter, importer-private state, Blender custom
   properties, application metadata, or an import having happened first.
   *(Lifted in part by ADR 0047: its add-on reads visible Blender add-on
   properties; the rest of the rule stands.)*
6. Exact GLTF representation for LODs, collision data, animation events, and
   other Nova-specific fields is future editor design work. This decision does
   not reserve private extras or carry the deleted Blender schema forward.

## Consequences

- The runtime module has one small interface: load 3DI and expose the parsed
  document for rendering, collision, animation, and inspection.
- Build, test, and release no longer need a Python toolchain (uv, pyproject,
  wheels) or package DCC tooling; the CI lint step uses the runner's stock
  interpreter.
- Checked-in native WAC tables and Godot shader sources are authoritative
  sources rather than outputs whose missing Python generators are implied.
- Historical reverse-engineering records may continue to discuss ASE, OED, or
  3DP as provenance. They must not advertise those paths as current products.

- `engine/base/vfs/vfs_capi.{h,cpp}` and `engine/formats/mission/mission_capi.{h,cpp}`
  (the flat C ABI over the VFS and MissionDocument) go with `opennova_shared`; the
  `abi_export_identity` ctest, `scripts/lint/abi_exports_check.py` and its baseline
  retire with them.
- The `.bad`/`.adm` writers (`bad_write.cpp`, `adm_write`) and their byte-exact
  round-trip proofs (`bad_roundtrip`, `adm_write`, `tests/test_bad_write_ffi.py`)
  retire; the read side stays pinned by `bad_parse` and the `adm_*` ctests.
  *(Reversed by ADR 0047: the writers and the `bad_roundtrip` and `adm_write`
  ctests return.)*
- Re-expressed natively: `tests/test_bad_pos_derivation.py` becomes the synthetic
  `anim_sample` pin plus the `OPENNOVA_JO_ASSETS`-gated `anim_positions_from_model_corpus`
  ctest; `anim_skeletal_clips_weapon_channel` returns on a committed twist
  fixture; the shader-resource pytests become `godot/tests/shader_resource_contract_test.gd`
  and `shader_provenance_pins_test.gd` (the retail `.fx` decode legs go with
  `third_party/modsuperoed`). GUT tests that authored their 3DI inputs through the
  removed edit/export bindings load models minted by our own writer instead
  (`fixtures/threedi/synth/`, generated by `tests/fixtures/minimal_3di_gen.cpp`;
  the model and variant tables are in `fixtures/README.md`).
- The `scripts/lint/` gates, `scripts/ida/cite_sweep.py`, `scripts/net/diff_0a.py`
  and `tools/net/pcap_to_hexcap.py` stay (decision 1); the render-parity
  publication tools (`scripts/render/*.py`) and the net parity-matrix harness
  (`export_parity_corpus.py` and its `.ps1` drivers) go, being FFI- and
  Qt-bound.

## Verification

- The tracked tree contains no Python package or product code, ASE, 3DP, or
  Blender scene files; the only `.py` files are the stdlib scripts named in
  decision 1.
- Native and Godot builds succeed with no authoring-format or shared-FFI target.
- Runtime tests exercise ADM/BAD readers and immutable 3DI fixtures.
- Active product and release documentation names only the native runtime and
  the future GLB/GLTF editor seam.
