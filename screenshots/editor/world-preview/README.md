# Godot world preview review images

Captured in Godot 4.6.1 on Windows using D3D12.

- `preview-disabled.png`: the synthetic example with the OpenNova World plugin
  disabled. The scene nodes are present, with no native world rendered.
- `preview-enabled.png`: the same scene and camera with the plugin enabled.
  Terrain, water, and placed models render; the Inspector shows the loaded
  native filenames on the authored GameWorld node.

The synthetic pair uses only the original assets in `examples/world_preview`.
The repeatable editor save/reload check is
`godot/tests/tools/world_preview_editor_check.gd`.
