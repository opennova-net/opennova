# Live Repro Task Template

## Goal

Reproduce one client/server path without inventing protocol behavior.

## Inputs

- Target: local OpenNova, retail client vs OpenNova, retail host vs OpenNova, or
  original NovaWorld.
- Topology: same-machine loopback, second-machine LAN, Docker/dev, or prod.
- Operator-owned accounts or no account needed:
- Capture plan:

## Steps

1. Record topology and exact env vars before launching.
2. Start the scoped server/client path.
3. Capture from process start through success/failure.
4. Collect `/api/server-info`, `/api/hosts`, `/api/lobbies`, and
   `/api/unknowns` when using OpenNova backend.
5. Collect `/PROFILE` `.sph` logs when applicable.
6. Decode and compare to known fixtures/tests.

## Output

- Dated run log.
- Commands and topology, without secrets or local absolute paths.
- Capture names in `.scratch`.
- Pass/fail signature.
- Next action: code fix, packet diff, IDA witness, or test gap.

