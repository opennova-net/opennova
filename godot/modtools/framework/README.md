# Editor framework

Shared, workspace-agnostic building blocks for the OpenNova Editor's shell and
inspectors. Everything here is editor-only (lives under `modtools/`); shell-neutral
UI primitives (FieldBinder, SyncGuard, UiBox) live in `godot/engine/ui/` instead.

## Drag-as-link: the LinkPayload contract

`links/link_payload.gd` is the one drag-data shape for resource references.
Any control that drags a reference packs a `LinkPayload`; any drop target
unpacks with `LinkPayload.from_drag_data(data)` (returns `null` when the
dragged data is not ours — foreign drags can never half-match).

Wire shape (`to_drag_data()`):

```gdscript
{"type": "opennova/resource_ref", "kind": ..., "name": ..., "path": ...}
```

Field semantics:

- **kind** — the reference-kind vocabulary the extractors and
  `NovaReferenceIndex` speak (`terrain`, `texture`, `font`, `object_model`,
  `strings`, `string_id`, ...). This is NOT the workspace-jump vocabulary;
  jump translation (`object_model` → `object`) happens at jump time via the
  shared `ResourceKinds.jump_kind()` (`resource_kinds.gd`), never inside a
  payload.
- **name** — the identity a drop target commits: the file name (with
  extension) for file kinds, the bare name for header references, the key for
  `string_id`. Drop targets route it through their own `value_from_path`
  normalizer, so a drop commits exactly what a browse pick would.
- **path** — advisory location only (tooltip/jump convenience). For
  `string_id` payloads it may carry the KEY, not a file path (the
  StringRefWidget jump-gate backfill) — never trust it as a location.

Sources:

- `ResourceRefWidget`'s badge (and the row itself) — drags the current value.
- The Resource Browser pane's rows — `ResourceTable.enable_drag_source(provider)`
  is opt-in; the persistent pane enables it, the MODAL picker never does, so a
  dialog row cannot start a system drag out from under its input grab.

Targets:

- `ResourceRefWidget` (and `TextureRefWidget`, which delegates): accepts a
  payload whose kind matches the widget's configured kind exactly, plus the
  `texture`/`image` spelling equivalence. Empty kinds never match (a kindless
  payload is malformed, not a wildcard). The committed value rides the same
  `_commit` path as picks: one `value_changed` emission, silent on same-value.
  Plain `String` drops are NOT payload drops: the name field defers them to
  LineEdit's native caret insert (committing on Enter/focus-out like typing)
  — LineEdit runs the forwarded drop AND its native insert for String data,
  so a forwarded String handler would double-apply. The other drop surfaces
  reject Strings outright.
- `StringRefWidget` deliberately adds nothing: its inner row is configured
  kind `string_id`, which the pane never produces, so file drops are rejected
  by kind matching alone. Per-key drops between string widgets work through
  the inherited handler and re-resolve against the target's own table.
