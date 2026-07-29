# Importer pipeline — audit (PAR-R6)

Unlike the other PAR-R audits, the importer is **our tool**, not an original
NovaLogic binary — there is nothing to reverse-engineer here. This audit
establishes that the importer introduces **no independent parity divergence**:
it is a composition of already-RE'd format readers plus a DCC (Blender) scene
builder, and every parity-bearing decision lives in an existing record. It
converts the **Importer pipeline** from `UNAUDITED` to *tracked-by-composition*
(divergence-ledger.md), 2026-07-05.

## What the importer is

`onimport.exe` = `apps/importer/` (the CLI, dispatcher, job runner, and Blender
`scene_builder`) over `pyopennova/` (the Python FFI layer to the native `libs`).
The dispatcher (`dispatcher.py`) is a worker pool; `import_runner.py` /
`jobs.py` / the `scene_builder/` package orchestrate reading an asset and
building a Blender scene / glTF component.

## Parity surface = the composed format readers (each already RE'd)

Every format the importer reads is parsed by a `pyopennova` FFI module that wraps
a native `libs` reader, and each of those has its own RE record and D-catalog —
the importer inherits their parity, it does not re-derive it:

| pyopennova FFI | Native lib | Parity record |
|---|---|---|
| `threedi_ffi` / `adm_ffi` | `libs/threedi` | [threedi/3di-gp-format-re.md](../threedi/3di-gp-format-re.md) |
| `bad_ffi` (skeletal anim) | `libs/anim` | `.bad`/`.adm` skeletal runtime record |
| `def_ffi` (item defs) | `libs/def` | [world/itemdef-re.md](../world/itemdef-re.md) (D-ITEMDEF) |
| `pff_ffi` (archives) | `libs/pff` / `libs/vfs` | [vfs/vfs-pff-mount-re.md](../vfs/vfs-pff-mount-re.md) (D-VFS) |
| `scr_ffi` (SCR container) | `libs/scr` | D-SCR register (ledger PERMANENT) |
| `ase_ffi` / `ase_material_writer` | `libs/threedi` ASE path | 3DI material-pipeline record |

## Where the importer's own choices live — and why they are NOT parity divergences

The importer's non-format logic is **output mapping** — parsed engine data →
Blender datablocks / decomposed glTF. That target is a DCC interchange, not the
original engine, so "does it match the original" does not apply; the governing
contracts are:

- [ADR: decomposed glTF pipeline](../../GOALS.md) / the `opennova_*` custom-property
  convention (export `.glb` components, not a monolithic `.blend`).
- The DCC roundtrip spec (Phases A–J) — the authored-vs-imported fidelity contract.
- `coords.py` (the Y-up / Z-negation convention) — a single shared transform,
  itself pinned by the format records' coordinate notes.

A bug in that mapping is a DCC-roundtrip regression (caught by the importer
tests below), not an original-engine parity divergence — so it belongs to the
DCC spec's tracking, not a `D-IMPORTER` catalog.

## Coverage

Behavior is pinned by `tests/test_importer_entrypoint.py`,
`test_importer_integration.py`, `test_importer_jobs.py`,
`test_importer_screenshot_capture.py`, and `test_max_import_runner.py` (plus the
`pyopennova` FFI unit tests). Asset-gated integration paths follow the
[asset-gated-tests.md](../asset-gated-tests.md) policy.

## Verdict

**Tracked-by-composition, no independent divergence.** The importer has no
`D-IMPORTER` catalog because it owns no parity-bearing format logic of its own:
format fidelity rides the six records above, and output fidelity rides the DCC
roundtrip spec. If a future importer stage ever re-implements a format read
in Python instead of calling the native lib, that stage would need its own row —
none exists today (`pyopennova` is FFI-thin over `libs`).
