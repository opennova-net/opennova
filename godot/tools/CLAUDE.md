# godot/tools/ - game-data workflows

- Godot's editor and headless project commands own these workflows. There is no
  separate tools executable or product config.
- Native format codecs and packing policy remain in the portable engine.
- Process discovery, child ownership, retail staging, working-directory rules,
  and error recovery stay behind GameRunSession and GamePacker.
- OpenNova runs never mutate source game data. Retail runs use a staged copy;
  headless packing writes only its explicitly selected output directory.
- Keep the retail test layout (loose overrides or existing PFFs) separate from
  release packing (localres.pff plus loose-by-contract files).
- Tests drive real files and the public RunSessionPlatform interface. Follow
  godot/tests/CLAUDE.md and the GUT skill for collection/error checks.
