# scripts/ida — maintainer tools that need a live IDA

These scripts talk to the `ida-pro-mcp` plugin over plain HTTP JSON-RPC
(`http://127.0.0.1:13337/mcp`, override with `IDA_MCP_URL`) with the retail
`Jointops.exe.kong.i64` loaded. They are not CI lints: CI has no IDA. Run them from
the repo root before a release, when resuming after a long gap, and at the end of a
post-merge tidy round.

| Script | What it checks |
|---|---|
| `cite_sweep.py` | The three-way link (`.claude/skills/grill-ida/LIFECYCLE.md` §4): every `[orig: Name @ 0xADDR]` / `(retail: Name @0xADDR` marker under `engine/`, `godot/src`, `godot/game`, `godot/modtools`, `apps/` and `docs/` is joined against the live IDB by address and classified (ok / inner-site / call-site / site-only / other-image, or a defect: name-mismatch, name-elsewhere, unknown-name, code-autoname, idb-autoname, undefined, malformed, unqualified-image); the reverse leg lists IDB `reimpl:` entry comments whose repo path no longer exists. Exit 1 on any defect; `--tsv` keeps the full join for adjudication. |

Rules the sweep encodes (from `docs/README.md` and the grill-ida skill): the address is
the join key, names drift legitimately and the IDB body decides; a cite of another image
(`dfx2med.exe`, `ModSuperOed.exe`, `misldr.dll`, `binkw32.dll`, `jodemo`) carries the
image name in the bracket; a site inside a function is written `Func @0xSITE` and a call
site `Callee @0xCALLSITE`; auto-names (`sub_`, `loc_`, `dword_`) never appear in a marker
once the IDB has a real name.
