# MNS stylesheets parse into a lossless document; the flat table is its flatten() view

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

The runtime keeps its flat view: `Document::flatten()` evaluates conditionals, joins
continuations, strips comments, unescapes, uppercases keys, last duplicate wins -
exactly the historical `mns::parse()` result, which now delegates to it so one
tokenizer exists. This is the same preserve-vs-honor split as ADR 0002: the document
preserves what was authored; flatten is what the engine honors.

## Decisions worth recording

- **Edits are minimal-delta.** `set_value` replaces only the value chunk: the name,
  alignment tabs, inline comment, and EOL of the line survive, so a one-value edit
  is a one-line diff. Values are logical (backslashes re-escape to `\\` on render)
  and get whitespace-trimmed - the format cannot represent leading/trailing value
  whitespace or embedded `//`/newlines, and `is_valid_value` rejects the latter.
- **Multi-line defines collapse on edit.** Editing a continuation-spanning value
  rewrites it as one line keeping the first line's layout and coalescing every
  spanned inline comment into one trailing comment: comment *text* is never lost,
  comment *position* is approximated. Zero shipped defines use continuations; the
  policy is pinned by test.
- **Inactive `#if 0` content stays text.** The runtime skips those lines one by one
  (directives still recognized, continuations not honored), so imposing define
  structure there would change flatten semantics. They are preserved verbatim as
  inactive-text nodes, visible through the source view, excluded from entries().
  The alternative - speculatively parsing them as defines - produces bogus entries
  for continuation tails and was rejected.
- **Diagnostics, never failures.** The in-file spec declares errors (duplicate
  names, bare backslashes, malformed `#if`); the original only reported them in
  debug builds. The parse stays permissive so every shipped file loads, and emits
  `{line, severity, code, message}` diagnostics the editor surfaces
  (D-MNS-1..4 in docs/mnu/menu-re.md).
- **From-scratch documents are canonical.** A sheet built programmatically (or via
  `set_variables`) renders insertion-ordered `NAME<TAB>value` lines with CRLF (the
  ship-faithful EOL) and no BOM; a parsed document keeps whatever it had, per line.

## Consequences

- `MnsStyleSheet.to_byte_array()/save_to_path()` became lossless; the ONED Menu
  Styles workspace's keystone test opens the shipped stylesheet and saves it
  byte-identically, and its undo restores comments and alignment exactly (undo
  snapshots are source text).
- The flat `mns::write()` dump remains as the documented lossy canonical form.
- Anything consuming `mns::StyleSheet` is untouched - flatten() reproduces the
  legacy parse bit-for-bit (guarded by `tests/mns/mns_document_test.cpp`).
