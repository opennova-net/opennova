---
name: new-format-lib
description: Scaffolds a new NovaLogic format library under engine/formats/ end to end — group-CMake registration, opennova namespace, parser/writer, fixtures with LFS and asset-gating policy, roundtrip ctest, and optional GDExtension binding — or extracts an existing parser out of a runtime lib into its formats/ home. Use when adding support for a new game file format, or when moving a format parser to engine/formats/.
---

# Add (or extract) a format library under engine/formats/

Done dozens of times; the conventions are strict. Copy a living exemplar, don't
invent: `engine/formats/dbf` (small binary format), `engine/formats/lwf` +
`tests/lwf/` (byte-exact roundtrip), `engine/formats/bad` (binary animation),
and `engine/formats/adm` (a text format extracted from runtime).

## 0. The placement gate (ADR 0030)

Before creating anything, apply the placement rule
(docs/adr/0030-formats-placement-criterion.md): ALL file knowledge — the
parsed model, read, and write — lives in `engine/formats/<lib>`, depending
only on `base/io` and other formats libs; what stays in `runtime/` (or
`net/`) is execution and simulation over the parsed model (a VM, an emitter,
promotion — never the parser). There is no "runtime-fused format": if a
format's parse code sits outside formats/, that is a gap to extract with the
recipe in step 6. Partial ports move too, with the coverage gap documented in
the lib header (the aip precedent).

Then decide, before writing code:

- Parse-only or writer? The project norm is a byte-exact parse→write roundtrip.
  If write policy diverges from the original bytes (it must never be raw
  passthrough — see docs/adr/0003-no-raw-passthrough-create-from-scratch.md),
  capture the policy in a new `docs/adr/NNNN-*.md` like ADR 0008 (PFF writer).
- Behavior taken from the original binary gets inline `[orig: Name @ 0xADDR]`
  citations (retail Jointops.exe unless stated; conventions in
  `docs/README.md`). RE findings land via the `re-doc` skill.
- Witness the original loader first with the `engine-research` skill; after the
  port, verify with `grill-ida` — the port is a faithful translation, never our
  own version (CLAUDE.md conventions).

## 1. Library skeleton (post-ADR-0029: no per-lib CMake target)

Files: `engine/formats/<name>/<name>.{h,cpp}` — FLAT since 2026-08-10:
headers and sources sit side by side in the lib dir (nested subdirs allowed).
There is NO per-lib CMakeLists.txt — the lib builds inside the
`opennova_formats` group, and `engine/` is the ONE public include root: every
cross-lib include is group-qualified, `#include <formats/<name>/<name>.h>`
(ADR 0040 decision 3; same-directory siblings may keep a bare `"file.h"`).
Registration is exactly ONE edit in `engine/formats/CMakeLists.txt`:

1. the source block, alphabetical by domain, with a one-line format comment:
   `# <name> — <what the format holds>` + one
   `${CMAKE_CURRENT_SOURCE_DIR}/<name>/<file>.cpp` line per TU.

The group already carries C++17, POSITION_INDEPENDENT_CODE, and a PUBLIC
link to `opennova_io` — add nothing else; no include-list edit exists any
more (the pre-flatten `include/<prefix>/` + `src/` split and the per-lib
PUBLIC include entries are gone).

Include prefix: always `formats/<name>/`. The group segment is what keeps
`formats/mission` and `runtime/mission` apart (likewise `particle`, `wac`) —
the flat per-group include roots that once made the prefix ambiguous are gone
(ADR 0040 Context). Namespace `opennova::<name>` for C++ surfaces.

## 2. Nothing to register twice

Both CMake roots share `engine/CMakeLists.txt`; the group target is already
linked by `godot/src` and the tests. Do not add a second target or a dynamic
FFI wrapper. One thing still matters:

- If the format is terrain-family (or otherwise seam-relevant), check whether
  `scripts/lint/include_graph_check.py` needs the new include prefix in its
  forbidden list — same commit.

## 3. Fixtures (`fixtures/<name>/`)

- Small representative samples ARE committed, and every file under `fixtures/`
  is one of the three classes `fixtures/README.md` defines: MINTED (written by
  a `tests/fixtures/minimal_*_gen.cpp` generator through our own writer and
  byte-compared by that generator's ctest), AUTHORED (text we wrote) or KEEP
  (a small retail-interop file listed with its reason in
  `scripts/lint/fixture_allowlist.json`). Prefer minting; a KEEP entry needs a
  reason. `fixtures/**` is LFS via `.gitattributes` and capped at 2 MiB per
  file — after `git add`, verify with `git lfs status` that the files staged
  as LFS objects, then run `python scripts/lint/fixture_lint.py --report`
  (CI runs it `--enforce --require-pulled`).
- Bulk retail corpora are NEVER committed (copyright). Gate a sweep test on one
  of the two machine roots through `tests/common/retail_paths.h`
  (`retail::install()` / `retail::assets()` / `retail::reference_fixture(rel)`, with `RETAIL_REQUIRE_OR_SKIP` or `retail::skip`):
  absent data returns exit 77, and the test is registered with
  `opennova_add_gated_test` so ctest reports Skipped, never Passed. A test whose
  synthetic legs ran reports its missing retail leg with `retail::skip_leg` and
  exits 0. Never read the environment directly (`scripts/lint/env_lint.py`
  hard-fails it) and never exit 0 on missing data. Register it in
  `tests/CMakeLists.txt` with `opennova_add_gated_test` (label `retail`, exit 77 =
  Skipped) or, for one binary with synthetic legs plus a `--retail` leg,
  `opennova_add_mixed_test` (which adds the labelled `<name>_retail` entry);
  `scripts/ci/test_suites.py` attests the core/retail selection against the
  reports, so there is no expectation table to edit. `docs/asset-gated-tests.md`
  describes the suites.
- Record where each committed sample came from in the test header comment.

## 4. Tests (`tests/<name>/`)

No test framework: plain `main()` with `TEST_EXPECT` from
`tests/common/test_expect.h`; locate the repo via `tests/common/test_paths.h`
(see `tests/dbf/dbf_roundtrip_test.cpp`). Minimum: parse test + byte-exact
roundtrip against the committed fixture. Wire into `tests/CMakeLists.txt`
following the dbf block — one executable per test, link `opennova_formats`,
`add_test(NAME <name>_roundtrip ...)`.

Run loop:

    bash scripts/build.sh --no-godot             # configure + build + full ctest
    ctest --test-dir build -C Release -R <name>  # then iterate on just yours

## 5. Optional: engine binding

- Binding: `godot/src/<name>/<type>.{h,cpp}` — a binding file is named after the
  type it declares, e.g. `godot/src/audio/ambient_mixer.h` wrapping
  `opennova::audio::AmbientMixer` (ADR 0040 decisions 2 and 5; bindings include
  root-relative, `"audio/ambient_mixer.h"`). `GDREGISTER_CLASS` in
  `godot/src/register_types.cpp` (`godot/src` already links the group —
  no CMake link edit), then `bash scripts/build_godot.sh` and fully restart
  any open editor (no hot-reload). Add a GDScript smoke test
  `godot/tests/<name>_data_test.gd` over the committed fixture (a core script
  never reads retail data); a script that needs retail data lives under
  `godot/tests/retail/` instead. Run it via the `gut` skill.
- Format libraries remain independent of authoring workspaces, inspectors,
  project surfaces, and import flows; see ADR 0045.

## 6. Extracting an existing parser out of runtime/ (the ADR 0030 recipe)

When a format already exists inside a runtime lib and passes the step-0 gate:

1. `git mv` the format's headers and TUs into `engine/formats/<name>/`
   (flat). Decide the prefix per step 1's rule — keeping a shared prefix
   with disjoint sets means ZERO consumer include churn; renaming is right
   only when the includer count is trivial.
2. Two source-list edits: add the block in
   `engine/formats/CMakeLists.txt`; remove the lines (and any per-source
   property entries, e.g. the terrain FP-flag list) from
   `engine/runtime/CMakeLists.txt`, rewording the runtime comment.
3. Flip the format-level tests' link word to `opennova_formats` in
   `tests/CMakeLists.txt`; consumer-level tests stay on `opennova_runtime`
   (it PUBLIC-chains formats).
4. Instruments: the citation ratchet is move-invariant (it walks every `engine/<group>/<lib>` source);
   update `include_graph_check.py` prefixes if the format was previously covered by
   a blanket-forbidden directory prefix.
5. Sweep the stragglers: doc path pointers (targeted per-file
   edits, never repo-wide sed — RE records mix moved and staying paths),
   README tables, `engine/CLAUDE.md` group lists.
6. Verify like step 4 plus: the format's full roundtrip suite green with ZERO
   `fixtures/**` drift, all lint gates, and any touched GUT surface in
   single-file isolation after `scripts/build_godot.sh` + full editor restart
   (the `gut` skill's silent-drop check).

## 7. Done

- `ctest --test-dir build -C Release` fully green including the new tests;
  roundtrip byte-exact on the fixture.
- Both CMake roots build (`bash scripts/build.sh` exercises both).
- `engine/CLAUDE.md`'s `formats/` list updated with the new lib.
- New domain vocabulary added to `CONTEXT.md` only if a term needed pinning.
