# ADR 0032: direct document I/O — godot/ adds nothing but Godot

- **Status**: accepted (2026-08-08; the adapter-shape round,
  maintainer-directed). **Replaced by
  [ADR 0033](0033-engine-owned-loops-device-shells.md) (2026-08-09)**, which
  restates this ADR's operative rules verbatim in its decision 3 so the
  architecture has ONE standing contract; this file stays as the record of
  the resource-system deletion itself.
- **Owners**: shell adapter layout
- **Supersedes/updates**: deletes ADR 0031 §1's band 4 ("res:// loaders/savers")
  as a permitted band — the layer it named no longer exists. Everything else in
  ADR 0031 stands.

## Context

The adapter registered 24 `ResourceFormatLoader`/`ResourceFormatSaver` objects
(plus a res:// import-plugin chain), one pair per NovaLogic format, integrating
every format with Godot's resource system. The 2026-08-08 census showed nothing
needed the layer: no `.tscn`/`.tres` references a Nova extension, no `.import`
sidecars exist, no export preset filters on one, nothing calls
`get_dependencies`/`load_threaded_*`, `res://` holds no game assets (the
front-priority NovaTexture loader guarded files that cannot occur there), and
production code loads every format through the documents' own byte/path APIs.
The layer was a second, mostly dead loading idiom whose only consumers were
tests asserting the layer itself.

## Decision

1. **Nova formats never integrate with Godot's resource system.** No
   `ResourceFormatLoader`/`ResourceFormatSaver`/`EditorImportPlugin` for
   NovaLogic formats; `ResourceLoader`/`ResourceSaver`/`load()` are never
   handed Nova files. Every document reads and writes itself — `Doc.new()` +
   `load_from_path()`/`load_from_bytes()` in, `save_to_path()` out — with the
   generic payload decode (SCR/BFC1) inside the `load_from_path` leg, where
   retail applies it. The `EditorResourceDocument` base makes `_save_resource`
   a required override; reaching the base is a wiring bug.
2. **The test for adapter code is: does this line exist because of Godot?**
   A line of `godot/` C++ or GDScript earns its place only by touching a Godot
   type, API, or lifecycle — marshalling, nodes, servers, Resource-shaped edit
   surfaces, device input, scene lifetime. Anything else (format semantics,
   witnessed math, gameplay rules, ordering contracts over engine types)
   belongs in `engine/`. This is ADR 0031's band test restated as the one-line
   rule the bands implement — *godot/ adds nothing but Godot* — and it applies
   to any future resource-system temptation exactly as it applies to
   everything else.

## Consequences

- 26 registered classes deleted across #451 and this round's cut (the format
  fleet, the NovaTexture front-priority loader, the import-plugin chain,
  `NovaDataFile`); `register_types.cpp` carries no resource-system calls at
  all, and `godot/src` registers ~90 classes, every one a binding,
  document, or presenter.
- The moved loader/saver bodies became direct-IO bindings:
  `TerrainData::save_to_path`, `TerrainTileInfo::load_from_path`/
  `save_to_path`, `SbfBank::save_to_path`,
  `MusicScript::load_from_path`/`save_to_path`,
  `CbinCreditsResource::load_from_path`/`save_to_path`.
- ADR 0031 §1's share column was a one-time estimate; the post-program
  file-level census measured binding glue ~21%, ONED document surface ~33%,
  presentation ~35%, documented seams ~7%, loaders ~3% → 0 with this round.
  Band conformance and the `adapter_cpp_orig_cites` ratchet remain the health
  metric (ADR 0031 §4) — deleting the loader band shrinks the surface, not the
  contract.
