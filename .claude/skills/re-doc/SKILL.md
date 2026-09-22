---
name: re-doc
description: Authors or refreshes a reverse-engineering record under docs/<domain>/<system>-re.md with verdicts, cited witnesses, and stable divergence IDs. Use after a grill-ida or engine-research session to land findings in the owning record and update affected indexes.
---

# Land a golden RE record

Inputs: the grilled system, and the evidence — the grill-ida/engine-research
session's findings, code comments citing D-IDs, and the ctests that pin
behavior. Read `docs/README.md` (conventions + index) and the
richest exemplar `docs/audio/mus-sbf-re.md` before writing. ADR exemplar, for
when a policy decision crystallised: `docs/adr/0008-pff-writer-policy.md`.

## Format contract

- Title: `# <SYSTEM> — reverse-engineering record`. The preamble names the
  implementing code (`engine/<x>`, `godot/src/<x>`), the binary (retail
  Jointops.exe unless stated) and the IDB; state that addresses are that
  binary's.
- Verdict table first: `Component | Verdict | Evidence`. Verdict vocabulary in
  use: MATCHING, MATCHING (behavioral proof), MATCHING (read-only grill),
  host code / not grillable, plus unlanded/divergent states. Evidence names
  citation counts and concrete ctest names.
- Witness map: per-function findings; every behavioral claim cites
  `[orig: Name @ 0xADDR]`. Note IDA renames made during the session.
- Divergence catalog `D-<DOMAIN>-n`: table of `ID | Ours | Original | Why /
  consequence` (or `ID | Status | Summary` for fix logs). IDs are STABLE —
  never renumber; document gaps rather than closing them.
- Hard rule: no raw decompiled code is ever committed — summarize and cite.

## Cross-file updates

1. `docs/divergence-ledger.md` — close/open the D-rows in the same PR;
   `python scripts/lint/ledger_check.py --write` regenerates the scoreboard.
   A D-id born in the ledger must ALSO get its full row in the record's
   catalog — the ledger mirrors, the record owns.
2. `docs/correspondence.md` — update only when the findings change a function
   correspondence or its verdict.
3. `docs/README.md` — add or rename a record link only when the set or names
   of records change. Update `docs/engine-primer.md` only when its subsystem
   map changes, and `docs/current-state.md` only when its routing or phase changes. Dated
   progress belongs in the owning record, not these navigation pages.
4. Code↔doc sync — source comments cite `docs/<domain>/<x>-re.md (D-...)`;
   grep the repo for the doc's D-IDs and confirm every cited ID exists in the
   doc and vice versa.
5. New ADR if a policy decision emerged: next number in `docs/adr/`, linked
   from the docs README ADR table.
6. Open questions survive only as the record's explicit unknown/follow-up
   entries — there is no scratch directory; what isn't landed is lost. The
   record stays pristine: the best current understanding, no drafts, no raw
   decompilation, no session chatter.

## Verify before declaring done

- Every ctest named in Evidence exists:
  `ctest --test-dir build -C Release -N -R <name>` (build first if needed:
  `bash scripts/build.sh --no-godot`).
- Every cited repo path exists; every address is in `@ 0x...` form.
- The doc reads standalone: a future session must be able to re-grill from it
  without the original transcript.

Done = doc landed in the format above, the cross-file updates applied, and the
evidence tests pass. This skill is the landing half: the project `grill-ida`
skill produces verification evidence and inline source/IDA fixes,
`engine-research` produces original-engine findings; `re-doc`
defines what the committed record must contain. End grill sessions by invoking
this skill.
