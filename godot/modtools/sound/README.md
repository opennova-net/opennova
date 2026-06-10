# Sound workspace

Author NovaLogic `.lwf` sound profiles: the trigger sets a menu, mission, or
object plays, each set built from layers of member sounds. Part of the
[OpenNova Editor (ONED)](../README.md).

## What you do here

A sound profile is a list of named Sound Sets; each set holds Layers, and each
layer holds Member sounds with per-member gain, pitch, and chance. The workspace
shows the whole profile as a tree (Sound Sets, then Layers, then Members) with an
add / remove / reorder toolbar and a Play button on every row, so you can audition
any set, layer, or member in place. The left inspector edits the selected row's
properties. Member selection on playback follows the engine's own state machine
(round-robin and random modes live in [`libs/audio`](../../../libs/audio)), so
what you hear in the editor is what the game picks at runtime.

Dialog banks (`.dbf`) are read by the engine for mission dialog but are not
authored here.

## Formats

| Format | Backing library | Notes |
|---|---|---|
| `.lwf` | [`libs/lwf`](../../../libs/lwf) | sound profile (LWF1): trigger sets, layers, member sounds; selection logic shared with the runtime via [`libs/audio`](../../../libs/audio) |

## How it is built

A non-3D data editor (no viewport camera) mirroring the Strings workspace: the
center hosts a self-contained editor view, the left inspector hosts the
per-selection form.

| File | Role |
|---|---|
| [`sound_workspace.gd`](sound_workspace.gd) | adapter: document ownership, change fan-out, shell binding |
| `sound_controller.gd` | document model: load / save `.lwf`, structure edits, selection, undo |
| `sound_inspector.gd` | left inspector: per-selection property form |
| `sound_preview_player.gd` | audition playback with engine-faithful member selection |
| `ui/sound_editor_view.gd` | center surface: sets / layers / members tree + toolbar |

## Related

- Editor framework: [`../README.md`](../README.md).
- Project overview: [top-level README](../../../README.md).
- Format and engine-behaviour record: [`docs/audio/lwf-dbf-sound-re.md`](../../../docs/audio/lwf-dbf-sound-re.md).
- Decisions: [ADR 0004](../../../docs/adr/0004-audio-selection-pushdown.md).
