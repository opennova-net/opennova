# Round-trip preserves every authored attribute, even ones the runtime ignores

Loading a shipped `.mnu` and re-saving it must be lossless. The MNU model therefore parses and serializes every attribute a real menu uses — including ones the runtime does not (yet) act on, such as `SB_EDGE_PAD`, `MINVAL`/`MAXVAL`, `MAXCHAR`, and the `NUMBER` flag. Round-trip fidelity is a first-class goal, independent of runtime behavior.

This splits "support" into two layers: the model **preserves** the full set of authored attributes, while the runtime **honors** only the display- and critical-path-relevant subset. The corpus harness asserts the preserve layer directly.

## Why not just model what we render

An idempotence round-trip test (parse → serialize → parse, then compare) is blind to an attribute dropped at parse time: it is absent on both sides, so the test stays green while data is lost. We instead assert attribute-key coverage between the original bytes and the serialized output, which catches silent drops. Carrying inert fields in the model is the deliberate cost of never silently mangling an artist's menu on save.
