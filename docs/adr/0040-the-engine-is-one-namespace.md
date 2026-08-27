# ADR 0040: the engine is one namespace — no Nova prefix, files follow classes, group-qualified includes

- **Status**: accepted (2026-08-26; maintainer directive)
- **Owners**: engine layout, build topology, the Godot binding layer
- **Supersedes/updates**: ADR 0034 decision 5's deferral ("file names keep
  their `nova_` prefix for now"); ADR 0029's and ADR 0024's include-root
  clause ("each GROUP directory is the one public include dir, so
  `#include <domain/file.h>` resolves"); ADR 0034 §2's list of the engine's
  non-Godot consumers. ADR 0020's terrain seam, ADR 0029's five group
  targets and PUBLIC chain, and ADR 0033/0034's boundary rule stand.

## Context

This is a port, but everything we build is a feature of *our* engine. There is
no separate "Nova layer" wrapping something else, so nothing in the tree earns
a `Nova`/`nova_` prefix: not the registered classes (retired in ADR 0034 d5),
not the files (deferred there), not the GDScript aliases and autoloads, not the
`NOVA_*` macros and guards, not the shader helper functions, and not the
C-linkage `gameprofile` records. The survivors are the words that are proper
nouns in their own right — `NovaWorld*` (the matchmaking service we speak the
wire protocol of), `NovaLogic` (the vendor), `opennova`/`opennova_*` (the
project).

Finishing the file rename exposed a real design flaw rather than a naming
nit. The engine's public include namespace was FLAT: each group target
exported its own directory as an include root, so `<mission/x.h>` was
resolved by CMake link order across four roots. The engine already aliased
itself — `formats/mission` vs `runtime/mission`, `formats/particle` vs
`runtime/particle`, `formats/wac` vs `runtime/wac` — surviving only because
the file names inside the twins happened not to collide. `godot/src`, whose
subdirectories mirror engine lib names and whose root is searched before the
engine roots, would have gained five exact path twins the moment `nova_` was
stripped (`audio/ambient_mixer.h`, `audio/sound_selector.h`,
`devtools/oned_ui.h`, `mission/mission_catalog.h`, `particle/effect_scene.h`
— each a binding whose class name equals the engine type it wraps, which is
the thin-seam ideal, not a mistake). The `nova_` prefix had been silently
carrying the uniqueness load; nothing enforced it.

## Decision

1. **No Nova prefix anywhere.** Files, identifiers, macros, header guards,
   GDScript `class_name`s, preload aliases, autoloads (`MusicService`),
   shader helper functions (`scene_output`, `is_q3_pass`, ...), and the
   C-linkage `gameprofile` set (`GameProfile`, `GameId`, `BootPhase`,
   `RequiredResource`, `ResourceSeverity`; enumerators `GAME_*`,
   `BOOT_PHASE_*`, `RES_*`). Survivors: `NovaWorld*` classes and the
   `novaworld_*` files that carry them (one proper noun, spelled like
   `engine/net/novaworld/` and `apps/novaworld_server/`), `NovaLogic`,
   `opennova`. "`nova_` never starts a file name" is now a lintable
   property.
2. **A file is named after the primary type it declares**, in snake_case;
   translation-unit splits keep `<class>_<facet>` (`simulation_present.cpp`).
   The class name, not the old file name, wins where they differed
   (`PffDocument` -> `pff_document.*`, `FrameFx` -> `frame_fx.*`,
   `WindowState` -> `window_state.gd`).
3. **`engine/` is the one public include root, and includes are
   group-qualified.** Every engine header is included as
   `#include <group/lib/file.h>` with group in {`base`, `formats`, `runtime`,
   `net`}: `<runtime/world/player_view.h>`, `<formats/mission/mission.h>`,
   `<base/vfs/vfs.h>`, `<net/npwire/peer_addr.h>`. Same-directory siblings
   may keep a bare `"file.h"`; every cross-lib include is the angle form.
   Each group target exports `engine/` PUBLIC. The group in the path is what
   makes the layer visible at the include line.
4. **The layering is a lint, read off the include path.**
   `scripts/lint/include_graph_check.py` (a PR gate) enforces: the ADR 0029
   d3 group order (`base/io` and `base/crt` include only each other;
   `formats` includes `base/io`, `base/crt`, `formats`; `base` includes
   `base`, `formats`; `runtime` adds `runtime`; `net` includes all four); no
   unqualified engine include anywhere under `engine/`, `apps/`, `tests/`,
   `godot/src`; the ADR 0020 terrain seam under its qualified prefixes; no
   include naming `godot` under `engine/`, `apps/` or `tests/`; and no
   `godot/src` subdirectory named like an engine group.
5. **Bindings keep `godot/src` as their include root** with quoted
   root-relative includes (`"audio/ambient_mixer.h"` is the binding,
   `<runtime/audio/ambient_mixer.h>` the engine type it wraps). Because no
   `godot/src` subdirectory may be named `base`, `formats`, `runtime` or
   `net` (decision 4), a binding path can never alias an engine path, and a
   binding may share its class name and directory name with the engine
   concept it exposes — the thin typed seam ADR 0034 asks for.
6. **The engine's portability has an honest consumer list.** ADR 0034 §2
   named an importer FFI and DCC plugins; those left with ADR 0037/0038 (no
   `oned_edit`, no shared-library export, no ctypes/pybind11). What consumes
   `engine/` without Godot today: the five `apps/` (the NovaWorld service,
   the in-match dev host, the LAN probe, the packet pretty-printer, the
   shared socket helpers), the ctest suite, the C-linkage headers, and any
   future non-Godot front-end. `engine/` stays Godot-free for them — now as
   a ratcheted property (decision 4), not a re-verified one.

## Consequences

- One rule for a reader: an angle include with a group prefix is the engine,
  a quoted root-relative include is the binding tree, a bare name is a
  sibling. The twin lib names (`mission`, `particle`, `wac`) stop aliasing.
- The `adapter_cpp_orig_cites_*`, `gd_orig_cites`, and size ratchets key on
  paths and survive the rename by rewriting their baselines in the same
  change; the maturity instruments are otherwise untouched.
- Historical records keep their historical names where they describe a past
  state; paths that name a current file are updated.
- No compatibility aliases: every consumer is rewritten in the same change
  (pre-1.0 policy).
