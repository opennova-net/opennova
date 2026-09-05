# ADR 0044: Godot authors native worlds

- **Status**: accepted (2026-09-05)
- **Updates**: ADR 0037's authoring direction; its ONED run and packaging contracts
  remain in place until their replacement is delivered.

Godot is the home for OpenNova world authoring. Native BMS, TRN, ENV, and mission
sidecar files remain authoritative; Godot scenes describe composition and editor
presentation. This keeps the authored result directly usable by the existing
runtime and retail-compatible tools, while Godot supplies the Inspector,
viewport, undo, save, and play workflow.

An editable world copy creates separately named native documents beside the
selected game data. Existing archive-backed assets remain dependencies. Its
explicit source mode uses the runtime's loose-override lookup policy; ordinary
retail previews retain archive-only mission selection. Generated scene nodes,
resolved textures, and preview environments are transient projections of those
documents and are never a second saved content model.

One world editing session owns the open native documents and their pending
changes. Environment base values remain separate from the mission's effective
overrides. Godot undo and save act on those documents, and Play starts the ordinary
game against their saved files. A later phase moves ONED's remaining launch and
packaging responsibilities before removing the separate modtools executable.
