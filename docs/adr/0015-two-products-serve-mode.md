# Two shipped products; the server is a mode of the game, not a product

OpenNova ships exactly two executables per platform:

- **`opennova.exe`** — the game. Pointed at a game directory (PFF-packed
  resources, exactly like a retail install), it boots the game from that data.
- **`opennova-modtools.exe`** — the OpenNova Editor (ONED). It produces the
  data the game runs.

There is no third "server" product. Hosting a dedicated game is a **serve
mode** of `opennova.exe`: the same binary, launched into hosting (the
server-options `.mnu` path a retail host used), runnable windowed or
`--headless`. This mirrors the original engine — NovaLogic shipped one game
exe that hosted, joined, and played — and it follows from two decisions this
project already made: single-player runs through the in-process listen server
([ADR 0011](0011-single-player-in-process-listen-server.md)), and there is
exactly one in-match network seam
([ADR 0009](0009-in-match-net-seam.md), [ADR 0013](0013-consolidated-net-core.md)).
A separate server binary would either duplicate that seam or be the same
binary with a different icon; both are debt.

## Decisions worth recording

- **Product taxonomy.** Two products, split by Godot export feature tags
  (`modtools` / `runtime_game`), each with a per-product main scene override
  in `project.godot` and a packaging boot smoke. Anything else that builds
  from this repo (the importer, `apps/nw_server`, `apps/nw_pp`, DCC plugins,
  the NovaWorld service) is a tool or a service, not a shipped game product.
- **Serve mode.** `opennova.exe` gains a serve entry (flag + the
  server-options menu path) that drives the existing host session bring-up —
  the ADR 0013 helper, the 62 Hz pump, the one seam. No new protocol code, no
  second gameplay network path. Headless serve must pass the packaging boot
  smoke like every other boot path.
- **`apps/nw_server` is reclassified.** It is the development and
  golden-harness host (the thin C++ binary the capture/diff loop drives), not
  a product seed. Its README says so; it is never packaged.
- **Titles are out of scope, deliberately.** JO and DFX2 are near-identical
  skins of one engine (retail DFX2 is approximately the JO binary with
  different data), so a per-title exe split is cheap *later* and premature
  *now*. Until a tracked decision revisits this: one title-agnostic
  `opennova.exe`, and no NEW hardcoded title identity where a named constant
  or config read is equally easy. The existing per-title data
  (`libs/gameprofile` table, `backend/seed/0001_games.sql`, gate strings in
  `libs/novaworld`) stays as-is — fragmented but recorded.
- **The editor stays detachable.** A long-term goal (GOALS.md) is exporting
  a game from ONED, possibly with editing tools available in the exported
  game. That is not a deliverable of any current program; it is an
  architecture constraint recorded in
  [ADR 0016](0016-engine-editor-boundary.md): nothing the engine or game
  ships may depend on the editor layer.
