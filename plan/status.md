# Status (historical record — NovaWorld integration, completed)

> **This file is a completed-effort record, not live status.** The integration PR #136
> landed on master, and master has since moved well past it (the consolidated in-match
> net core, ADR 0013, and the retail-join fidelity rounds in
> `docs/net/novaworld-net-re.md`). For current truth read the source and
> `docs/net/novaworld-net-re.md`; the tables below are kept as history.

Integration trunk: `web-nw-for-real-master`. All 20 PRs below were **MERGED** to the trunk,
and the single integration PR **#136** (`web-nw-for-real-master` → `master`) followed.
Last updated as live status: 2026-06-12.

**Remaining: PR 21 (operator-run cutover).** Runbook: [`pr21-cutover-runbook.md`](pr21-cutover-runbook.md).

## Client completion (ADR 0010)

Our own runtime client against both targets (OpenNova server / original NovaWorld):

- Phases 0–2 done on the trunk: endpoint picker, gate→hello→auth→verify session, GSB browse.
- Against **our** server the client reaches Verified/CONNECTED end-to-end (re-verified
  2026-06-12 on the trunk tip: 20/20 net ctests, live boot, HTTP `server-info`/`unknowns`/GSB smoke).
- Against **original NovaWorld** (`gs.novaworld.net` = 207.178.209.201; web/asset host
  207.178.209.204 — NovaLogic's boxes, **not ours**; they are the target we emulate) the client
  now **reaches CONNECTED/VALIDATED over the lobby UDP session — confirmed LIVE 2026-06-12**
  (`~/Desktop/capture_opennova.pcapng`: full `0x41→…→ServerVerifyResult→` keepalives). NW-S5 was a
  DSP seq/ack bug, not a login/CD-key gate (RE doc Wave 5): 1-based seq + header-only acks + the
  full verify `Cookie` var-list (NWUID echoed) + retail-matched join CU set fixed it. Oracle
  `nw204_lobby_decode`; parity in `client_session_loopback`.
- **Authenticated web flow vs real `.204` (login → GSB → join) — CONFIRMED LIVE 2026-06-12**
  (RE doc Wave 6; `~/Desktop/capture_opennova2.pcapng`). The out-game client logged in as **ljim**
  (PCID `A-A02-085D18`), browsed **7 real servers** (GSB is unauthenticated on `.204`), ran the
  two-phase NWJoin, and sent a JointOperations ClientHello to the real game host `.204:3875` —
  the full multiplayer entry flow against genuine NovaLogic NovaWorld. Built from the retail HTTP
  contract: SessionInit web-domain extraction + `startupurl` template substitution, NWStart step,
  all-fields-EPASK-encrypted login POST + NWLogin poll, identity-cookie seeding, `jop_2.gsb?a=1`,
  full NWJoin query. 20/20 scoped net ctest green; GDExtension builds clean. The JointOperations
  session stops at the hello (ADR 0009 in-match seam — no gameplay yet). Minor follow-up: decode
  GSB server-name strings Latin-1→UTF-8 (a `©`/0xA9 name trips a Godot UTF-8 warning).
- Phase 3 (EPASK account login) **LANDED 2026-06-12** against our own server: the binding
  HTTP login chain (prepare GET → login POST → relay GET, cookie jar) + panel username/password
  fields. Session-only (no persistence).
- Phase 5 (join) **reached the proto-switch boundary 2026-06-12**: the out-game client logs in,
  browses the GSB, clicks Join → NWJoin two-phase → decodes the host address → opens a session
  to the host with `PN="JointOperations"`, flipping the connection protocol from NOVAWORLDUDP to
  JointOperations. Stops at that ClientHello (no in-match gameplay; ADR 0009 seam). Verified end
  to end with a headless driver against the local server: the server logged `NWLogin → auth ok`,
  `NWJoin (second call) rid=1`, and `[nwudp] refusing PN='JointOperations'`, and
  `GET /api/unknowns` shows the `pn:JointOperations` sighting with the full ClientHello. Plan +
  detail in [`phase3-host-wiring.md`] and the `we-need-to-get-gentle-owl` plan. **Provisional:**
  the JointOperations PG GUID/PV1 are placeholders ("JO-PROVIS-PG") pending the
  `StartPlaying @ 0x4d45e0` grill — sufficient to cross the boundary locally (our server rejects
  on PN alone), needed only for a real retail host.

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
| 21 | production deploy + cutover | B | **runbook ready, pending operator** | [`pr21-cutover-runbook.md`](pr21-cutover-runbook.md); needs AWS + 1P vault + Cloudflare + docker host |

Final step after PR 21: merge **#136** → `master`.

## Decisions log

| Date | Decision |
|------|----------|
| 2026-06-10 | Deployable server = relanded PR #37 C++ stack; opennova-int stays reference-only |
| 2026-06-10 | SQLite; fresh database at cutover (no user migration from the old postgres) |
| 2026-06-10 | Launcher stays C# WinForms; hook deleted; hosts-file redirection; admin required; toggle off means launch disabled (strict, no escape hatch) |
| 2026-06-10 | Secrets via 1Password service-account token + op CLI in the deploy container |
| 2026-06-10 | Cutover: fresh EC2 + EIP, Cloudflare DNS, smoke test; old box retires |
| 2026-06-10 | World-sim scene rename target: `GameWorld` |
| 2026-06-11 | grill wave 3: novacrypto verified vs retail using opennova-int Python as production-proven second witness; NK/CK = url_cipher and BK = literal "986119" (the earlier "NK/CK/BK = PUBcrypto" was a mislabel) |
| 2026-06-11 | Dropped the staging environment / terraform-workspace concept — single prod environment (staging can be re-added later) |
| 2026-06-12 | `207.178.209.201/.204` (gs.novaworld.net + web host) confirmed as **genuine NovaLogic NovaWorld** — we do not control them; they are the parity target. An earlier "stale build of our emulator" reading was wrong (the live gate really does issue `abc`/`xyz` udpcodes pre-login; opennova-int's "stubs" mirror sniffed reality) |

## Verification milestones

- [x] Scoped net ctest green on win + mac + ubuntu (PR 3)
- [x] Local server boot smoke (PR 3)
- [x] jodemo re-validates browser visibility through the hosts path, no hook (PR 16)
- [x] Retail JO local end-to-end: gate, login, browse, host, second-client join, teardown (PR 11/16)
- [x] Unknowns API shows JOINTOPERATIONS PN sightings when a match starts (PR 9)
- [x] Two-client menu milestone through our game shell (PR 19)
- [x] novacrypto byte-exact vs retail: NWU + EPASK + PUBcrypto + url_cipher (grill wave 3)
- [ ] Production deploy from a docker-only machine: infra apply, app deploy, backup verified (PR 21)
- [ ] Retail JO smoke over the internet; `nw.<domain>` resolves to the EIP and the launcher connects (PR 21)

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
| NW-S5 | Genuine NW sends no `ServerStartVerify`/validation after our `ClientConnected` (gate/hello/SessionInit fine) | client P3 | **fixed** (2026-06-12, RE doc Wave 5): a real `.204` capture (`fixtures/novaworld/nw204_lobby.hexcap`) refuted the Wave-4 "needs HTTP login" inference. Login is NOT a verify prereq; the verify is not credential-gated (CD-key fields empty on the wire, `Success=1`). Real cause = DSP seq/ack: our outbound 0x43 seq was 0-based and we never acked the settings packet. Fixed: 1-based seq + header-only acks + full `Cookie` var-list (NWUID echoed). Oracle `nw204_lobby_decode`; parity in `client_session_loopback`. Supersedes Wave-4 NW-S5 and reverted NW-S4. |
