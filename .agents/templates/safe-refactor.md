# Safe Refactor Task Template

## Goal

Improve organization without changing behavior or packet bytes.

## Preconditions

- Exact surface named.
- Current tests or capture oracle identified.
- Byte-output invariants listed.

## Steps

1. Keep changes narrow.
2. Preserve public APIs and packet order unless the task explicitly says to
   change them.
3. Keep protocol/session code in libraries and socket/UI code in wrappers/apps.
4. Run the scoped verification before and after when practical.
5. If a mismatch appears, stop and classify it before continuing.

## Forbidden

- Changing wire bytes or timing as part of cleanup.
- Deleting strange retail behavior because it looks redundant.
- Replacing typed models with raw passthrough.
- Moving gameplay into `NovaWorldClient`, `NovaNetClient`, or the NovaWorld
  backend service.

## Output

- Files changed.
- Behavior-preservation evidence.
- Tests run.
- Residual risk.

