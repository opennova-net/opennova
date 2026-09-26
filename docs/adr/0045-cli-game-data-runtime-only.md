# ADR 0045: CLI game data and a runtime-only distribution

> **Updated by [ADR 0048](0048-authoring-in-the-godot-editor.md).** `--resource-dir` also
> accepts a `res://` or `user://` directory, mounted as a file tree (the authoring content
> root, D-VFS-12), and an export built with `opennova/pack_content` carries its content and
> boots it with no arguments. The retail runtime export keeps this record's contract: no
> arguments print usage and exit 2, and an operating-system path mounts exactly as before.

- **Status**: Accepted, 2026-09-19
- **Supersedes**: ADR 0037; the bundled-data and picker contracts in ADR 0025;
  ONED product and distribution provisions of ADRs 0015 and 0039.

OpenNova ships an engine/runtime. Users supply their own loose or packed game
data through the required `--resource-dir` option. The source-owned game tree,
its generators and packaging, ONED, its process wrapper and its native UI are
removed. There is one Windows runtime archive and no bundled PFF or loose data.

The folder picker, saved directory, and executable-adjacent discovery are removed.
Missing/empty input prints usage and exits 2; a directory that cannot mount exits 1.
`/d`, `--loose-root`, `/game`, and `/exp` retain their existing loading semantics.
Game/expansion selection and player preferences remain; stale resource-directory
settings are ignored. Generic formats, native loading and the game's F3 tools stay.

Gameplay tests compose temporary resource roots from per-format fixtures and
native writers through RuntimeFixture, TestFs and WorldFixture. These fixtures
live outside the exported project and cannot act as default game data. Format
writer regression tests remain independent of the removed product bundle.

Historical ONED and retail-validation records remain historical references;
their application paths and run/pack commands are no longer supported.

The witness census drops `0x4a68a0` (early-error-file placement in the deleted
packing policy/package tool) and `0x58b470` (texture staging in that package tool).
The runtime format implementations and their other witnesses are unchanged.
