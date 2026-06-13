# Lifecycle

Cross-session procedures: resuming an interrupted grill, re-verifying a fixed divergence, surviving
an IDB rebuild, and keeping the three-way link honest over time. Everything here leans on the same
invariant as the formats: the **address is the join key** — markers, IDA comments, and
correspondence rows all carry it.

The stance across sessions is **trust, but verify**: prior grilled work — IDA names and types,
correspondence rows, markers, records — is input, not suspect. Spot-check it instead of
re-deriving it, and ratchet forward: each grill ends with more anchored pairings, more grilled
axes, and better evidence than it started with. Confidence moves only on evidence — in either
direction.

---

## 1. Resume an interrupted grill

A grilling session lands durable state as it goes (the RE record via `re-doc`, source markers,
IDB annotations), so resumption is a read problem, not a recovery problem:

1. Read `docs/<domain>/<system>-re.md` and the system's rows and tables in
   `docs/correspondence.md` — durable session state lives there; a session lands its
   findings before it ends.
2. **Verify the binary first** — the `docs/correspondence.md` header pins (retail `Jointops.exe`,
   imagebase `0x400000`, IDB `Jointops.exe.kong.i64`) against `survey_binary`. A mismatch means a
   rebuilt IDB or the wrong instance: stop and resolve (§3) before trusting any address.
3. Resume from: any row with `status ≠ matching`, plus any row whose `[orig: … @ 0xADDR]` marker is
   missing from the source (`grep -rn "\[orig:" libs/ godot/ apps/`, join by address).
4. Rows that are `unknown` because the pairing was only **guessed** get re-anchored first
   (IDA-WORKFLOW.md §2) — new evidence may have appeared since.
5. Don't re-grill `matching` rows — but spot-check one or two: the anchoring evidence still holds
   at the recorded address, the marker still greps out, the IDA name matches the row. A pass
   transfers trust to the rest; a fail means drift (§4) or a rebuild (§3) — resolve it before
   grilling on top. Settled ground stays settled until the reimpl changes (§2).

---

## 2. Re-verify after a divergence fix

When the reimpl is fixed after a `divergent` verdict, don't re-grill the system:

1. Re-pull the original at the cited address and re-check **only the axis that diverged**.
2. If it now matches: land the flip via a `re-doc` pass — the record's divergence entry gains
   `— resolution: fixed in <commit>` (the `D-<DOMAIN>-n` id is never renumbered), the
   `docs/correspondence.md` row flips `divergent → matching`, and the code↔doc D-ID sync is
   re-checked.
3. Deliberate deviations don't flip: the row stays `divergent`; the record cites the ADR or
   directive (existing styles: "intentional mirror", "accepted as D-MNU-1 (ADR 0005)").
4. A later full grill can confirm stability; a single-axis fix doesn't require one.

---

## 3. Survive an IDB rebuild

Addresses shift when the IDB is rebuilt; byte signatures don't. Detection: the
`docs/correspondence.md` header pins no longer match `survey_binary`, or marker/row addresses stop
resolving in the live IDB (resume step 2 catches both).

1. **Re-anchor everything first, write nothing yet**: for each row, re-locate the original via
   its byte signature (`make_signature_for_function`, minted at confirmation time; mint missing
   ones from the old IDB if it still exists).
2. Then update in one pass, in order: rows and addresses across `docs/` (use the `re-doc` skill's
   cross-file sync as the checklist) → source markers (`[orig: NAME @ 0xOLD]` → `@ 0xNEW`) → IDA
   entry comments (re-set the reverse links). Addresses from the old image never mix with the new
   in one doc — re-pin the header first.
3. `idb_save` once at the end.
4. Re-grep the markers and re-join against the correspondence rows to confirm nothing was missed.

---

## 4. Drift check (the three-way link lint)

Markers, correspondence rows, and IDA comments go stale independently (refactors move files; rows
get edited by hand). Periodically — before a release, or when resuming after a long gap:

1. `grep -rn "\[orig:" libs/ godot/ apps/` — extract address + name from each marker.
2. Join against the `docs/correspondence.md` rows by address.
3. Report, diff-style, for review before fixing: markers without a row, rows without a marker, and
   name mismatches (names drift legitimately — the address decides who's right).
4. The D-ID half is the `re-doc` skill's cross-file step 3: grep `D-<DOMAIN>-` across source vs
   records and confirm both directions resolve.

---

## 5. Upgrading a guessed pairing

New evidence (a shared string found later, a distinctive constant, a signature hit) upgrades
confidence:

- Update the row's `evidence` column as a trail: `fingerprint → string "Too many"`.
- The verdict only changes after the function is actually re-grilled at the new confidence —
  `unknown` never silently becomes `matching`.

---

## 6. Concurrent grilling

`docs/correspondence.md` is shared across systems, and agent work happens in worktrees
(`.claude/worktrees/`). Convention: **one system per worktree**; each session lands its own
rows via `re-doc`, so the conflict surface is the landing PR, not the session. If two
landings race, rows are independent: sort by address and dedupe at merge.
