---
name: new-format-lib
description: Scaffolds a new NovaLogic format library under libs/ end to end — CMake target, opennova namespace, parser/writer, fixtures with LFS and asset-gating policy, roundtrip ctest, and optional GDExtension binding. Use when adding support for a new game file format, or when extracting a subsystem into a new portable library.
---

# Add a format library under libs/

Done dozens of times; the conventions are strict. Copy a living exemplar, don't
invent: `libs/dbf` (small binary format), `libs/lwf` + `tests/lwf/` (byte-exact
roundtrip), `libs/refs` (most recent conventions).

## 0. Decide before writing code

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

## 1. Library skeleton

`libs/<name>/CMakeLists.txt` + `include/<name>/<name>.h` + `src/<name>.cpp`.
Copy `libs/dbf/CMakeLists.txt` and rename: STATIC lib `opennova_<name>`,
PUBLIC `include/`, C++17, POSITION_INDEPENDENT_CODE. Namespace
`opennova::<name>` (C ABI exports flat and domain-prefixed). If Python/Godot
need it over FFI, bundle the static into `opennova_shared` (grep the root
CMakeLists for `opennova_shared`).

## 2. Register in BOTH build graphs — the classic miss

- Root `CMakeLists.txt`: `add_subdirectory(libs/<name>)` alongside the others.
- `godot/engine/CMakeLists.txt`: the parallel, hand-maintained list — required
  the moment an engine binding links the new lib.

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
following the dbf block — one executable per test, link `opennova_<name>`,
`add_test(NAME <name>_roundtrip ...)`.

Run loop:

    BUILD_GODOT=0 bash scripts/build.sh          # configure + build + full ctest
    ctest --test-dir build -C Release -R <name>  # then iterate on just yours

## 5. Optional: engine binding and editor surface

- Binding: `godot/engine/<name>/nova_<name>*.{h,cpp}`, `GDREGISTER_CLASS` in
  `godot/engine/register_types.cpp`, link in the engine CMake, then
  `bash scripts/build_godot.sh` and fully restart any open editor (no
  hot-reload). Add a GDScript smoke test `godot/tests/<name>_data_test.gd`
  using the fixture-skip pattern; run it via the `gut` skill.
- ONED workspace/inspector: follow "Add a workspace" in
  `godot/modtools/README.md`.

## 6. Done

- `ctest --test-dir build -C Release` fully green including the new tests;
  roundtrip byte-exact on the fixture.
- Both CMake graphs build (`bash scripts/build.sh` exercises both).
- README updated: the "C/C++ Libraries" table AND the library count in the
  Repo Layout row (it drifts).
- New domain vocabulary added to `CONTEXT.md` only if a term needed pinning.
