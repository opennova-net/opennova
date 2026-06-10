# The editor models every construct; no raw import-to-export passthrough

A menu must be creatable from nothing (no source `.mnu` required), and the editor must be able to represent and author every construct. The hard rule that delivers both: round-trip fidelity is achieved only by modeling each construct as typed, structured data the editor owns — never by stashing raw imported bytes/text and replaying them on save.

So when the coverage harness (ADR 0002) flags a dropped attribute, the fix is always a new typed field with parse + serialize, never a catch-all "unknown data" buffer. `NovaMnuDocument` keeps a parsed `mnu::Document`, mutates it structurally, and re-serializes from structure; `create_empty()` builds a valid document from scratch. A create-from-scratch round-trip test is the live proof that nothing depends on having imported a file.

## Consequences

- A "preserved" attribute is a real typed field with a sensible default, so a from-scratch menu can carry it too — there is no construct that exists only via import.
- Inspector UI may lag the model (a field can round-trip before it has an editing widget), but that is a UI gap, not a passthrough: the data is structured and authorable through the API.
