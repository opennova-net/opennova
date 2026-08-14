# HUD workspace (preview)

A read-only ONED workspace that previews the in-game HUD layout authored in
`hudpos.def`. It is the editor-side companion to the runtime HUD overlay; both
read the same data (`HudPos`) and draw with the same shell-neutral helpers
(`godot/src/ui/hud_*.gd`), so the preview and the live HUD cannot drift.

## Files

- `hud_workspace.gd` — `HudPreviewWorkspace extends EditorWorkspace`. Identity +
  open (`*.def`, loose or via the mounted VFS) + a 2D preview viewport + a
  read-only inspector. No new/save/export — authoring `hudpos.def` is out of
  scope (preview only), so the writer-shaped tiers stay at their base defaults.
- `hud_preview.gd` — `HudLayoutPreview extends Control`. Draws each HUD element
  at its design-space position (scaled by `HudLayout`) with stubbed live values:
  health bar, stance indicator, radar/spinmap bounds, the static HUD frame, and
  the text element positions. Real art (`.tga`) is loaded best-effort from the
  VFS; missing art degrades to labeled placeholder boxes so the layout is always
  legible.

## Faithfulness

The element model is the witnessed original — see
[`docs/interface/hud-re.md`](../../../docs/interface/hud-re.md). Notably the
IDB-misnamed "spinmap" function is the **stance indicator** (discrete
`HUDSTANCE` frames), while the separately authored `HUDSPINMAP*` rectangle is
the real heading-up gameplay map (D-HUD-1/-2); the design space is a fixed 1024x768
(`Viewport_ScaleToVirtualCoords @0x5d2b20`); and HUD text uses the original `.fnt`
bitmap fonts via `FntResource`. The weapon-coupled elements (ammo, weapon
name, dynamic crosshair spread) are follow-ups for the preview; they are live in
the game HUD. The preview shows only the gameplay spinmap's authored bounds;
the live HUD now compiles its terrain, retained 0x40/0x6B blips, pulse markers,
waypoint tether/distance, and the counter-rotating compass ring. The old
"minimap `@0x599700`" name remains a separate heat-bar misnomer.

The workspace is registered in the shell — a `WorkspaceDef` row in
`editor_workstation.gd` (`_workspace_defs()`), the `Workspace.HUD` enum entry, and
the `hud` icon in `editor/ui/icons/` — making it one of the thirteen workspaces; see
[the framework README](../README.md) for how registration works.
