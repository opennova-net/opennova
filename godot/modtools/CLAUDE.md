# godot/modtools/ - ONED

- ONED is the product name. It stores run settings, runs OpenNova loose,
  stages and runs retail, and stops its one child.
- The complete product decision is ADR 0037. Do not add asset authoring,
  workspaces, project/import state, previews, an asset database, or MCP.
- Keep the ONED interface small. Process discovery, child ownership,
  retail staging, working-directory rules, and error recovery stay behind it.
- The surface is the engine's ImGui window (`OnedUi`, ADR 0039): a new
  control is a new `OnedAction` in `engine/runtime/devtools/oned_ui.*` that
  `oned_app.gd` executes. No Godot `Control` UI here; the scene is the app
  node, the `OnedUi` node and two native `FileDialog`s.
- `opennova-modtools.exe`, the `OpenNova Mod Tools` preset, the `modtools`
  feature, and hidden `--pack-game` command are release contracts.
- Run OpenNova never mutates the selected game-data directory. Retail staging
  writes only beneath `user://packed`. The headless pack command writes only
  the requested game directory.
- The interactive retail layout is loose files plus a zero-entry boot PFF;
  the release pack layout is `localres.pff` plus loose-by-contract files. Do
  not collapse those two outputs.
- ONED tests live in `godot/tests/` (GUT), suffix `_test.gd`.
