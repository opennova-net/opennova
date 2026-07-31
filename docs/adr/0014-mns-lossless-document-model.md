# MNS stylesheets parse into a lossless document; runtime uses its evaluated view

The `.mns` menu stylesheet is a hand-authored text file. The only one NovaLogic
shipped (`menu_style.mns`, vendored at `fixtures/mns/menu_style.mns`) opens with a
38-line comment header that is the format's own specification, groups its defines
with blank lines and section comments, aligns values with tab runs, and ends without
a final newline. The original support parsed it straight into a flat
name -> value map, and "writing" was a sorted dump of that map: opening the shipped
file in an editor and saving would have destroyed every one of those authored
properties. That fails the project's round-trip bar (the MNU/RTXT byte-stable
standard, [ADR 0002](0002-mnu-round-trip-preserves-superset.md)).

`mns::Document` (libs/mns/include/mns/mns_document.h) is now the source of truth: an
ordered node list (blank / comment / directive / define / inactive-text) whose typed
fields **exactly partition the input bytes** - leading whitespace, the name in its
authored case, the alignment run, the value chunk with escapes intact, the inline
comment, the per-line EOL. Serialization renders from those fields, so byte-identity
for an untouched document is a consequence of complete modeling, not a stored copy
of the input. This is the ADR-0003-compatible reading of losslessness: there is no
raw-span replay; every byte is owned by a field something can edit
([ADR 0003](0003-no-raw-passthrough-create-from-scratch.md)).

The runtime keeps a separate evaluated view:
`Document::evaluate()` returns `{StyleSheet, diagnostics, success}` using the
witnessed retail rules, while `flatten()` is the convenience for callers that
deliberately accept diagnostics. `mns::parse()` delegates to the evaluator and
propagates retail syntax failure. This is the same preserve-vs-honor split as
ADR 0002: the document preserves what was authored; evaluation is what the
engine honors.

## Decisions worth recording

- **Edits are minimal-delta.** `set_value` replaces only the value chunk: the name,
  alignment tabs, inline comment, and EOL of the line survive, so a one-value edit
  is a one-line diff. Values are logical (backslashes re-escape to `\\` on render)
  and get whitespace-trimmed - the format cannot represent leading/trailing value
  whitespace or embedded `//`/newlines, and `is_valid_value` rejects the latter.
- **Multi-line defines collapse on edit.** Editing a continuation-spanning value
  rewrites it as one line keeping the first line's layout and coalescing every
  spanned inline comment into one trailing comment: comment *text* is never lost,
  comment *position* is approximated. If the continuation crosses conditional
  directives or inactive source, those structural lines remain in place and the
  edited value is written across the surviving value segments instead; an edit
  must never delete control flow. Zero shipped defines use continuations; both
  policies are pinned by test.
- **Inactive source stays repairable.** The lossless document retains inactive
  text and directives exactly. Runtime evaluation separately mirrors retail's
  directive scan, including directive recognition during continuations and its
  search for `#` inside an inactive region.
- **Document diagnostics do not block editing; evaluator failures block
  runtime use.** A malformed file can open, display diagnostics, and be repaired
  without data loss. The runtime evaluator returns `success=false` for syntax
  the original rejects, so callers cannot accidentally run a partial sheet.
- **From-scratch documents are canonical.** A sheet built programmatically (or via
  `set_variables`) renders insertion-ordered `NAME<TAB>value` lines with CRLF (the
  ship-faithful EOL) and no BOM; a parsed document keeps whatever it had, per line.

## Consequences

- `MnsStyleSheet.to_byte_array()/save_to_path()` became lossless; the ONED Menu
  Styles workspace's keystone test opens the shipped stylesheet and saves it
  byte-identically, and its undo restores comments and alignment exactly (undo
  snapshots are source text).
- The flat `mns::write()` dump remains as the documented lossy canonical form.
- Existing `mns::StyleSheet` consumers keep their small interface; byte/text
  loaders now receive retail syntax failure from the evaluator instead of a
  partial table.
