# The editor demo recorder

Records the OpenNova editor demo: a packaged Windows editor driven through its MCP
(`scripts/mcp/editor_mcp.py`, docs/mcp.md "The editor MCP") under Godot's Movie Maker, cut into a
captioned MP4 (about 94 s, 1600x900, 30 fps, the file card's wave as its audio) and a README GIF
(about 29 s, 960 px wide, 15 fps). Windows only; experimental, like the editor it shows.

| File | What it is |
|---|---|
| `record_demo.py` | The entry point: one subcommand per stage |
| `storyboard.json` | The scenes in cut order: captions, cut points, the takes and their parts, the GIF's ranges |
| `demo_lib.py` | The driver: the work folder, the storyboard, the editor process and its MCP, a take's marks and holds |
| `takes.py` | The recorders: take A and take B, one function per part |
| `prep.py` | The unrecorded stages: `prep`, `reset`, `tidy` |
| `cut.py` | The edit list, the encode, the GIF and the cleanup |

## What it records

Two takes, each one editor launch:

- **Take A** (part `project`): the welcome page with its recents and the game install checked, its
  New project form filled (the workspace's `new_project`), the new project ("Operation Nightfall", an
  expansion building on jox01), the import chooser, one mission's plan (CP10, grouped, Needed by in
  words) and its import.
- **Take B** (on the project take A made), parts in recording order: `files` (Files by kind),
  `mission` (CP10 in 3D: daylight, a glide into the village, a truck picked by the canvas's own click
  and dragged, an event and the script edited with a command's words shown as a hover shows them,
  Problems naming what blocks a build), `defs` (an item in words, weapons by
  game text, Duplicate and save), `menus` (a menu on the canvas, a label typed live), `models` (the
  Blackhawk, its materials and collision layers, a soldier's run clip), `textures` (a thumbnail, a
  texture as the game reads it and as drawn), `build` (save all, build, its result panel closed, then
  Play behind: the game's own screenshot), `card` (a wave's file card, the wave playing, the card
  closed).

`storyboard.json` is the storyboard: each scene's caption, what it shows, the requests that drive
it, and the marks its frames are cut between. The recorders lay down the marks; the cutter reads
captions and cut points from the storyboard alone.

## Requirements

- Windows, Python 3.11 or newer, and Pillow (the cut). Nothing else from PyPI.
- The packaged editor: `editor/opennova-editor.exe` from an `opennova-editor-windows-v<version>.zip`
  (CI's packaging, or `scripts/package_godot_windows.ps1`).
- The retail game install (JO:CA with jox01 installed): its data is imported, never written.
- A portable ffmpeg with libx264 and libass (a gyan.dev "essentials" build works), passed by its
  path as `--ffmpeg` (never looked up on PATH). Do not install it system-wide or change PATH for it.
- Disk: the raw PNG frames run to gigabytes per take, and the import copies about 9,000 files into
  the project. Delete the frames once encoded (`clean`).

## Running it

Every stage takes `--work <work>`; everything a run writes lands under it:

| Folder | Holds |
|---|---|
| `appdata/` | The editor child's APPDATA: its settings, `imgui.ini`, recent projects |
| `tmp/` | The editor child's TMP and TEMP; each launch's Godot log, stdout log and pid file |
| `projects/` | The demo project (`nightfall`) and the recents' projects |
| `frames/<take>/` | A take's Movie Maker frames (`frameNNNNNNNN.png`, `frame.wav`), `marks.json`, `take.json`, `game.png` |
| `rehearse/<take>/` | A rehearsal's screenshots at the marks, and its marks |
| `cut/` | The linked frame sequence, `captions.ass`, `cut.json` (the resolved edit list), `scenes.json` |
| `out/` | `opennova-editor-demo.mp4` and `opennova-editor-demo.gif` |

```bash
# once per work folder: the welcome page's recents
python scripts/demo/record_demo.py prep     --work <work> --editor <editor.exe> --install "<JO install>"
# take A: welcome page, new project, import
python scripts/demo/record_demo.py record a --work <work> --editor <editor.exe> --install "<JO install>"
# an empty workspace for take B, a rehearsal (screenshots, no movie), then take B
python scripts/demo/record_demo.py tidy     --work <work> --editor <editor.exe>
python scripts/demo/record_demo.py record b --work <work> --editor <editor.exe> --rehearse
python scripts/demo/record_demo.py record b --work <work> --editor <editor.exe>
# out/opennova-editor-demo.mp4, then out/opennova-editor-demo.gif, then the raw frames deleted
python scripts/demo/record_demo.py cut      --work <work> --ffmpeg <ffmpeg.exe>
python scripts/demo/record_demo.py gif      --work <work> --ffmpeg <ffmpeg.exe>
python scripts/demo/record_demo.py clean    --work <work>
```

- `prep` makes the recents' projects (Desert Patrol, River Run on jox01) from the install, which
  the editor then remembers for the welcome page's New project form. Run it once per work folder.
- `record a` refuses when the demo project exists: `reset` takes it off the recents and deletes
  its folder before a retake.
- `record b` opens the project take A made. Take B saves what it edits (Build saves all), so a
  retake of B starts from the edited project; for a clean retake, `reset` and record take A again.
- `--rehearse` runs a take with no movie: the editor's own screenshot at each mark (under
  `rehearse/<take>/`), holds cut short, unsaved edits discarded at the quit. Use it to check a
  scene before spending a take on it.
- `--only build,card` records just those parts (into `frames/b-build-card/` unless `--name` says
  otherwise; `--replace` deletes an earlier take in that folder). The cut takes each scene from
  the newest take that recorded its part, so a retake of some parts replaces just their scenes.
- `cut --partial` skips the scenes no take has recorded yet; `--fonts-dir` names where the caption
  font (Segoe UI Semibold) is when the system's fonts folder lacks it.
- `clean --projects` deletes the projects too. Each take's `marks.json`, `take.json` and `game.png`
  stay, so what was recorded stays known.
- Each editor gets its own MCP port (the storyboard's per take; 8993 for `prep`, `reset`, `tidy`);
  `--port` moves one off a port in use.

## The rules it keeps

- **No cursor, no focus, no desktop capture.** The editor starts behind every other window
  (`game_mcp.BehindLaunch`: the foreground lock taken for the start, the first window shown without
  activation, each window sent to the bottom of the z-order) and is driven through its MCP alone:
  nothing moves the mouse, sends OS input or brings a window to the front. Play starts the game behind
  (`play {behind}`: the editor shows its window without activation and sends it to the bottom while it
  starts, keeping the foreground). The frames are
  Godot Movie Maker's (`--write-movie <take>/frame.png --fixed-fps 30`), never a screen grab; the
  Play scene's picture is the game's own `game_screenshot`. Never minimize the window to hide it:
  a minimized window draws nothing, and restoring it takes the foreground.
- **The watchdog only reads.** A thread reads the editor window's show state and client size every
  half second and records each change as a `WINDOW` mark (every mark also checks it, adding
  `WINDOW-CHANGED`); it never changes the window. A take whose window was minimized, maximized or
  resized says so in its marks and in `take.json`.
- **APPDATA isolation.** The editor child's APPDATA (Godot's `user://`: the editor's settings,
  `imgui.ini`, recent projects) and its TMP and TEMP point into the work folder, so the user's own
  editor settings are never read or written. Nothing else of the environment changes, and no stage
  reads configuration from environment variables: every path is an argument.
- **Movie time is frame time.** Movie Maker renders every frame at a fixed 30 fps whatever the
  wall time, so a hold of N seconds waits for N*30 more frames written (`Editor.hold`), never a
  sleep; frames that stop coming (a minimized window draws none) fail the take after ten times the
  hold in wall time, 30 s at least. Operations (new project, import, build) are polled through the `operation` query until
  they end, never awaited through the request tool's own 300 s wait: under Movie Maker an
  operation's frames are slow in wall time.
- **Only its own processes.** Each stage quits the editor it started (`editor_request quit`, the
  unsaved-changes prompt answered as asked); only an editor that does not take the quit is ended,
  and only that process.

## The storyboard's format

- `scenes[]`: `id`, `take`, `part` (the take's part that records it), `caption` (one line, no em
  dashes), `shows` and `mcp` (documentation), and `cut[]`: pieces played in order.
- A piece runs `from` one mark `to` another, each a mark label or `label+N` / `label-N` frames
  from it, the end exclusive. `seconds` speeds the piece up to about that long (every Nth frame);
  `hold` holds its last frame that many seconds. A `still` piece shows a PNG of the take (scaled to
  the frame) for `seconds`.
- `audio`: the scene, the mark where the take's own `frame.wav` is cut from (`lead` seconds
  before it), and how long.
- `gif.ranges[]`: `from` a scene (to the end of `to`, or of itself), `skip` seconds into it,
  `seconds` long when given; resolved against the spans the cut wrote to `cut/scenes.json`.
- A new part needs a function in `takes.py` (`PARTS`) that lays down the marks its scenes name.

## The editor's own forms it drives

Every gesture a take shows is the editor's own over its MCP (the editor MCP's gaps lane): the New
project form filled and the build result's panel and the file card closed through `set_workspace`
(`new_project`, `build_result`, `card`), the card opened by `about_file`, a mark picked by the
viewport's `click` command (the canvas's own click), a script command's words shown by the script
viewport's `assist` option, and Play started behind by `play {behind}`.
