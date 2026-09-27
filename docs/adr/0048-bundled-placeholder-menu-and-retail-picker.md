# ADR 0048: A bundled placeholder menu, the retail picker, and no launcher

- **Status**: Accepted, 2026-09-27
- **Supersedes**: ADR 0045's "no bundled data" and "no folder picker, saved
  directory, or executable-adjacent discovery" clauses. Its CLI semantics
  stand. Retires the OpenNova Launcher that ADR 0015 placed outside the Godot
  product taxonomy.

## Context

ADR 0045 made `--resource-dir` mandatory: a player who double-clicked
`opennova.exe` got exit code 2 and a console line. OpenNova's own game data is
not ready, and the only way a player could use the project was to bring a
retail Joint Operations install and a command line.

The separately distributed .NET OpenNova Launcher pointed a stock retail
install at our NovaWorld through the hosts file and installed expansions from
a catalogue the NovaWorld service published. It carried a CI release flow, a
downloads bucket and CDN, an expansion publish pipeline (routes, tables, a
GitHub tag client, a web catalogue and admin page) and its own settings, all
to redirect one hostname.

## Decision

1. **`assets/` is OpenNova's own game data**, authored from scratch (no retail
   byte), committed as plain git blobs and shipped beside `opennova.exe` in the
   game zip. Today it holds only a placeholder main menu (`main.mnu`, one
   `STARTUP` screen with literal text) and its one font (`opennova.fnt`, minted
   by `tests/fixtures/minimal_fnt_builder.h` and byte-guarded by the
   `minimal_fnt_gen` ctest). It grows into the real game as that data arrives.
2. **No `--resource-dir` boots the bundled menu.** The shell mounts
   `<exe dir>/assets` (a source run uses the repo's `assets/`) as a plain loose
   root, without the retail boot-manifest report. `--resource-dir <path>`,
   `--loose-root`, `/d`, `/game` and `/exp` keep ADR 0045's semantics exactly;
   `--resource-dir` with no value is still a usage error (exit 2), and an
   unmountable one still exits 1.
3. **PLAY RETAIL hands the session to a retail install.** The bundled menu's
   `PLAY_RETAIL` and `CHANGE_FOLDER` buttons are wired by control name
   (`BundledMenuCompanion`, ADR 0001). PLAY RETAIL mounts the saved install
   when it still mounts, else opens a native folder picker; CHANGE FOLDER
   always opens the picker. A picked folder must mount as a packed runtime
   install (the boot-table archives; no loose fallback). A folder that does
   not is refused with a dialog, nothing is saved, and the bundled menu stays.
   A mounted pick is saved as `[resources] retail_dir` in `user://opennova.cfg`
   and the shell switches to that install's own menus for the rest of the
   session. A `--resource-dir` is still never saved. There is no F9 re-pick.
4. **The OpenNova Launcher is retired.** `launcher/`, its CI job and publish
   workflow, and the launcher-only `/api/server-info` route are deleted. Retail
   interop testing points `gs.novaworld.net` at a server with one hosts-file
   line (NW-L2 in `docs/net/novaworld-net-re.md`).
5. **The expansion distribution pipeline is retired with it.** The web
   catalogue and admin Releases pages, `/api/expansions`, the admin release
   routes, the `/admin/internal` publish callbacks, the GitHub tag client and
   its libcurl dependency, the `EXPANSION_*` configuration and the generated
   expansion seed are deleted. Migration `0007` drops the expansion tables and
   the launcher's `games.executable_name` column. The game's own expansion
   concept (`/exp`, `expansion/<name>/`, `exp_bits`, account game access,
   NWJoin's expansion checks) is untouched.
6. **The infrastructure is retired separately.** The downloads bucket and CDN,
   the `launcher_ci` IAM user and the `infra/github` expansion-repo stack stay
   until a dedicated pass (`TODO.md`); nothing in the application depends on
   them.

## Consequences

- Double-clicking `opennova.exe` shows a menu instead of exiting. The package
  smoke boots the staged zip layout with no arguments (exit 0) and keeps the
  `--resource-dir` usage-error run.
- `MenuShell.setup` reloads everything it reads from a root (cached documents,
  text tables, stylesheet, sound profile) when the root changes, so the
  bundled-to-retail switch shows the install's own menus.
- The bundled menu names its font and colors literally, so it needs no string
  table, stylesheet or cursor file. Everything the retail boot manifest lists
  stays absent from `assets/` until the game needs it.
