# Environment workspace

Tune the outdoor environment (`.env`): fog, sky, water, and time-of-day lighting
keyframes, previewed live on the scene you are editing. Part of the
[OpenNova Editor (ONED)](../README.md).

## What you do here

The Environment workspace is a popup panel rather than a full-screen workspace.
Open it from the Atmosphere row in the nav or the sun icon in a 3D view, and it
overlays the active Terrain or Object viewport so you can watch lighting changes on
your scene instead of switching away from it. Controls cover the environment name,
fog (level and type), sky (speed and height), terrain, water, and cloud tints, an
overall light scale, the current time of day, and a list of time-of-day keyframes
(each a time plus a set of colors) that the runtime interpolates between. New and
Save are in the panel.

## Formats

| Format | Backing library | Notes |
|---|---|---|
| `.env` | [`libs/env`](../../../libs/env) | environment: fog, sky, water, and time-of-day color keyframes (text, parsed per mission) |

## How it is built

A *popup* workspace: its `WorkspaceDef` is registered with `popup = true`, so the
shell toggles a panel instead of swapping the viewport. The adapter
[`environment_workspace.gd`](environment_workspace.gd) applies
edits to whichever Terrain or Object viewport is active.

| File | Role |
|---|---|
| [`environment_workspace.gd`](environment_workspace.gd) | popup adapter: builds the inspector, applies to the active viewport |
| `environment_editor.gd` | document model: load / save `.env`, time-of-day state, live preview |
| `environment_inspector.gd` | the fog / sky / water / tint / time-of-day keyframe controls |

## Related

- Editor framework: [`../README.md`](../README.md).
- Project overview: [top-level README](../../../README.md).
