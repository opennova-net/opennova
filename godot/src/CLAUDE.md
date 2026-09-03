# godot/src/ — pure C++ GDExtension bindings (part of the core engine)

The Godot layer's native half (ADR 0016/0028/0034 d6): C++ only — the
GDExtension classes binding `engine/` to Godot. Register new classes in
`register_types.cpp`. ADR 0035 supersedes ADR 0033's callback-bus design: a
line here earns its place only as a device leg (marshalling, nodes, servers,
input, audio, draw-list appliers), the typed bridge from `inmatch::Session` to
Godot's `GameFramePipeline`, or a documented seam bridge — format/runtime logic
and every witnessed behavior belong in `engine/`. The engine's formats never touch
Godot's resource system
(documents self-read/write via `load_from_path`/`save_to_path`).
Names and includes (ADR 0040): there is no "Nova layer" — a file is named after the
type it declares, no `nova_`/`Nova` prefix anywhere (`NovaWorld*` is the service's
proper noun and stays); a binding may share its class name and directory with the
engine concept it exposes (`AmbientMixer` in `audio/ambient_mixer.h` wraps
`opennova::audio::AmbientMixer` from `<runtime/audio/ambient_mixer.h>`). Binding
includes are quoted and root-relative (`#include "audio/ambient_mixer.h"`, this
directory is the include root); engine includes are `<group/lib/file.h>`; no
subdirectory here may be named `base`, `formats`, `runtime` or `net`
(`include_graph_check.py` enforces all three).

Citations (ADR 0042 d7, ADR 0043): a witness citation is `[orig: Name @ 0xADDR]`
everywhere — there is no second marker form. `godot_orig_cites` is ONE
non-increasing count over the whole Godot side (`godot/src` plus the
`godot/game`, `godot/modtools` and `godot/probes` GDScript): a cite moves freely
between GDScript and binding C++; the count shrinks only when witnessed code
moves to its engine home (or dies as verified dead code) and may never grow.
Genuinely witnessed engine behavior still belongs in `engine/` (ADR 0042's
boundary rule). `scripts/lint/cite_census.py` keeps the SET of cited addresses
from losing a member silently.

Size ratchet: no `.cpp`/`.h` here past 2500 lines — split by responsibility
(one type per TU pair), never by "leg"; `scripts/lint/ratchet_counts.py` fails
on any increase. Shipping GDScript has no size ratchet; it has
`gd_foreign_private_accesses` instead: a script that reaches into another
object's `_privates` is a method annex, not a class, and the count only falls.

The game-level GDScript runtime (world, debug, mission, object, terrain,
ui, avatar, mcp, resource_index, strings, util) lives in `godot/game/` (ADR 0034 d6) — anything
there that is really engine behavior is the C++ rewrite queue.

Placement rule: no GDScript here, ever. Game scripts go to `godot/game/`;
ONED scripts go to `godot/modtools/`.

Error/diagnostic channels (ratcheted at zero — `gd_prints_outside_debug`,
`cpp_binding_console_writes`): a failure the caller already receives through the
return/error contract reports context via `push_warning` (negative-path tests
drive those legs; GUT counts engine errors as failures); `push_error` is for
invariant violations nothing recovers from. Load/lifecycle narration uses
`print_verbose` (visible under `--verbose`), live inspection goes through the
dev tools (F3: engine-owned ImGui windows; `devtools/` here is the
`ImGuiPassNode` seam with its two product nodes `DevTools`/`OnedUi` and the
`FrameStats` board binding, ADR 0039), and `engine/` diagnostics ride the
`io/log.h` sink.
Never raw `print`/`printerr`/`print_line`/`WARN_PRINT`/`ERR_PRINT` in shipping
code.

Gotchas:

- After any native change here: run `scripts/build_godot.sh` and fully restart the Godot
  editor — GDExtension registration does not hot-reload, and GDScript referencing an
  unregistered class fails to parse (GUT then silently drops those test scripts).
- The editor loads only `godot/bin/libopennova.*`. A stale editor means a stale
  `godot/bin` DLL — rebuild via `scripts/build_godot.sh` and fully restart.
- godot-cpp `Basis(axis, angle)` diverges from core Godot for negative-component axes.
  When porting GDScript Basis math to C++, add a parity test first.
- `ResourceRoot::set_root_dir` clears the dir index and texture caches — a 94s -> 2s
  mission-load regression hid here; do not call it casually. `list_files()` is
  kind-curated: load known filenames via `read_file`/`has_file`; don't expect them listed.
- Bind native C++ engine APIs directly. Do not introduce a parallel flat FFI surface.
- Net bindings (`network/novaworld_client`, `network/lan_session`) are thin pumps
  over the wire-compatible codecs — `engine/net/npwire` for the in-game codec + capture decode
  (ADR 0019), `engine/net/novaworld` for matchmaking — sockets and signals here, protocol and
  crypto in `engine/` (ADR 0010). Keep wire behavior in the portable libs so it stays
  unit-testable and interoperable; see `docs/net/novaworld-net-re.md`.
- Decoded in-match entities have one runtime fold and one presenter (ADR 0026,
  ADR 0043 d9): `ClientReplicaPipeline` owns `ClientState`, and the one native
  `EntityPresenter` node (`simulation/entity_presenter.cpp` — the placed walk
  and the shared legs; `entity_presenter_wire.cpp` — the wire walk's cold path
  and hot rows) renders both the placed rows and the wire rows for live joiners.
  Nothing may grow another entity reducer or presenter. `NovaWorldClient` is matchmaking/handoff, not a
  gameplay-replication stack.
