# ADR 0021 - Avatars.def writer policy

Status: accepted. Gates the `Avatars.def` save path (`libs/avatars` writer +
the planned `NovaAvatarDatabase.save_to_path` / ONED Avatars workspace).
Witnessed behavior is in [docs/playerinfo/avatars-re.md](../playerinfo/avatars-re.md);
addresses are Jointops.exe retail (imagebase 0x400000).

## Context

`Avatars.def` is a hand-authored ASCII config (see the retail fixture
`fixtures/avatars/Avatars.def`): `////` comment banners, tab-aligned columns,
blank-line grouping, and trailing demo-build flags (`skipdemo`) on nationality
and division lines. The original engine has **no writer** — it only parses the
file ([orig: CAvatarDefs_ParseConfigLine @ 0x57a3f0]) and denormalizes the
resolved data into runtime structs, discarding the authored part references and
all formatting. The `libs/def` family it sits beside is likewise parse-only.

The OpenNova editor must *author and save* `Avatars.def` (a maintainer
decision). That forces a writer where the original has none, and raises the
round-trip question: what does "faithful" mean for a writer the original never
had, over a format whose retail bytes are full of comment art?

[ADR 0003](0003-no-raw-passthrough-create-from-scratch.md) forbids carrying raw
original bytes through a writer to manufacture a parity match. So byte-exact
reproduction of the hand-authored retail file — which would require preserving
its comments and exact whitespace verbatim — is off the table by policy, and is
also not meaningful for an editor that normalizes layout as the artist adds and
removes entries.

## Decision

`libs/avatars` `avatars_write` emits a **canonical `Avatars.def` from scratch**
from the in-memory model:

- **From-scratch, deterministic output.** One canonical formatting (a fixed
  indent and field order); no raw byte passthrough. The model is a faithful
  *superset* of what the parser reads — it additionally retains the authored id
  tokens (`raw_id`, e.g. `N00`/`D00`/`001`), the trailing flags (`flags`, e.g.
  `skipdemo`), the combo's head/body/arms *reference names* (which the original
  drops — see D-PLAYERINFO-4), and any unrecognized in-block lines (`raw_lines`)
  — so nothing semantic is lost across a load/save.
- **Comments and decorative whitespace are not preserved.** The retail file's
  `////` banners and tab art are authoring decoration, not data; the canonical
  writer does not reproduce them.

## Round-trip contract

Not "writer output equals the hand-authored retail bytes." Instead, two
properties, both pinned by `tests/avatars/avatars_roundtrip_test.cpp`:

1. **Lossless over the model** — `parse -> write -> parse` yields an equal model
   (same parts/combos/nationalities/divisions and their fields).
2. **Idempotent** — `parse -> write -> parse -> write` is **byte-identical on
   the second write**. Once a file is in canonical form, saving it again changes
   nothing.

A from-scratch construction case (build a model in code, write, re-parse, verify
fields) proves the writer does not depend on parsed input.

## Consequences

- **Editor saves are clean and stable**: re-saving an unchanged avatar set is a
  no-op at the byte level; diffs reflect only real edits.
- **First save of a retail file reformats it** (comments dropped, layout
  canonicalized). This is expected for an authoring tool and is the
  ADR-0003-compliant alternative to raw passthrough. Teams that want to keep the
  original retail file pristine should author into a copy.
- **The semantic superset is the parity guarantee**, not the byte stream:
  `avatars-re.md` D-PLAYERINFO-4 documents why combos keep reference names the
  runtime discards, which is exactly what makes the lossless round-trip possible.
