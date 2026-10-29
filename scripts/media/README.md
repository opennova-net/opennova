# Editor showcase

The homepage uses a silent, 30-second feature reel with native playback controls.
Its sources live in `web/public/media/editor-showcase.{mp4,webm}`; the poster is
`editor-showcase-poster.webp`. `web/src/utils/media.ts` owns the paths and description.

## Capture

Use a disposable copy of the Acid Gambit project and isolated editor settings.
Keep raw takes, capture scripts and project checkpoints outside Git (for example,
under `.scratch/editor-showcase/`). Use the real editor and its MCP request surface
(`scripts/mcp/editor_mcp.py`, `docs/mcp.md`) to stage edits. Capture the rendered
window at **1920 × 1080**, with a readable editor font and a large document/preview
pane. Allow preview rebuilds to finish before saving a frame.

Export zero-based PNG sequences (`0000.png`, `0001.png`, …) into these directories.
The encoder interprets each sequence at 30 fps:

| Directory | Frames | Action |
| --- | ---: | --- |
| `01-overview` | 90 | Slowly orbit the completed mission |
| `02-terrain` | 90 | Raise terrain beside the outpost |
| `03-placement` | 60 | Place and rotate a watchtower |
| `04-patrols` | 120 | Draw a patrol path and assign units |
| `05-logic` | 120 | Add a group-alert action to an area trigger |
| `06-animation` | 120 | Preview a walking character, then show its skeleton |
| `07-menu` | 120 | Reposition the main-menu title in the preview |
| `08-play` | 180 | Walk through the exported mission in the actual runtime |

For gameplay, launch the mission using the editor's Play action. Reset to a safe
start before recording; verify the whole take stays in gameplay. Check the saved
PNG dimensions, since the runtime's movie output can use the project viewport
size instead of the requested window resolution.

## Encode and review

Install FFmpeg (including `ffprobe`) on PATH and supply a local TrueType font:

```powershell
python scripts/media/encode_editor_showcase.py .scratch/editor-showcase --font C:/Windows/Fonts/segoeuib.ttf
```

The script adds a separate caption band, encodes H.264 MP4 and VP9 WebM, and creates
a WebP poster. It checks the input sequences, silent output, 30-second duration,
and an 8 MB limit per video before replacing the published assets. Use
`--output-dir <directory>` to review an alternate encode first.

Review every cut and the final frame, then check homepage playback, seeking and
layout on desktop and a narrow viewport. The video must retain `controls`,
`muted`, `loop`, `playsinline` and `preload="none"`, without autoplay. Run the
landing-page tests, typecheck and production build from `web/` after changes.
