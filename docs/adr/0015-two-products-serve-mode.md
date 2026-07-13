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
- **Product-owned settings.** The applications do not share mutable resource
  state. ONED owns `user://oned_settings.cfg`; the game owns
  `user://game_settings.cfg`. The former
  `user://terrain_editor_state.cfg` is ignored without migration or deletion,
  so launching either product cannot silently rewrite the other's root.
- **Resource launch policy.** A normal `opennova.exe` launch mounts only the
  retail PFF table in the game's saved root. Public `/d` keeps that same root
  and PFF requirement, adding loose files as overrides. ONED Play launches the
  game with `/d` plus a private root handoff after Godot's argument separator;
  that session mounts ONED's root loose-only and does not inspect or require
  PFFs. `/exp <name>` selects an expansion within the active root/mode and is
  session-authoritative when explicitly supplied. The public surface is
  `/d` and `/exp`; there is no OpenNova `/game` option.
- **Serve mode.** `opennova.exe` gains a serve entry (flag + the
  server-options menu path) that drives the existing host session bring-up —
  the ADR 0013 helper, the 62 Hz pump, the one seam. No new protocol code, no
  second gameplay network path. Headless serve must pass the packaging boot
  smoke like every other boot path.
- **`apps/nw_server` is reclassified.** It is the development and
  golden-harness host (the thin C++ binary the capture/diff loop drives), not
  a product seed. Its README says so; it is never packaged.
- **One supported engine family.** JO and DFX/DFX2 are skins of the same
  underlying game engine, so OpenNova ships one title-agnostic
  `opennova.exe` for that supported family rather than a per-title binary or
  runtime selector. The broader `libs/gameprofile` table remains intact for
  importer and PFF-tool asset decoding; its JO Demo and BHD rows are tooling
  profiles, not runtime-support declarations. Backend catalog and network
  identity data remain separate concerns.
- **The editor stays detachable.** A long-term goal (GOALS.md) is exporting
  a game from ONED, possibly with editing tools available in the exported
  game. That is not a deliverable of any current program; it is an
  architecture constraint recorded in
  [ADR 0016](0016-engine-editor-boundary.md): nothing the engine or game
  ships may depend on the editor layer.
