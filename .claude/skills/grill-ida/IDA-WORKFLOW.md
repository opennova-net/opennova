# IDA Workflow

How to drive the `ida-pro-mcp` tools during a grill or engine-research session: connect and
verify, anchor each reimpl↔original pairing with evidence, pull and understand an original, and
write names/types/comments back **safely** and inline.

The governing rule: **read freely, write carefully.** Reading the binary never hurts; a wrong
write can silently corrupt months of curation (a bad `set_type` rewrites every caller's
decompilation). So writes are gated on confidence, and the slow `idb_save` is batched to checkpoints.

---

## 1. Connect and verify

Call order, every session, before touching anything:

1. `server_health` — is the MCP server up and an IDB loaded?
2. `list_instances` — which IDBs are open, on which ports, and which is `active`.
3. `select_instance` — route to the one for this system. Routing is **sticky**: every later call
   goes to the selected instance until you switch. `select_instance(port=0)` resets to the default.
4. `survey_binary` — the documented FIRST analysis call. Returns md5, segments, imagebase, entry
   points, top strings/functions, imports — use it for both connectivity and triage; don't fan out
   to `list_funcs`/`imports`/`find_regex` for that. Use `detail_level:"minimal"` for big binaries
   (>10k functions — `Jointops.exe` is one).

**Verify the binary identity** against the repo pin in `docs/correspondence.md`: retail
`Jointops.exe`, imagebase `0x400000`, IDB `Jointops.exe.kong.i64`. Every address in `docs/` is
absolute in that image. If they don't match — a demo or another title's IDB is active, or the IDB
was rebuilt — **stop and say so**; demo/retail address drift silently invalidates every citation.
(Wrong instance: `list_instances` → `select_instance`. Rebuilt IDB: [LIFECYCLE.md](LIFECYCLE.md) §3.)
When a session legitimately targets a different binary (`dfx2med.exe`, `ModSuperOed.exe`,
`dfvas.exe` — see `docs/engine-primer.md`), say which image you are on and never mix addresses
from two images in one note.

**Troubleshooting**

- `server_health` fails or times out → the ida-pro-mcp plugin isn't running; start it in IDA, retry.
- md5/filename mismatch → the wrong IDB is active (e.g. a demo-build IDB open next to the retail
  one): `list_instances`, then `select_instance` to the right port. (Addresses that mismatch on the
  *right* binary mean a rebuilt IDB — see LIFECYCLE.md.)
- Connection refused with IDA open → check the MCP server port and firewall before suspecting the IDB.

---

## 2. Anchor correspondence (do this before grilling, and before any write)

Each reimpl symbol must be paired to an original with a stated **method** and **confidence**. Never
write to IDA or the source while a pairing is still a guess.

Precedence (strongest first):

1. **Shared string / literal** — `search_text` / `find_regex` for an error message, magic, or
   format tag the system uses (the `"Too many"` mission-loader strings, `'LWF1'`, `'DLG0'`), then
   `xrefs_to` the hit to land in the function. → **anchored**.
2. **Unique numeric constant / fixed-point scale** — `find_bytes` / `find_regex` for a distinctive
   immediate (`0x2B0749C1`, the sound-selection RNG seed; a Q16.16 threshold; a CRC polynomial).
   → **anchored / probable**.
3. **Call-graph propagation** — from an already-anchored node, walk `callees` / `callgraph` /
   `xrefs_to` to its neighbours and match them positionally against the reimpl call tree.
   → **probable**.
4. **Structural + globals cluster** — 2+ *related* functions (loader + validator, an init family)
   that read the same globals and agree on struct field offsets/sizes, matched as a cluster against
   the reimpl call tree. Arg counts alone stay weak — the cluster is what lifts it. → **probable**.
5. **Byte signature** — `make_signature_for_function` (arg is **`addrs`**, plural) to mint a
   rebuild-stable signature; useful to re-locate the function after an IDB rebuild and to spot the
   same routine across game variants (JO retail vs demo, DFX). → corroboration.
6. **Structural fingerprint** — arg count, struct sizes, branch count only. → **guessed**.

Record the method in the row's `evidence` column (drafted in session notes, landed into
`docs/correspondence.md` via the `re-doc` skill). **Guessed** pairings stay read-only and end as
verdict `unknown`, never `divergent`.

For a multi-function system, shape the anchoring in one call: `analyze_component(addrs=[…])` returns
per-function summaries, the internal call graph, and shared data for the whole candidate set.
Compare that internal call graph against the reimpl's call tree — agreement moves pairings
guessed → probable before a single write happens.

---

## 3. Pull and understand an original

- Default: `analyze_function(addr)` — compact one-call view (pseudocode + strings + constants +
  callers + callees + xrefs + basic blocks). Token-efficient; the right first look.
- If the address **isn't a defined function** (common after a string xref lands mid-blob),
  `define_func` first — IDA infers bounds, or pass `start:end` — *then* `analyze_function` /
  `decompile`.
- `decompile(addr)` for full pseudocode; `disasm(addr, …)` for exact instructions/bytes when the
  decompiler is wrong or you need encodings.
- `read_struct` / `type_inspect` for layout; `xrefs_to` / `trace_data_flow` to see who touches a
  global or field. After `read_struct`, `xrefs_to_field(struct, field)` lists every read/write site
  of one field — the direct offset/width/sign check against the reimpl (validators read many fields
  at once; `xrefs_to_field` pinpoints one).
- `insn_query(func=…, mnem=…)` for scoped control-flow checks — every jump or call in a function,
  with operand filters; pairs with `disasm` when the exact encoding matters.
- **Mine validation / integrity functions** — they enumerate field bounds, sentinels, and sizes,
  i.e. they are free struct documentation. Read them early. Find them among the callees of an
  anchored loader (`callees`) or by name pattern (`_validate` / `_check` / `_verify`); each bounds
  check is a struct-table row: `if (hdr->lods > 4) fail` ⇒ `0x14  u32  LOD count (≤4)  @ 0xADDR`.
- **Mine data tables** — behavior hides in data as often as in code. `find_bytes` for a table's
  magic/header, `xrefs_to` the table base for its indexers, `read_struct` for the layout.
- **Wide strings** — menu/UI literals are UTF-16 (`docs/correspondence.md` §2 rows say "wide …
  literals"); `search_text` can miss them. `find_bytes` the interleaved-`00` byte pattern, or
  re-anchor via a byte signature.
- **Stale pseudocode after a retype** — Hex-Rays caches decompilation; after `set_type` /
  `declare_type` the target refreshes but **callers** may serve pre-retype views. Re-`decompile`
  anything you quote after a type edit; never diff against a cached view.
- **Never convert number bases by hand** — use `int_convert` for any hex/dec/bin conversion. A
  mis-typed conversion silently poisons every downstream offset, constant, and citation. Pull all
  addresses/offsets/constants from the tools, not from memory, and derive conclusions from the
  binary, never from the existing (often wrong) names and comments.
- **When the decompiler is wrong** — odd pseudocode (a bare `while (count--) *dst++ = *src++;`, an
  impossible parameter) → `disasm` the range. `rep movsb` / `rep stosb` are inlined memcpy/memset;
  MSVC fastcall passes args in ECX/EDX and Hex-Rays sometimes misreads a prologue. These are
  compiler artifacts, not reimpl divergences — record them as such, don't grill them.

---

## 4. Write back inline — with the confidence gate

**Shared-state rule (this repo).** `Jointops.exe.kong.i64` is the maintainer's curated IDB.
Auto-names (`sub_`, `dword_`, `loc_`) rename directly at **anchored** confidence, each announced as
an audit line (§6). Changing any **human-curated name**, and any **probable**-confidence type edit,
is a **proposal** — in conversation when interactive, listed in the session's final report when not
— applied only on the user's OK. Every applied change is an audit line (§6) and lands in the RE
record's "IDB changes made during the session" section (shape: `docs/audio/mus-sbf-re.md`).

Enriching IDA is half the point of the session — a grilled system should leave the IDB fully named
and typed (functions, locals/stack, globals, signatures, structs, enums). Apply edits the moment
understanding crystallizes (don't batch the *understanding*), and announce each one (§6). Gate by
confidence:

**`rename`** (batch: `func` / `data` / `local` / `stack`)
- Always `dry_run:true` first when touching anything you didn't author this session; set
  `stop_on_error:true`.
- **Never** pass `allow_overwrite` without the user's explicit OK — clobbering a curated name is
  the main way to lose work.
- Match the IDB's existing `Subsystem_Action` naming style (`Mission_LoadBMSFile`,
  `SoundBank_OpenFile`).

**`set_type` / `declare_type`** — *no dry-run exists.* A wrong `set_type` silently rewrites the
decompilation of every caller.
- **anchored** → apply (announced). Multiple anchored edits batch through
  `type_apply_batch(edits=[…], stop_on_error=true)`, announced as one grouped audit line.
- **probable** → propose the exact signature first; apply on OK via
  `diff_before_after(addr, action='set_type', action_args={type})` — it applies the edit *and*
  returns the before/after decompilation in one call. Inspect the after-view; if it degraded,
  revert by re-applying the previous type. (Also good for spot-checking renames mid-batch.)
- **unsure** → prefer a reversible `set_comments` over a type edit. `declare_type` to build up named
  structs in the local type library, then `set_type` to apply them.
- **Flag/enum fields** — before typing one, `type_query` for an existing enum; if absent,
  `enum_upsert` it (idempotent; `bitfield:true` for flags) with member values mined from the
  validation functions, *then* apply the type.

**`set_comments` / `append_comments`** — reversible and harmless; use freely to record findings and
the reverse cross-link (§5). `append_comments` dedupes by default.

**`infer_types`** — never auto-run during grilling; it mass-mutates. Only on explicit request.

**Never during grilling** — `patch`, `patch_asm`, `put_int`, `undefine`, `define_code`,
`delete_stack`. These mutate bytes or destroy definitions; a grilling session only *reads* the
binary and *annotates* the IDB. If something looks byte-wrong in the image, stop and tell the user.
(`define_func` on undefined code is fine — that's analysis, not mutation.)

---

## 5. The three-way cross-link (on confirmed correspondence)

Every confirmed pairing gets linked three ways, anchored on the **address** (names drift, addresses
don't):

1. **The source** — a back-reference marker above the definition, address-first:
   `// [orig: Scr_DecryptBuffer @ 0x53D090]` (GDScript: `##`; doc-comment `/** … */` above a
   public API; one marker per definition).
2. **IDA** — a `set_comments` on the original's **entry address** pointing back:
   `reimpl: opennova::scr::scr_decrypt @ libs/scr/src/scr.cpp`.
   Keep the reverse link entry-only; use additional `set_comments` at inner addresses solely to
   annotate a specific divergence at its site.
3. **The row** — a correspondence row drafted in the session notes in the
   `docs/correspondence.md` column shape
   (`reimpl symbol (file) | original | addr | signature / role | evidence | status`), landed via
   the `re-doc` skill. Data tables, inlined fragments (`[merged-into: 0x…]`), reimpl-only code
   ("new code — no original"), and confirm-only originals follow the existing rows' style.

---

## 6. Announce every edit (one line each — the session is an audit trail)

Format: `target ← tool addr old→new (confidence: reason)`. Examples:

```
IDA   ← rename 0x40e250  sub_40E250 → BMS_ValidateRecordCounts   (anchored: "Too many" strings @ 0x40e326)
IDA   ← set_type 0x53d090  int(uint8_t*, int)                    [probable — confirm before apply?]
IDA   ← comment 0x53d090  reimpl: opennova::scr::scr_decrypt @ libs/scr/src/scr.cpp
code  ← marker  libs/scr/src/scr.cpp:41  // [orig: Scr_DecryptBuffer @ 0x53D090]
notes ← row     scr_decrypt ↔ Scr_DecryptBuffer @ 0x53D090  status=matching
```

At session end, close the trail with a final report:

```
code ← N markers inserted
IDA  ← R renames, T types, C comments   idb_save (final)
docs ← /re-doc landed: record (verdict, divergences M) + correspondence rows (+N) + index sync
```

---

## 7. Save cadence

`idb_save` is the only durability mechanism and the slow op on a large engine binary, so checkpoint
rather than save-per-edit. The checkpoints are mandatory, not a suggestion — a session that saves
only once at the end is one crash away from losing every rename:

- after correspondence for a system is established,
- after each confirmed function's batch of renames/types (confirmed = pairing anchored/probable and
  its axes grilled),
- at session end / before writing the verdict.

**Not** after every comment — the *edits* stay inline, the *saves* are checkpointed.

---

## 8. Confidence ladder (write rules per level)

The vocabulary and the core rule (guessed = read-only, verdict `unknown` never `divergent`) live in
SKILL.md; this table is the per-level write detail. The shared-state rule (§4) applies on top at
every level: curated-name changes are always proposals.

| Confidence | Correspondence basis | Writes allowed |
|---|---|---|
| **anchored** | shared string/constant, or signature match | rename (dry-run first), set_type / type_apply_batch, comments, marker, notes row |
| **probable** | call-graph position, structural + globals cluster | rename (dry-run first); **propose** type edits, verify via diff_before_after; comments, marker, notes row |
| **guessed** | weak structural fingerprint only | **read-only**; no writes; verdict `unknown` |
