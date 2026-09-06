# Game data workflows

Open the source project in Godot 4.6.1 after building the GDExtension and
installing the addons (`scripts/bootstrap_godot.sh`). The **OpenNova** dock
starts in the bottom panel; **Project > Tools > OpenNova: Game data tools**
opens it. Godot saves its dock layout.

The active scene's `GameWorld.world_source` supplies the data directory and
mount policy. Configure it using the [world editor](../addons/opennova_world/README.md).
The dock has no separate workspace or data selection. Retail installation and
pack output paths are machine-local Godot project metadata.

- **Run Game** saves pending native edits and opens OpenNova's ordinary menu
  using that world's data. **Play World** in the viewport toolbar opens its
  selected saved mission. Both launch the current Godot executable and project.
- **Stage & Run Retail** saves, copies data and a Joint Operations runtime into
  `godot/.godot/opennova/retail/`, and opens the retail menu on Windows. Select an
  installation containing `Jointops.exe`, Bink, and `game.cfg`. The real
  `binkw32_.dll` takes precedence over a hook's `binkw32.dll`. Base JO/demo
  sources without expansion are supported; other title/expansion launch policies
  need a separate compatibility slice.
- **Stop** stops only the child started by this editor. Starting another target
  waits for the old child to exit before staging or launching. Closing the editor
  also stops its child.
- **Pack Game Data** saves and writes a loose source world into an empty output
  folder outside the source tree. A previous pack's marked output can be reused;
  stale files are removed. Foreign nonempty folders and overlapping paths are
  refused. Existing archives are not unpacked: archive-backed world copies keep
  their retail dependencies and cannot be presented as complete distributable packs.

Native save conflicts or invalid Inspector values block run and pack actions.
Packing and staging perform filesystem work synchronously; large retail copies
can pause the editor. The result reports archived, loose, skipped files and
unhandled subdirectories. Sources must be flat native game-data trees.

`GameRunSession` owns process replacement and Stop. `RunSessionPlatform` supplies
OS calls and staging; tests fake that public interface. `GamePacker` uses the
native `PffDocument` writer and its engine-owned packing policy. It preserves
retail's boot archive names, 16-byte entry names, loose SBF/early-error files,
and separate staging/release layouts. Retail sources retain archive-only lookup;
loose sources and editable game-data copies enable loose overrides.

## Headless release packaging

From the repository root:

```bash
"$GODOT_BIN" --headless --path godot --script res://tools/pack_game.gd -- --pack-game <loose-source> <game-output>
```

Exit 0 means `localres.pff` and required loose files were written. The source
project must have its native extension and class cache ready (`--import`). The
output can already contain the runtime executable: only generated artifact
names are overwritten. No standalone tools executable is required or exported.

`scripts/package_godot_windows.ps1` invokes this same command and produces both
existing ZIP names: the development download contains the game with tracked
loose `assets/`; the game download contains the game with packed data. Debug
exports include the F3 ImGui addon; release exports omit it. Editor addons,
examples, workflow scripts, tests and probes stay out of the game PCK.

Run `game_packer_test.gd`, `game_run_session_test.gd` and `export_presets_test.gd`
for focused GUT coverage. The real dock check is
`res://tests/tools/game_workflow_editor_check.gd`; run it in a disposable editor
project from the Script editor's **File > Run** action.

Use a disposable project with its own editor cache and stable copies of the
editor addon. Do not edit its scripts while a native preview check is running.
