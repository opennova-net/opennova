# Status

Integration trunk: `web-nw-for-real-master`. All PRs below target it.
Updated: 2026-06-11.

## PR sequence

| # | PR | Track | State | Notes |
|---|----|-------|-------|-------|
| 1 | plan/ scaffold + CI trigger + .gitignore | shared | OPEN | this PR |
| 2 | GameWorld rename (world-sim scene) | A | pending | frees the NovaWorld name for the service |
| 3 | C++ server reland (libs + apps + backend + tests) | A | pending | from `net/pr37-rescue` (1bb779e1), per-path extraction |
| 4 | web portal reland | C | pending | verbatim from 1bb779e1 |
| 5 | NovaWorldClient binding reland | A | pending | |
| 6 | parity fixtures + replay tests | A | pending | .nwmsg captures from worktree-net-final |
| 7 | IDA grill wave 1: gate, session, containers | A | pending | per-system verdicts to RE doc §8 |
| 8 | IDA grill wave 2: login crypto, GSB/GLB, teardown | A | pending | includes NW-L1/NW-L2 launcher items |
| 9 | unknown-message tracking + /api/server-info | A | pending | |
| 10 | session/host lifecycle hardening | A | pending | grill-informed |
| 11 | server image + dev compose | B | pending | |
| 12 | GHCR images workflow | B | pending | |
| 13 | terraform port | B | pending | |
| 14 | deploy toolbox + 1Password flow | B | pending | |
| 15 | backup sidecar | B | pending | |
| 16 | launcher (hosts-file model) | C | pending | |
| 17 | web copy pass | C | pending | |
| 18 | launcher publish flow | B/C | pending | |
| 19 | client session + menu wiring milestone | A | pending | first user-visible milestone |
| 20 | in-match net seam ADR | A | pending | design only |
| 21 | staging end-to-end + cutover record | B | pending | acceptance run |

## Decisions log

| Date | Decision |
|------|----------|
| 2026-06-10 | Deployable server = relanded PR #37 C++ stack; opennova-int stays reference-only |
| 2026-06-10 | SQLite; fresh database at cutover (no user migration from the old postgres) |
| 2026-06-10 | Launcher stays C# WinForms; hook deleted; hosts-file redirection; admin required; toggle off means launch disabled (strict, no escape hatch) |
| 2026-06-10 | Secrets via 1Password service-account token + op CLI in the deploy container |
| 2026-06-10 | Cutover: fresh EC2 + EIP, staging smoke, Cloudflare DNS flip; old box retires |
| 2026-06-10 | World-sim scene rename target: `GameWorld` |

## Verification milestones

- [ ] Scoped net ctest green on win + mac + ubuntu (PR 3)
- [ ] Local server boot smoke (PR 3)
- [ ] jodemo re-validates browser visibility through the hosts path, no hook (PR 16)
- [ ] Retail JO local end-to-end: gate, login, browse, host, second-client join, teardown (PR 11/16)
- [ ] Unknowns API shows JOINTOPERATIONS PN sightings when a match starts (PR 9)
- [ ] Two-client menu milestone through our game shell (PR 19)
- [ ] Staging deploy from a docker-only machine; backup verified; destroy clean (PR 21)
- [ ] Prod cutover: DNS flip, launcher resolves new IP, old box retired (PR 21)

## Grill items carried

| ID | Question | Owner PR |
|----|----------|----------|
| NW-G1 | POSTIPADDRESS/POSTIPPORT: required by jodemo, omitted by onnet on retail. Per-binary required/optional gate VAR table | 7 |
| NW-G2 | GSB vs GLB chunk tags: pin retail vs demo formats definitively | 8 |
| NW-G3 | Teardown semantics: what the client emits on host quit; is ServerGoodBye expected | 8 |
| NW-L1 | DFX2 gate hostname (dfx2.exe CNapiGateManager default) | 7 |
| NW-L2 | Non-gate novalogic/novaworld URLs the client contacts (news, MOTD) | 7 |
