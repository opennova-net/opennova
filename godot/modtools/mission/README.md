# Mission workspace

Create or open `.bms` missions and author their world: place and edit objects,
waypoints, zones, and BMS event scripting, then press Play to run the mission
in-editor on the engine's own mission runtime. Part of the
[OpenNova Editor (ONED)](../README.md).

## What you do here

A mission's header selects its terrain and environment; both load into the shared
terrain viewport, and the mission's placed objects are instanced on top by the
host-agnostic placer. New Mission builds an empty mission on the currently loaded
terrain. From there you place entities from the object palette, move them with
viewport drag (terrain-grounded), and edit per-entity properties in the inspector:
identity, team, AI class and script, behavior, weapon loadout, and group fields,
all backed by the reflected mission schema. Waypoint paths, zones (area triggers),
markers, and mission properties (briefing, music, win conditions) are editable in
the same inspector, with undo/redo and Save / Save As.

The Events panel edits BMS scripting: triggers and actions with typed parameter
domains recovered from the original engine. Play runs the mission through the same
runtime and present pass the game host uses, so WAC scripts, BMS events, and AI
behave exactly as in the game runtime; Stop rewinds the world and restores the
authored scene (one runtime, see
[`docs/runtime-architecture.md`](../../../docs/runtime-architecture.md)).

While playing, the game's own controls apply: WASD moves, the mouse looks,
F4 switches first/third person, C and Z toggle crouch and prone, and F3 opens
the same Mission debug panel the game runtime has (the mouse is freed while it
is open so its controls are clickable; it is also available from the Simulate
panel while editing). Esc stops and returns to editing.

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
