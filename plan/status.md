# Status

Integration trunk: `web-nw-for-real-master`. All 20 PRs below are **MERGED** to the trunk; the
single integration PR **#136** (`web-nw-for-real-master` → `master`) is **OPEN**.
Updated: 2026-06-11.

**Remaining: PR 21 (operator-run cutover).** Runbook: [`pr21-cutover-runbook.md`](pr21-cutover-runbook.md).

## PR sequence

| # | PR | Track | State | Notes |
|---|----|-------|-------|-------|
| 1 | plan/ scaffold + CI trigger + .gitignore | shared | MERGED | |
| 2 | GameWorld rename (world-sim scene) | A | MERGED | freed the NovaWorld name for the service |
| 3 | C++ server reland (libs + apps + backend + tests) | A | MERGED | from `net/pr37-rescue` (1bb779e1) |
| 4 | web portal reland | C | MERGED | |
| 5 | NovaWorldClient binding reland | A | MERGED | |
| 6 | parity fixtures + replay tests | A | MERGED | |
| 7 | IDA grill wave 1: gate, session, containers | A | MERGED | verdicts in RE doc §8 |
| 8 | IDA grill wave 2: login crypto, GSB/GLB, teardown | A | MERGED | |
| 9 | unknown-message tracking + /api/server-info | A | MERGED | |
| 10 | session/host lifecycle hardening | A | MERGED | |
| 11 | server image + dev compose | B | MERGED | |
| 12 | GHCR images workflow | B | MERGED | |
| 13 | terraform port | B | MERGED | |
| 14 | deploy toolbox + 1Password flow | B | MERGED | |
| 15 | backup sidecar | B | MERGED | |
| 16 | launcher (hosts-file model) | C | MERGED | |
| 17 | web copy pass | C | MERGED | |
| 18 | launcher publish flow | B/C | MERGED | |
| 19 | client session + menu wiring milestone | A | MERGED | |
| 20 | in-match net seam ADR | A | MERGED | |
| — | grill wave 3: novacrypto NW-C1..C4 (EPASK/PUBcrypto/url_cipher/NWU) | A | MERGED | `edbb6c33`, all MATCHING byte-exact vs retail; RE doc §8 + correspondence §5.1-5.4 |
| 21 | staging end-to-end + cutover | B | **runbook ready, pending operator** | [`pr21-cutover-runbook.md`](pr21-cutover-runbook.md); needs AWS + 1P vault + Cloudflare + docker host |

Final step after PR 21: merge **#136** → `master`.

## Decisions log

| Date | Decision |
|------|----------|
| 2026-06-10 | Deployable server = relanded PR #37 C++ stack; opennova-int stays reference-only |
| 2026-06-10 | SQLite; fresh database at cutover (no user migration from the old postgres) |
| 2026-06-10 | Launcher stays C# WinForms; hook deleted; hosts-file redirection; admin required; toggle off means launch disabled (strict, no escape hatch) |
| 2026-06-10 | Secrets via 1Password service-account token + op CLI in the deploy container |
| 2026-06-10 | Cutover: fresh EC2 + EIP, staging smoke, Cloudflare DNS flip; old box retires |
| 2026-06-10 | World-sim scene rename target: `GameWorld` |
| 2026-06-11 | grill wave 3: novacrypto verified vs retail using opennova-int Python as production-proven second witness; NK/CK = url_cipher and BK = literal "986119" (the earlier "NK/CK/BK = PUBcrypto" was a mislabel) |

## Verification milestones

- [x] Scoped net ctest green on win + mac + ubuntu (PR 3)
- [x] Local server boot smoke (PR 3)
- [x] jodemo re-validates browser visibility through the hosts path, no hook (PR 16)
- [x] Retail JO local end-to-end: gate, login, browse, host, second-client join, teardown (PR 11/16)
- [x] Unknowns API shows JOINTOPERATIONS PN sightings when a match starts (PR 9)
- [x] Two-client menu milestone through our game shell (PR 19)
- [x] novacrypto byte-exact vs retail: NWU + EPASK + PUBcrypto + url_cipher (grill wave 3)
- [ ] Staging deploy from a docker-only machine; backup verified; destroy clean (PR 21)
- [ ] Prod cutover: DNS flip, launcher resolves new IP, old box retired (PR 21)

## Grill items carried

Wave 1/2 items were resolved in their owner PRs (#7/#8, merged). Wave 3 (login/session crypto)
is complete — see RE doc §8 (NW-C1..C4) and [`pr21-cutover-runbook.md`](pr21-cutover-runbook.md)
is unaffected by them.

| ID | Question | Owner PR | State |
|----|----------|----------|-------|
| NW-G1 | POSTIPADDRESS/POSTIPPORT required-vs-optional per binary | 7 | resolved in PR 7 |
| NW-G2 | GSB vs GLB chunk tags: retail vs demo | 8 | resolved in PR 8 |
| NW-G3 | Teardown semantics: what the client emits on host quit | 8 | resolved in PR 8 |
| NW-L1 | DFX2 gate hostname (dfx2.exe) | 7 | deferred (needs dfx2.exe IDB) |
| NW-L2 | Non-gate novalogic/novaworld URLs (news, MOTD) | 7 | resolved in PR 7 |
| NW-C1..C4 | NWU / EPASK / PUBcrypto / url_cipher byte-exact vs retail | wave 3 | **resolved** (`edbb6c33`) |
