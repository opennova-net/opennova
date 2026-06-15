# Avatars workspace

The ONED Avatars workspace edits `Avatars.def` — the Joint Operations character
database that composes modular head / body / arms parts into `combo` characters
under a `nationality -> division` tree, and that the `PLAYER_INFO` menu surfaces.
Format and behavior are witnessed in
[docs/playerinfo/avatars-re.md](../../../docs/playerinfo/avatars-re.md)
(`[orig: CAvatarDefs_Init @ 0x57b180]`,
`[orig: CAvatarDefs_ParseConfigLine @ 0x57a3f0]`); the parser, writer, and
authoring model live in `libs/avatars` behind the `NovaAvatarDatabase`
GDExtension class.

## Document

`avatars_document.gd` (`AvatarsDocument extends EditorResourceDocument`) wraps a
`NovaAvatarDatabase` and owns the open / save / dirty lifecycle. Every edit flows
through `set_model()` on the database, whose `changed` signal the base turns into
the dirty flag and `state_changed` — so the shell derives Save / undo state from
one document. New documents start as an empty database; save writes the `.def`
from scratch (`save_to_path`, ADR 0009 — never raw passthrough).

## Workspace

`avatars_workspace.gd` (`AvatarsEditorWorkspace extends EditorWorkspace`) holds
the document, mounts the 3D preview through a `ViewportMount`, and exposes three
workflow inspectors (`enum Workflow { TREE, PARTS, COMBOS }`, TREE default):

- **Tree** (`ui/inspectors/tree_inspector.gd`) — a read-only nationality ->
  division -> character tree; the rows show the RTXT name keys the `.def`
  references and a flag tag (e.g. `(skipdemo)`). Selecting a character shows that
  combo in the preview.
- **Parts** (`ui/inspectors/parts_inspector.gd`) — a kind selector (head / body /
  arms) + a part list + a detail form (name, display name, the three `.3di`
  graphics with a picker, camo, voice, sex).
- **Characters** (`ui/inspectors/combos_inspector.gd`) — pick a nationality +
  division, edit each combo's id and its head / body / arms part references, and
  add / remove combos.

Each inspector commits by reading `db().get_model()`, mutating the matching entry,
and calling the workspace's `apply_model()`.

## Preview

`avatar_preview.gd` (`AvatarPreview extends Control`) composes the resolved
combo's head / body / arms part `.3di` models into one `SubViewport` scene under a
shared environment, framed by a fly camera with the editor grid and axis gizmo. A
missing or unknown part graphic simply skips that slot.

The parts render static at rest: the original combo -> spawned-player model
binding (and the in-game camo application) is unwitnessed — see
**D-PLAYERINFO-1** in the RE record — so the preview and `apply_camo()` stop at
the resolved part geometry behind that seam.
