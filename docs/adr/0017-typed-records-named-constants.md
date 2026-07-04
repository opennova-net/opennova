# Contracts are typed records, not dictionaries; constants are named, not magic

Cross-object contracts in this codebase have drifted into stringly-keyed
Dictionaries: document-tab rows, gizmo state, reference-service bundles,
reference edges, focus payloads, schema rows, MCP arguments. A Dictionary
contract has no schema, no completion, no type errors, and no single place
to read what the shape *is* — every consumer re-learns it from a producer's
comment, and a typo'd key fails silently at runtime. The repo already proved
the alternative twice: `LinkPayload` (a RefCounted record that degrades to a
Dictionary only at the drag-data transport boundary) and
`WorkspaceDef`/`InspectorDef` (typed Resources that explicitly replaced an
untyped dict table).

The same reasoning applies one level down: a bare numeric literal in code is
a contract with no name. In engine ports the number is witnessed and cited
(`[orig: Name @ 0xADDR]`), which is its name; everywhere else it needs one.

## Decisions worth recording

- **New cross-object contracts are typed records.** GDScript: a `RefCounted`
  (or `Resource` when it must serialize) with typed fields, following
  `LinkPayload`/`WorkspaceDef`. C++: a small struct with named members.
  Python: a dataclass. A Dictionary/JSON shape is acceptable only AT a
  transport or serialization boundary (drag data, MCP wire, config files,
  FFI), produced by `to_*()`/`from_*()` on the record — the record is the
  contract, the dict is its encoding.
- **Existing dictionary contracts convert adopt-on-touch**, except the
  seeded conversions the maturity program schedules explicitly
  (document-tab rows, tile gizmo state, reference services, reference
  edges, focus payloads, mission param schema rows, MCP tool defs/args,
  object material defs). No big-bang sweep; a contract you edit is a
  contract you type.
- **No magic numbers.** A literal with meaning gets a `const` with a name
  (and a comment only when the name cannot carry the constraint). Engine
  constants carry their `[orig]` citation as usual — the citation is the
  provenance, the const name is the meaning. Obvious arithmetic identity
  values (0, 1, -1, 2 in index math) are not "magic".
- **Enforcement is a ratchet, not a purge.** A diff-scoped lint flags NEW
  Dictionary-typed public signatures in `godot/modtools/` and
  `godot/engine/` (with an allowlist for sanctioned transport edges) and
  reports new bare literals advisorily; repo-wide counts only ratchet
  against a committed baseline. Soft mode first; hard-fail only after a
  clean wave. The maintainer can bump a baseline, logged in the program
  doc.
