# godot/modtools/ — the OpenNova Editor (ONED)

- Naming: this is "the OpenNova Editor (ONED)" or "the editor" — never "terrain editor".
  Terrain is one of its twelve workspaces; the scene root is the EditorApp node
  (`editor/editor_app.gd`), which owns boot wiring, window sizing, and the MCP service.
- Code-first: no `.tres` workspace resources, and the shell never switches on workspace
  type — it reads capability hooks off each workspace. The contract is
  `framework/editor_workspace.gd`; the how-to (add a workspace / add an inspector) is in
  [README.md](README.md), plus one README per workspace directory.
- UI copy is artist-facing: physical/visual language ("draw distance", "blend layer"),
  not engine internals ("CDEP", "LOD bitstream", "mip slot").
- Mission play-in-editor shares the game's runtime: one mission runtime + one present
  pass (docs/runtime-architecture.md and docs/adr/). Read those before touching
  `mission_runtime.gd` / `mission_present_pass.gd`. The editor/runtime sharing seams are
  in docs/oned/editor-runtime-parity.md.
- Workspace tests live in `godot/tests/` (GUT), suffix `_test.gd`.
