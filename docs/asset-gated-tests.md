# Asset-gated tests — the matrix, local setup, and the CI stance

Some tests exercise data we cannot commit: retail game installs, extracted retail
assets, and network captures of retail sessions. Each such test is gated on an
environment variable and **skips as a PASS** when the variable is unset — the C++
gates print a `SKIP`/`[skip]` line and `return 0` (they are plain `add_test`
entries, not `GTEST_SKIP`/`DISABLED`), pytest gates use `pytest.skip`, and the one
GUT gate uses `pending()`. Consequence: **a green full run does not mean these
tests exercised anything.** When touching a gated area, set the variable and check
the test's output for real work, not just its exit code.

Machine paths live in `.claude/settings.local.json` `env` (untracked) — never in
tracked files. Capture files default to `${CMAKE_SOURCE_DIR}/.scratch/…`
(gitignored) before the env override applies, so populating the main checkout's
`.scratch/` is enough for the capture-gated ctests.

## The matrix

| Env var | Gates | Data | CI-able? |
|---|---|---|---|
| `OPENNOVA_JO_DIR` | ctest `rtxt_jo_install_sweep`, `env_jo_install`, `terrain_tile_composer` (CP12 `.til` entries #53/#1013); GUT `terrain_static_shadow_runtime_test.gd` (the Scrate1 shadow witness legs) | retail JO install dir (packed `.pff`); the CP12 oracle additionally needs an install whose packed set carries `CP12.TIL`/`TRNTILEA1.TGA` (JO:CA ships without them — that leg skips) | never (copyright, ~1.5 GB) |
| `OPENNOVA_MISSION_CORPUS` | ctest `mission_corpus`; GUT `mission_corpus_binding_test.gd` | dir of retail `.bms` missions | never (copyright) |
| `OPENNOVA_JO_ASSETS` | ctest `occlusion_armry`, `root_motion`, the `wac_corpus` sweep; GUT `sound_dialog_test.gd`, `sound_integration_test.gd`; pytest `test_bad_pos_derivation.py` (corpus legs; its synthetic tests run ungated) | extracted retail assets with `weapon.def` + models | never (copyright) |
| `OPENNOVA_WAC_CORPUS_DIRS` | ctest `wac_corpus` extra corpus dirs (semicolon list, beside the `OPENNOVA_JO_ASSETS` sweep) | dirs of retail `.wac` scripts | never (copyright) |
| `OPENNOVA_MNU_EXTRA` | ctest `mnu_compat` developer-only loose menu leg | a single loose `.mnu` | never (developer-local) |
| `NW_INGAME_HEXCAP` | ctest `nw_ingame_histogram`, `nw_ingame_pool_records` | focused in-match hexcap dump | possible: policy allows a small *sanitized* `.hexcap` via LFS (user-gated follow-up) |
| `NW_PROFILE_SPH_DIR` | ctest `nw_serverlog_decode` | dir with `host.sph` + `client.sph` `/profile` recordings | never (retail run output; can embed account identity) |
| `NW_DVXI5_PCAP` / `NW_DVXI3_PCAP` / `NW_DVXC1_PCAP` | ctest `nw_pool_groundtruth`, `nw_dvxi3_groundtruth`, `nw_dvxc1_groundtruth` | authored probe captures (`.pcapng`) | never (raw-capture policy) |
| `NW_PROBE3AGAIN_PCAP` (+ `NW_PROBE3AGAIN_HOST_SPH`) | ctest `nw_probe3again_lifecycle`, `nw_capture_decoder` extra leg | 3-player co-op capture + host `.sph` oracle | never |
| `NW_WHITENOISE_PCAP` | ctest `nw_whitenoise_coverage` | stock retail co-op capture | never |
| `NW_GOLDEN_GAMEPLAY` | ctest `npruntime_golden_gameplay`, `npruntime_golden_client`, `nw_golden_diff` | golden retail↔retail gameplay capture (`.scratch/golden/retail-gameplay-session.pcapng`) | never |
| `NW_KARO_GUIDED_PCAP` | ctest `nw_karo_guided` | a local retail Karo Highlands capture carrying Stinger guided traffic (S2C 0x44) - the D-NET-64 wire-validation leg | never |
| `NW_GOLDEN_LAN_JOIN` / `NW_GOLDEN_LAN_JOIN_SESSION` | ctest `npruntime_golden_lan_join`, `npruntime_two_endpoint_socket` cross-check, `npruntime_golden_lan_join_session` | golden retail LAN host/join captures | never |
| `NW_GOLDEN_OURS` | ctest `nw_golden_diff` ("ours" side) | our own freshly captured join | committable in principle, but the diff needs the retail golden too |
| `NW_GOLDEN_VEHICLE_SESSION` + `NW_ITEMS_DEF` | ctest `netsim_client_replica_pipeline_capture_parent_follow` (skips clean when either is absent) | golden retail vehicle-session capture (`.scratch/golden/retail-vehicle-session.pcapng`) + an extracted retail `ITEMS.DEF` for compact-record class resolution | never |
| `NW_LIVE_WEAPON_ROOT` / `NW_LIVE_WEAPON_EXPANSION` | ctest `npruntime_weapon_table` corpus leg (its synthesized cases run ungated; getenv-gated in the test, not CMake) | retail `weapon.def` roots (base + expansion) | never (copyright) |
| `OPENNOVA_WEAPON_SAV` | ctest `playersav_weapon_sav` (corpus leg only; its synthesized cases run ungated) | a retail `weapon.sav` player profile — `<install>/expansion/<exp>/weapon.sav`, else `<install>/weapon.sav` (net-re §5.66) | never (player profile data, and the file carries the local player's callsign-adjacent selections) |
| `OPENNOVA_MODSUPEROED_DIR` | pytest modsuperoed automation smoke | third-party OED pack | already in CI (LFS submodule; the `modsuperoed-smoke` job) |
| (LFS content, no env var) | pytest `test_retail_render_evidence.py` | the committed render-evidence store under `screenshots/parity/render-lighting-2026-08/` — the suite validates the repo's OWN published evidence (hashes, decodable PNGs, manifests), so its gate is LFS materialization, not an install: it skips as a PASS when the store holds LFS pointer stubs (e.g. `lfs.fetchexclude`d clones) | possible in principle (LFS bandwidth), not wired |

Manual probes/tools share the gating pattern but are not CI-collected (see
`godot/tests/CLAUDE.md`: `*_probe.gd` are uncollected): `NW_SP_MISSION` +
`NW_RESOURCE_DIR` (the standalone-game probe pattern — boot straight into a
mission with a retail install mounted, e.g. `ladder_climb_probe.gd`),
`OPENNOVA_JO_DIR` + `SPLASH_CAPTURE_DIR` (the splash render probe),
`JO_ASSETS_DIR` + `JO_PROBE_MISSIONS` (mission load/re-ground perf probes),
`JO_RESOURCE_DIR` / `JO_EXPANSION` / `JO_MISSION` (mount diagnostics),
`NOVA_RESOURCE_DIR` / `NOVA_MISSION_BMS` (the modtools screenshot driver),
and the render-fixture capture probe family (`render_fixture_capture_probe.gd`
+ `scripts/render/`): `NOVA_RENDER_FIXTURE_ID` / `NOVA_RENDER_FIXTURE_MINUTE` /
`NOVA_RENDER_FIXTURE_OUTPUT`, `NOVA_RENDER_CAPTURE_MODE` /
`NOVA_RENDER_CAPTURE_PROFILE`, `NOVA_MISSION_RESOURCE_DIR` /
`NOVA_RUNTIME_RESOURCE_DIR` / `NOVA_EXPANSION`, `NOVA_EVIDENCE_SOURCE_COMMIT`,
and `NOVA_RENDER_STATIC_SHADOW_SUPPRESS_BMS_IDS` — documented in detail in
[docs/render/README.md](render/README.md).

## Local setup

Add to `.claude/settings.local.json` (adjust to this machine's paths):

```json
{
  "env": {
    "OPENNOVA_JO_DIR": "C:/Users/<you>/Desktop/Games/Joint Operations Combined Arms",
    "OPENNOVA_MISSION_CORPUS": "C:/Users/<you>/Desktop/JOX",
    "OPENNOVA_JO_ASSETS": "C:/Users/<you>/Desktop/JOX"
  }
}
```

Capture-gated ctests need no env vars if the files sit at their defaults under the
main checkout's `.scratch/`: the goldens at `.scratch/golden/`, probe captures at
`.scratch/<name>.pcapng`. Captures are produced by the recipes in
`.agents/README.md` (retail-join stack) and consumed via `nw_pp --stream` first —
see the capture policy in `.agents/interop.md`.

## Why captures are never committed

Beyond the blanket rule for retail data, **NovaWorld traffic can embed the account
username and password** (the gate/auth exchanges), and `.sph` recordings carry
account identity and machine paths. Treat every capture as credential-bearing
until proven otherwise. The tracked-fixture line is drawn in `.agents/interop.md`:
promote only small *sanitized* artifacts (`.nwmsg`, focused `.hexcap`, `.gsb`,
manifest rows) — `fixtures/novaworld/` shows the shape. Raw `.pcapng`/`.sph` stay
in gitignored `.scratch/`, full stop.

## CI stance (assessed 2026-07-04)

- The full `ctest` CI job already runs every gated test as a skip-pass; the logic
  they would exercise is covered in CI by the **inline-pcap unit tests**
  (`nw_pool_decode_unit_test`, `nw_capture_decoder_test`
  craft tiny in-memory pcaps and run unconditionally) — that is the sanctioned CI
  substitute, per the net-test convention.
- Retail-data gates can never run on public runners (copyright + size). A
  self-hosted runner with local retail data is the only path and is not currently
  worth the maintenance.
- The one improvement worth considering later: commit a trimmed, sanitized
  `.hexcap` so `nw_ingame_histogram`/`nw_ingame_pool_records` exercise data in CI.
  User-gated; sanitization review required.

## The two-tier wire-compat gate (maturity program NET-0)

The retail golden diff is env-gated and therefore skip-passes-as-green in CI — a
net-touching change can look green while silently altering wire bytes. The
maturity program (docs/maturity-program.md) closes that with two tiers:

- **Tier 1 — default CI, cannot skip.** `nw_codec_identity` pins the EXACT bytes
  of every in-match encoder against committed FNV-1a64 vectors over a synthetic
  corpus (our bytes only, so it is committable and unconditional; it also runs in
  the Linux net job). It exists because encode↔decode round-trips cannot see a
  SYMMETRIC codec change — both sides edited together stay field-identical while
  the wire moves. Updating a vector is a wire-format change: it requires the
  [orig] witness or a D-NET entry in the same commit, never a bare regeneration
  (`NW_CODEC_DUMP=1` prints the replacement table). The second tier-1 leg
  (NET-0b) is `nw_self_capture` — `nw_golden_diff`'s self mode: a deterministic
  in-process opennova↔opennova join + play session is captured live and
  coverage-diffed per (direction, tag) against the committed opennova-produced
  fixture `fixtures/novaworld/self-capture-session.pcap` (LFS; zero retail
  bytes, so committable). Because both sides are ours the comparison is EXACT
  set equality in both directions — no noise floor, no allowlists — and a
  missing fixture FAILS rather than skips. Regenerating the fixture
  (`OPENNOVA_WRITE_SELF_FIXTURE=1`, which re-reads and re-verifies the file) is
  a wire-coverage change: justify the tag delta in the same commit.
- **Tier 2 — local, mandatory protocol for net-touching PRs.** Run the retail
  golden diff (`NW_GOLDEN_OURS` + the `.scratch/golden/` gameplay capture) and
  the npruntime golden joins against local retail data, and **attest the run in
  the PR description** (the commands + PASS lines). A net-touching PR without
  the attestation is not reviewable. This is the standing substitute for the
  un-CI-able retail gates above.
