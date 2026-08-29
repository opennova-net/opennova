# scripts/ida — maintainer tools that need a live IDA

These scripts talk to the `ida-pro-mcp` plugin over plain HTTP JSON-RPC
(`http://127.0.0.1:13337/mcp`, override with `--url`) with the retail
`Jointops.exe.kong.i64` loaded. They are not CI lints: CI has no IDA. Run them from
the repo root before a release, when resuming after a long gap, and at the end of a
post-merge tidy round.

| Script | What it checks |
|---|---|
| `cite_sweep.py` | The three-way link (`.claude/skills/grill-ida/LIFECYCLE.md` §4): every `[orig: Name @ 0xADDR]` marker (the one citation form since ADR 0042 d7; the sweep still matches the retired `(retail: Name @0xADDR` spelling so a reappearance surfaces as drift) under `engine/`, `godot/src`, `godot/game`, `godot/modtools`, `godot/shaders`, `godot/tests`, `apps/`, `tests/` and `docs/` (sources, shaders, the engine-side `.md` records, CMake lists; a marker wrapped onto comment continuation lines counts) is joined against the live IDB by address and classified (ok / inner-site / call-site / short-form / site-only / other-image, or a defect: name-mismatch, name-elsewhere, unknown-name, code-autoname, idb-autoname, undefined, malformed, unqualified-image); the reverse leg joins every IDB `reimpl:` entry comment against the tree: `reimpl-stale` (the path is gone), `reimpl-moved` (the path exists but neither it nor its `.h`/`.cpp` sibling still carries a marker inside the function, another file does), `reimpl-orphan` (nothing cites the function; a line that says so - `unported`, `retired`, `no marker` - is accepted). Exit 1 on any defect; `--tsv` keeps the full join for adjudication. `--fix-reimpl` plans the reverse-link rewrites whose new home is unambiguous (a retired path follows its `nova_` rename twin, else the single citing file, else the single engine/ citing file for a push-down), `--apply` writes them into the IDB and `--save` runs `idb_save`; orphans and ambiguous links are listed for a hand rewrite. Runs from the repo root; IDA must be on this machine (bulk replies ride through a temp file). |

Rules the sweep encodes (from `docs/README.md` and the grill-ida skill): the address is
the join key, names drift legitimately and the IDB body decides; a cite of another image
(`dfx2med.exe`, `ModSuperOed.exe`, `misldr.dll`, `binkw32.dll`, `jodemo`) carries the
image name in the bracket; a site inside a function is written `Func @0xSITE` and a call
site `Callee @0xCALLSITE`; auto-names (`sub_`, `loc_`, `dword_`) never appear in a marker
once the IDB has a real name.
