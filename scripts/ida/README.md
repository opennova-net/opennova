# scripts/ida — maintainer tools that need a live IDA

These scripts talk to the `ida-pro-mcp` plugin over plain HTTP JSON-RPC
(`http://127.0.0.1:13337/mcp`, override with `--url`) with the retail
`Jointops.exe.kong.i64` loaded. They are not CI lints: CI has no IDA. Run them from
the repo root before a release, when resuming after a long gap, and at the end of a
post-merge tidy round.

| Script | What it checks |
|---|---|
| `cite_sweep.py` | The three-way link (`.claude/skills/grill-ida/LIFECYCLE.md` §4): every `[orig: Name @ 0xADDR]` marker (the one citation form since ADR 0042 d7; the sweep still matches the retired `(retail: Name @0xADDR` spelling so a reappearance surfaces as drift) under `engine/`, `godot/src`, `godot/game`, `godot/probes`, `godot/shaders`, `godot/tests`, `apps/`, `tests/`, `assets/`, `fixtures/` and `docs/` (sources, shaders, the engine-side `.md` records, CMake lists, menu files; a marker wrapped onto comment continuation lines counts, up to the line that closes its bracket) is joined against the live IDB by address and classified (ok / inner-site / call-site / short-form / site-only / other-image, or a defect: name-mismatch, name-elsewhere, unknown-name, code-autoname, idb-autoname, undefined, malformed, unqualified-image, image-spelling); a name matches in its MSVC-demangled and undecorated forms too (`D3DXTex::CImage::LoadDDS`, `D3DXCreateCubeTextureFromFileInMemory`), and a CamelCase word with no underscore that the IDB does not know reads as a label (a site cite); the reverse leg joins every IDB `reimpl:` entry comment against the tree: `reimpl-stale` (the path is gone), `reimpl-moved` (the path exists but neither it nor its `.h`/`.cpp` sibling still carries a marker inside the function, another file does), `reimpl-orphan` (nothing cites the function; a line that says so - `unported`, `retired`, `no marker` - is accepted). Exit 1 on any defect; `--tsv` keeps the full join for adjudication. `--fix-reimpl` plans the reverse-link rewrites whose new home is unambiguous (a retired path follows its `nova_` rename twin when the twin still cites the function, else the single citing file, else the single engine/ citing file, else - no engine/ file citing it - the single godot/src one; a link that says `device leg` is accepted as documented), `--apply` writes them into the IDB and `--save` runs `idb_save`; orphans and ambiguous links are listed for a hand rewrite. Runs from the repo root; IDA must be on this machine (bulk replies ride through a temp file). |

Rules the sweep encodes (from `docs/README.md` and the grill-ida skill): the address is
the join key, names drift legitimately and the IDB body decides; a cite of another image
carries the image in its marker, by its file name (one spelling per image: `gobj.dll`,
`JOTACmed.exe`, `ModSuperOed.exe`, `UPDATE.EXE`, `REVUPDAT.EXE`, `pack.exe`; the records'
established short forms `(dfbhd)`, `(jodemo)`, `TrnGen`, `dfx2med` still read), right
after the address (`@0x1 (dfx2.exe)`, `@0x1, misldr.dll`), before its name
(`JOTACmed.exe sub_44E640 @0x44e640`) or as the marker's qualifier (`[orig: A @0x1, B @0x2
(dfx2.exe)]`); a prose mention inside a marker (`(NWJoin.dll PUB* cookies)`) qualifies
nothing. `scripts/lint/cite_images.py` holds the image table (it mirrors
`docs/engine-primer.md` section 2, and the sweep prints any image one lists and the other
lacks) and the rule, which `scripts/lint/cite_census.py` shares to key its census by image
and address. A record whose preamble makes another image its default is listed in the
sweep's `DOC_DEFAULT_IMAGE`. A site inside a function is written `Func @0xSITE` and a call
site `Callee @0xCALLSITE`; auto-names (`sub_`, `loc_`, `dword_`) never appear in a marker
once the IDB has a real name.
