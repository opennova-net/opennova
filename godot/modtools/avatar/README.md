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
from scratch (`save_to_path`, ADR 0003 — never raw passthrough; writer policy
ADR 0021).

## Workspace

`avatars_workspace.gd` (`AvatarsEditorWorkspace extends EditorWorkspace`) holds
the document, mounts the 3D preview through a `ViewportMount`, and exposes three
workflow inspectors (`enum Workflow { TREE, PARTS, COMBOS }`, TREE default):

- **Tree** (`ui/inspectors/tree_inspector.gd`) — a read-only nationality ->
  division -> character tree; the rows show the RTXT name keys the `.def`
  references and a flag tag (e.g. `(skipdemo)`). Selecting a character shows that
  combo in the preview. Parser diagnostics are summarized above the tree.
- **Parts** (`ui/inspectors/parts_inspector.gd`) — a kind selector (head / body /
  arms) + a part list + a detail form (name, display name, the three `.3di`
  graphics, camo, voice, sex). Display keys use `StringRefWidget`; graphics use
  `ResourceRefWidget` so the editor can browse, validate, drag/drop, and jump to
  the Object workspace when the shell reference services are present.
- **Characters** (`ui/inspectors/combos_inspector.gd`) — pick a nationality +
  division, edit each combo's id and its head / body / arms part references, and
  add / remove combos. Missing resolved snapshots show inline warnings.

Each inspector commits by reading `db().get_model()`, mutating the matching entry,
and calling the workspace's `apply_model()`.

## Preview

`engine/avatar/avatar_preview.gd` (`AvatarPreview extends Control`, in the
shared engine layer because the game's PLAYER_INFO menu host mounts the same
preview) composes the resolved combo's third-person head / body `.3di` models
into one `SubViewport` scene under a shared environment, framed by a fly
camera with the editor grid and axis gizmo. The combo's arms reference remains
editable in Parts and Characters, but is not overlaid on the standing preview:
all four retail arm graphics use larger rigs with weighted references beyond the
19-bone preview skeleton. The current first-person runtime demonstrates that
`ArmsG` needs weapon/viewmodel rig context. Missing or unknown composed head/body
graphics are shown in an overlay instead of being silently ignored.

The composed head/body parts play the shared `Dt1rst.bad` + `PI_Idle.BAD`
preview idle when those assets resolve, and otherwise render static at rest.
The original combo → spawned-player model binding and in-game camo application
remain unwitnessed; see **D-PLAYERINFO-1** in the RE record.
