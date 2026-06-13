---
name: engine-research
description: Answers a new question about how the original engine works. Finds the relevant original functions in IDA via the ida-pro-mcp server — string xrefs, data refs, callgraph walks from known anchors, byte signatures — witnesses the behavior, and lands durable findings with [orig: Name @ 0xADDR] citations into the tracked RE records via the re-doc skill. Use when asking how the original engine does something not yet witnessed, hunting an unknown function, struct, or data table, or when asked to IMPLEMENT an engine feature whose original behavior isn't witnessed yet — "implement the HUD" means research the original HUD (behavior and look) first, then port; never invent one. To verify an existing reimplementation against the binary, use grill-ida instead.
argument-hint: "question (e.g. 'how does the original pick terrain LOD?')"
---

# Answer a question about the original engine

Research, not verification: start from a question, end with witnessed
behavior and citations. There may be no reimpl counterpart, no pairing, and
no verdict — the deliverable is knowledge that survives the session, and it
feeds the project's core rule: we never implement "our own version" of
engine behavior; we port what is witnessed. Tool driving, the IDB write
policy, and troubleshooting are shared with the grill skill: read
`.claude/skills/grill-ida/IDA-WORKFLOW.md` before writing anything to the
IDB. Engine-wide conventions and the full toolbox: `docs/engine-primer.md`.

## 1. Don't re-research the witnessed

State the question in one sentence, plus what would count as an answer.
Then check, in order: `docs/correspondence.md` (the anchor index — named
originals with addresses); the domain's `docs/<domain>/*-re.md`; `[orig:`
markers near the relevant reimpl code (grep `libs/ godot/ apps/`); and
`CONTEXT.md` for vocabulary. If the answer is already recorded, cite it and
stop. Partial hits become the anchors for step 3.

## 2. Preconditions

Requires the optional ida-pro-mcp server (maintainer's machine only). If
`mcp__ida-pro-mcp__*` tools are missing or `server_health` fails: STOP and
report the open question to the user so it is not lost (durable open
questions belong in the RE record's unknown/follow-up entries, via
`re-doc`), and say what to start. When up: `list_instances` →
`select_instance` → `survey_binary` (minimal detail), and confirm the repo
pin from `docs/correspondence.md`: retail `Jointops.exe`, imagebase
`0x400000`, IDB `Jointops.exe.kong.i64`. On any other image (demo, other
title, rebuilt IDB) the addresses in docs/ do not apply — say which image
you are on, and never mix addresses from two images in one note.

## 3. Find the function — the hunt ladder

Strongest lead first; each found site seeds the next rung.

1. **By string** — `search_text` / `find_regex` for an error message,
   format tag, or console string the behavior would touch, then `xrefs_to`
   each hit to land in its function. Menu/UI literals are UTF-16 wide:
   when text search misses, `find_bytes` the interleaved-`00` byte pattern.
2. **By data** — a known global or table (correspondence rows pin many) →
   `xrefs_to` it; or `find_bytes` a magic / table header, `read_struct`
   the layout, then `xrefs_to` the table base to find its indexers.
3. **By callgraph** — walk `callees` / `xrefs_to` / `callgraph` outward
   from an already-witnessed anchor and match candidates by position and
   role.
4. **By constant** — `find_bytes` / `find_regex` for a distinctive
   immediate: a fixed-point scale, CRC polynomial, RNG seed.
5. **Shape the area** — for a multi-function candidate set,
   `analyze_component` returns per-function summaries, the internal call
   graph, and shared data in one call.

A hit landing in an undefined blob means IDA has no function there yet:
`define_func`, then proceed.

## 4. Witness the behavior

`analyze_function` first (compact one-call view), `decompile` for full
pseudocode, `disasm` when the decompiler looks wrong — `rep movs*` is
inlined memcpy, ECX/EDX is MSVC fastcall; compiler artifacts, not engine
logic. `read_struct` / `type_inspect` / `xrefs_to_field` for layout;
`insn_query` for scoped control-flow checks. Mine validation functions
(free struct docs) and data tables (behavior hides in data). Details:
IDA-WORKFLOW.md §3.

Every claim carries a confidence — **anchored / probable / guessed** — and
an address. A guess is promoted by evidence, never by repetition.

## 5. Leave the IDB better — within the shared-state policy

A newly understood `sub_XXXXXX` at anchored confidence gets renamed
(dry_run first; match the IDB's existing `Subsystem_Action` style, e.g.
`Mission_LoadBMSFile`, `SoundBank_OpenFile`) plus a one-line
`set_comments` summary at entry. Changing a human-curated name, or any
probable-confidence type edit, is a proposal first. Full policy:
IDA-WORKFLOW.md §4. `idb_save` if anything was written.

Gotchas: after `set_type` / `declare_type`, Hex-Rays may serve stale cached
pseudocode — re-`decompile` the function and its callers rather than
trusting a view captured before the retype. After renames, re-pull before
quoting names in notes.

## 6. Land the findings

Durable knowledge lands in tracked docs the same session it is witnessed —
there is no scratch directory, and `docs/` stays pristine as the best
current understanding of the original engine:

- Findings shaping code being written now → inline `[orig: …]` citations
  at the port site; the port itself is a faithful structural translation
  of what was witnessed (CLAUDE.md conventions), never a from-scratch
  reinterpretation.
- Durable system knowledge → the `re-doc` skill: a
  `docs/<domain>/<system>-re.md` record and/or `docs/correspondence.md`
  rows. Originals with no reimpl can land as confirm-only rows; open
  threads land as the record's explicit unknown/follow-up entries.
- Spawned implementation work → a TODO.md entry naming the addresses.

Raw decompilation never lands in any file — quote it in the conversation
only (docs/README.md rule).

## Done

The question is answered with addresses — or explicitly unresolved, with
what was ruled out landed as the record's follow-up entries — durable
findings are landed via `re-doc`, any IDB writes are logged and saved, and
the routing (inline cite / record / TODO) is stated.
