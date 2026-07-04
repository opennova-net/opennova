# Tests exercise public seams; a test that needs a private is an API bug report

The GUT suite reaches into `_underscore` members of other objects on ~1,300
lines. The worst files poke sub-inspectors and their child widgets
(`._scripting`, `._sc_trigger_params`), call private methods as entry points
(`._set_resource_root_dir`, `._refresh`), and read internal subsystem state
(`._popovers`, `._document`). Every one of those lines is the test suite
telling us an API is missing: the behavior is real enough to assert but has
no public surface to assert through. The cost is symmetric — internals
cannot be refactored without breaking tests that never cared about the
internals, which is exactly what slowed the shell decomposition.

The repo already has the antidote pattern:
`EditorWorkstation.get_workspace_adapter()` was added precisely to kill six
external reach-ins, and the A8 extraction updated ~30 test sites that poked
moved internals.

## Decisions worth recording

- **Tests drive and assert through public surfaces.** When a test needs
  something private, the fix is on the code side, by category:
  - *Reaching into child controls / sub-components* → the owner exposes a
    public accessor for the component (the `get_workspace_adapter`
    precedent), or better, a public query for the *fact* the test wants.
  - *Calling a private method as an entry point* → the action becomes a
    public verb (it evidently IS an operation users of the class perform).
  - *Reading internal state* → a public getter for the state that is
    genuinely part of the contract; state that isn't contract shouldn't be
    asserted at all.
- **Setup shortcuts count too.** Tests that reach in to *arrange* state
  (not just assert) get public seams the same way — construction
  parameters, configuration methods, or test-visible factories.
- **The ratchet.** The count of non-self `._name` accesses in
  `godot/tests/**` is baselined and may not increase; the maturity program
  drives the top offenders toward zero with refactor-only slices (identical
  assert counts prove nothing behavioral changed). The long tail converts
  adopt-on-touch.
- **Privates stay private.** The rule is not "make everything public" — it
  is that the public surface must be sufficient to observe every behavior
  worth testing. If exposing something feels wrong, the test is asserting
  an implementation detail; delete the assertion or redesign the seam.
