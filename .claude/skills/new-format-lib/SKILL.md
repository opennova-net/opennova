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
`opennova_formats` group, and the group dir is the ONE public include dir
(`#include <name>/<name>.h` resolves repo-wide). Registration is exactly ONE
edit in `engine/formats/CMakeLists.txt`:

1. the source block, alphabetical by domain, with a one-line format comment:
   `# <name> — <what the format holds>` + one
   `${CMAKE_CURRENT_SOURCE_DIR}/<name>/<file>.cpp` line per TU.

The group already carries C++17, C99, POSITION_INDEPENDENT_CODE, and a PUBLIC
link to `opennova_io` — add nothing else; no include-list edit exists any
more (the pre-flatten `include/<prefix>/` + `src/` split and the per-lib
PUBLIC include entries are gone).

Include prefix: keep the format's historical prefix (usually `<name>/...`).
When a domain is split across formats/ and runtime/, the two libs may expose
ONE prefix with disjoint header sets (ADR 0030 decision 2; precedents:
terrain/terrain_query, particle/, mission/). Namespace `opennova::<name>` for
C++ surfaces.

## 2. Nothing to register twice

Both CMake roots share `engine/CMakeLists.txt`; the group target is already
linked by `godot/src` and the tests. Do not add a second target or a dynamic
FFI wrapper. One thing still matters:

- If the format is terrain-family (or otherwise seam-relevant), check whether
  `scripts/lint/include_graph_check.py` needs the new include prefix in its
  forbidden list — same commit.

## 3. Fixtures (`fixtures/<name>/`)

- Small representative samples ARE committed: `fixtures/**` is already LFS via
  `.gitattributes` — after `git add`, verify with `git lfs status` that they
  staged as LFS objects, not text.
- Bulk retail corpora are NEVER committed (copyright). Gate a sweep test on a
  runtime env var following `tests/mission/mission_corpus_test.cpp`:
  `std::getenv("OPENNOVA_...")`, and when unset print
  `skipped (set OPENNOVA_... to ...)` and exit 0 — skip-and-pass.
- Record where each committed sample came from in the test header comment.

## 4. Tests (`tests/<name>/`)

No test framework: plain `main()` with `TEST_EXPECT` from
`tests/common/test_expect.h`; locate the repo via `tests/common/test_paths.h`
(see `tests/dbf/dbf_roundtrip_test.cpp`). Minimum: parse test + byte-exact
roundtrip against the committed fixture. Wire into `tests/CMakeLists.txt`
following the dbf block — one executable per test, link `opennova_formats`,
`add_test(NAME <name>_roundtrip ...)`.

Run loop:

    BUILD_GODOT=0 bash scripts/build.sh          # configure + build + full ctest
    ctest --test-dir build -C Release -R <name>  # then iterate on just yours

## 5. Optional: engine binding

- Binding: `godot/src/<name>/nova_<name>*.{h,cpp}`, `GDREGISTER_CLASS` in
  `godot/src/register_types.cpp` (`godot/src` already links the group —
  no CMake link edit), then `bash scripts/build_godot.sh` and fully restart
  any open editor (no hot-reload). Add a GDScript smoke test
  `godot/tests/<name>_data_test.gd` using the fixture-skip pattern; run it via
  the `gut` skill.
- ONED is run-only. Format libraries do not add a workspace, inspector,
  project surface, or import flow; see ADR 0037.

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
5. Verify like step 4 plus: the format's full roundtrip suite green with ZERO
   `fixtures/**` drift and any touched GUT surface in
   single-file isolation after `scripts/build_godot.sh` + full editor restart
   (the `gut` skill's silent-drop check).

## 7. Done

- `ctest --test-dir build -C Release` fully green including the new tests;
  roundtrip byte-exact on the fixture.
- Both CMake roots build (`bash scripts/build.sh` exercises both).
- README updated: the "C/C++ Libraries" table row.
- New domain vocabulary added to `CONTEXT.md` only if a term needed pinning.
