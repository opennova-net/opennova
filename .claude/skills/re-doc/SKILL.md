---
name: re-doc
description: Authors or refreshes a golden reverse-engineering record under docs/<domain>/<system>-re.md in this repo's exact format — verdict table, witness map with [orig] citations, stable D-<DOMAIN>-n divergence catalog, and the cross-file index updates. Use after a grill/IDA verification session, or when graduating RE findings from untracked notes/ into tracked docs.
---

# Land a golden RE record

Inputs: the grilled system, and the evidence — a grill-ida transcript or
untracked `notes/<domain>/*.md` scratch, code comments citing D-IDs, and the
ctests that pin behavior. Read `docs/README.md` (conventions + index) and the
richest exemplar `docs/audio/mus-sbf-re.md` before writing. ADR exemplar, for
when a policy decision crystallised: `docs/adr/0008-pff-writer-policy.md`.

## Format contract

- Title: `# <SYSTEM> — reverse-engineering record`. The preamble names the
  implementing code (`libs/<x>`, `godot/engine/<x>`), the binary (retail
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

## Cross-file updates (the part that gets forgotten)

1. `docs/README.md` — add/refresh the "RE records by domain" row and status
   (landed / unlanded with PR reference / in flight).
2. `docs/correspondence.md` — add the grilled functions to the parity matrix
   with verdicts.
3. Code↔doc sync — source comments cite `docs/<domain>/<x>-re.md (D-...)`;
   grep the repo for the doc's D-IDs and confirm every cited ID exists in the
   doc and vice versa.
4. New ADR if a policy decision emerged: next number in `docs/adr/`, linked
   from the docs README ADR table.
5. `notes/<domain>/` scratch that graduated here can be pruned; `notes/` is
   untracked and is never committed either way.

## Verify before declaring done

- Every ctest named in Evidence exists:
  `ctest --test-dir build -C Release -N -R <name>` (build first if needed:
  `BUILD_GODOT=0 bash scripts/build.sh`).
- Every cited repo path exists; every address is in `@ 0x...` form.
- The doc reads standalone: a future session must be able to re-grill from it
  without the original transcript.

Done = doc landed in the format above, the cross-file updates applied, and the
evidence tests pass. This skill is the landing half of a grill: the user-level
`grill-ida` skill produces the evidence and inline source/IDA fixes; `re-doc`
defines what the committed record must contain. End grill sessions by invoking
this skill.
