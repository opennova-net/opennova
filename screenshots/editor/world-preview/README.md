# Godot world preview review images

Captured in Godot 4.6.1 on Windows using D3D12.

- `preview-disabled.png`: the example scene with the OpenNova World plugin
  disabled. The scene nodes are present, with no native world rendered.
- `preview-enabled.png`: the same scene and camera with the plugin enabled.
  Terrain, water, and placed models render; the Inspector shows the loaded
  native filenames on the authored GameWorld node.

These captures predate the example scene's switch to the game's own tracked
source tree (`assets/mnml.bms`); they show the former synthetic data set,
which has since been removed. The repeatable editor save/reload check is
`godot/tests/tools/world_preview_editor_check.gd`.
