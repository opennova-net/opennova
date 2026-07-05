# MIS mission metafile support

Status date: 2026-06-18.

## Current verdict

OpenNova can now write `.mis` text from the in-memory mission document and load the same writer-generated `.mis` subset back into `bms::File` form. This is intentionally a conservative interchange path, not a claim that the full DFX2 Mission Editor grammar is mapped.

The live IDA MCP session available during this pass exposed only `Jointops.exe` (`Jointops.exe.kong.i64`). The expected editor database exists on disk as `C:\Users\taylor\Development\ida_dbs\dfx2med.exe.i64`, but no MCP-connected IDA instance had it open and the exposed API could not switch databases. A proper `dfx2med.exe` grill remains required before marking the full `.mis` grammar as matched.

## Implemented parser/writer subset

The writer in `libs/mission/src/mission.cpp` emits:

- `// mission metafile`
- `begin general_information` with header fields needed by the current mission workspace: names, terrain/env refs, attributes, counts, weather, time, map zoom, water/fog overrides, scores, wind, defaults.
- Empty `weapon_availability`.
- Optional `briefing`.
- Area trigger, waypoint, group, layer stubs for non-empty records.
- Events with nested `triggers` and `actions`.
- Placed entities as `begin item N` records with type id, id, transform, team/AI/waypoint/group fields, engagement distances, spawn limits, and editor-exposed strings.

The reader accepts the same shape. Unknown keys in known sections are skipped so a future writer expansion can be additive, but unknown top-level section semantics are not inferred.

## Known limitations

- All parsed `begin item` records currently land in the generic item pool. The `.mis` text does not carry the BMS pool kind directly, and the exact `dfx2med.exe` classifier behavior still needs IDA confirmation against `items.def`/type flags.
- The parser does not claim support for hand-authored or legacy `.mis` variants beyond the OpenNova writer subset.
- Weapon availability is emitted as an empty section and skipped on read; the loadout semantics are still tracked separately for mission editor work.
- No correspondence row is added to `docs/correspondence.md` yet because the editor-side function anchors were not available in this MCP session.

## Divergence catalog (D-MIS)

Stable IDs for the writer-subset gaps in "Known limitations" (dispositions in the canonical vocabulary of [divergence-ledger.md](../divergence-ledger.md)).

| ID | Divergence | Disposition |
|---|---|---|
| D-MIS-1 | All parsed `begin item` records land in the generic item pool; the `.mis` text does not carry the BMS pool kind, and the `dfx2med.exe` classifier behavior (against `items.def`/type flags) is not witnessed | **NEEDS-RE** — needs a `dfx2med.exe` grill (no MCP-connected IDA had that DB open this pass). |
| D-MIS-2 | `weapon_availability` is emitted as an empty section and skipped on read; loadout semantics are tracked separately for mission-editor work | **WITNESSED-READY-DEFERRED** — the writer subset intentionally omits it for now. |
| D-MIS-3 | The parser supports the OpenNova writer subset only; hand-authored / legacy `.mis` variants and unknown top-level section semantics are not inferred | **NEEDS-RE** — the full `dfx2med.exe` `.mis` grammar grill (the current verdict's stated prerequisite). |

## Verification coverage

- `tests/mission/mission_mis_test.cpp`: author a mission, save `.mis`, reload `.mis`, verify header/entity/event fields, then save/reload as `.bms`.
- `tests/mission/mission_c_abi_test.cpp`: generic C ABI `save_path`/`load_path` dispatch writes text `.mis` and reloads it by extension.
- Godot tests cover `NovaMissionData.save_as/open_file` dispatch and Mission workspace Open/Save As filters.
- `tests/resource_index/resource_index_test.cpp` and refs tests cover `.mis` as mission resources and dependency graph extraction.
