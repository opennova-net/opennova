# TODO

## General Mission

- [ ] Waypoint types needs to be an enum and then not a number in the UI
- [ ] Zones are awkward to create
- [ ] AI class and AI script: should be a selection, not a free input, if we can pull the options from a loadable resource (def, etc)
- [ ] "Weapon loadout" field semantics need an IDA grill (the editor panel itself is in)
- [ ] "Raw" group fields need an IDA grill and better editor integration
- [ ] Too much useless text noise in Mission tab
- [ ] Briefing needs to be a larger textbox. Also confirm whether it can be a string (rtxt) and integrate nicely if so
- [ ] "Music track" being a number is no good. Better integration
- [ ] Still no sound?

## BMS Scripting

- [ ] "PlayWavList" needs deeper editor integration
- [ ] Validate "reset after" / "pre mission" / "post mission" is implemented properly
- [ ] "ShowWaypoints" needs to be validated
- [ ] If there is no "sub-type" for an action (ie: only Null), just disable the box
- [ ] MisvarChange/Set etc need better editor integration

## Editor-wide

- [ ] The editor needs "depth": these files reference each other, and those references should be links

## Cleanup & verification backlog

- [ ] Terrain native `[orig]` citation pass: `godot/engine/terrain/` + `libs/terrain` carry no inline citations; grill-ida the mesh build / sampler / lighting chain and land a `docs/terrain/*-re.md` record (docs/README.md lists terrain as record-less)
- [ ] Present-pass / entity-reconcile citation pass: `engine/world/mission_present_pass.gd`, `mission_entity_registry.gd`, `wire_present_pass.gd` document design but carry no `[orig]` anchors; engine-research the original present/tick chain and cite into `docs/runtime-architecture.md` + `docs/correspondence.md`
- [ ] Two-net-stack convergence: the NovaNetClient replay/spectate path vs the NovaWorldClient/NovaSimulation listen-server path (see godot/engine/CLAUDE.md); decide convergence once the net workstream stabilizes
- [ ] `engine/mcp/` relocation (optional): all 11 files are class_name-referenced with zero `res://engine/mcp` literals, so it can move (e.g. next to `modtools/mcp/`) without path edits if engine/ layering ever needs it
- [ ] `opennova::io` adoption continuation: migrate remaining per-lib byte readers on-touch (policy in libs/CLAUDE.md); excluded: mus/wac VM cursors (faithful-port surface)
- [ ] `mission_controller.gd` full decomposition (beyond what the inspector split needed): extract selection/gizmo/placement concerns
- [ ] Mission workspace rail conversion: with the inspector decomposed into section components, moving Mission onto `_build_inspector_defs()` workflow rows is a small step, but it swaps the in-panel mode tabs for the shell's workflow rail (visible layout change) - needs a deliberate UX pass
