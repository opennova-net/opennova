# Mission workspace

Create or open `.bms` missions and author their world: place and edit objects,
waypoints, zones, and BMS event scripting, then save and run the game from the
editor's main toolbar to test the loose mission. Part of the
[OpenNova Editor (ONED)](../README.md).

## What you do here

A mission's header selects its terrain and environment; both load into the shared
terrain viewport, and the mission's placed objects are instanced on top by the
shell-agnostic placer. New Mission builds an empty mission on the currently loaded
terrain. From there you place entities from the object palette, move them with
viewport drag (terrain-grounded), and edit per-entity properties in the inspector:
identity, team, AI class and script, behavior, weapon loadout, and group fields,
all backed by the reflected mission schema. Waypoint paths, zones (area triggers),
markers, and mission properties (briefing, music, win conditions) are editable in
the same inspector, with undo/redo and Save / Save As.

The Events panel edits BMS scripting: triggers and actions with typed parameter
domains recovered from the original engine. ONED does not host a second mission
runtime: testing always launches the real game over saved loose assets. F5 runs
the game normally and F6 launches the current saved mission.

## Formats

| Format | Backing library | Notes |
|---|---|---|
| `.bms` | [`libs/mission`](../../../libs/mission) | mission records and schema; events run on the BMS event runtime; the simulation lives in [`libs/world`](../../../libs/world) + [`libs/wac`](../../../libs/wac) |

## How it is built

The adapter lives with the other multi-pane adapters in
[`mission_workspace.gd`](mission_workspace.gd); the heavy
lifting is in this folder.

| File | Role |
|---|---|
| [`mission_workspace.gd`](mission_workspace.gd) | adapter: shell binding (viewport mount, inspector, action bar) |
| `mission_controller.gd` | document model: create / load / resolve / place / edit / save, selection, undo |
| `mission_inspector.gd` | left browser + right dock: per-selection editors and the Mission form |
| `mission_entity_fields.gd` | reflected entity parameter schema |
| `mission_param_schema.gd`, `param_slot.gd` | typed parameter domains for trigger / action editing |

## Related

- Editor framework: [`../README.md`](../README.md).
- Project overview: [top-level README](../../../README.md).
- Format and engine-behaviour record: [`docs/mission/bms-event-runtime-re.md`](../../../docs/mission/bms-event-runtime-re.md), [`docs/world/world-wac-ai-re.md`](../../../docs/world/world-wac-ai-re.md).
- Runtime decisions: [ADR 0006](../../../docs/adr/0006-unified-mission-runtime-present-pass.md), [ADR 0007](../../../docs/adr/0007-skeletal-runtime-and-entity-visual.md).
