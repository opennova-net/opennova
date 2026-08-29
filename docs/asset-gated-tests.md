# Asset-gated tests — the roots, the matrix, local setup, and the CI stance

Some tests exercise data we cannot commit: retail game installs, extracted retail
assets, retail mission corpora, and network captures of retail sessions. Each such
test is gated on one of four documented roots and **reports Skipped** without it:
a fully gated ctest returns 77 (`opennova_add_gated_test` sets
`SKIP_RETURN_CODE`, so `ctest` prints `***Skipped`) after a `SKIP: needs ...`
line; a mixed test runs its synthetic legs and prints `SKIP-LEG: needs ...` for
the retail leg, exiting 0; the GUT gates `pending()`. A green run therefore
never hides an unexercised gate, but it also never proves the gate ran: when
touching a gated area, set the root and read the test's output.

The four roots (`docs/dev-env-vars.md`) are read only by the resolvers —
`tests/common/retail_paths.h` (`retail::install()`, `assets()`,
`mission_corpus()`, `captures_root()`, `golden(name)`, `capture(name)`,
`sph(name)`, `asset_file(name)`, `expansions()`, `weapon_sav()`, `skip`,
`skip_leg`, `RETAIL_REQUIRE_OR_SKIP`), `godot/tests/support/retail_data.gd`
(`RetailData.install()`, `assets()`, `mission_corpus()`, `expansions()`,
`mount_install_with(witness)`), and the
`Get-OpenNovaRetail*` getters in `scripts/net/lib.ps1`. Machine paths live in
`.claude/settings.local.json` `env` (untracked), never in tracked files.

## The matrix

| Root | Points at | Gates |
|---|---|---|
| `OPENNOVA_JO_DIR` | a packed retail JO install (the `.pff` set; expansions under `expansion/<name>/`) | ctest `rtxt_jo_install_sweep`, `env_jo_install`, `bink_retail`, `sbf_jo_install_sweep`, `mission_ai_path_conformance`, `mission_coop_convoy`, `mission_script_report`, `bunker_walkin`, `truck_dismount`, `npruntime_authored_payload_00trg` (00TRg through revx02, or the `OPENNOVA_JO_ASSETS` tree); the mission-kernel ctests (`mission::MissionKernel` over `tests/common/retail_mission_files`) `ai_threat`, `ladder_00tra`, `truck_rest_00tra`, `ai_muzzle_pose` (CP01), `vehicle_ride_00tra`, `defense_00trg`, `lose_flow_04tr`, `particle_gore_set_catalog`; the SKIP-LEG legs of `terrain_tile_composer` (iterates `expansions()` for the CP12/00TRa tile witnesses), `ground_conform` (the CP01 standing leg), `score_roundtrip` (the `score.ini` the install ships loose beside its archives), `playersav_weapon_sav` (`weapon.sav` at the root or under an expansion), `minimap_overlay` (00TRg with revx02), `npruntime_weapon_table` (the live `weapon.def` oracle over the install's expansions); GUT `avatar_preview_test`, `e50trib_mount_alignment_test`, `skeletal_anim_test`, `sound_pff_install_test` (every expansion), `terrain_static_shadow_runtime_test`, `veg_assets_test`, `vehicle_emplacement_alignment_test` |
| `OPENNOVA_JO_ASSETS` | an extracted retail asset tree (`items.def`, `weapon.def`, `ammo.def`, models, `.adm`, `.bms`, `.til`, `.lwf`, ...) plus its `fixtures/` subtree: the retail-interop fixture set (`retail::reference_fixture(rel)` / `RetailData.fixture(rel)` resolve `<assets>/fixtures/<rel>`), the retail files whose in-tree copies this repository no longer carries | ctest `root_motion`, `anim_positions_from_model_corpus`, `anim_reload_clips_us01`, `anim_weapon_action_clips`, `wac_corpus` (plus argv corpus dirs), `cpt_jo_assets_sweep`, `lwf_jo_assets_sweep`, `npruntime_authored_payload_00trg` (00TRg; also served by the install's revx02), `netsim_client_replica_pipeline_capture_parent_follow` (`items.def`, and the vehicle capture below: Skipped whenever either is absent); the mission-kernel ctests `ai_corpse`, `rock_collision_00trg`, `soak_00trg`, `native_assets_00trg`, `npruntime_remote_body_state`, `npruntime_held_weapon_attach`; the SKIP-LEG legs of `occlusion_armry`, `threedi_panm_ctrl` (the six retail controlled models), `particle_smoke_all_fixtures` (the `.ptl` corpus), `sound_profile` (`sndprof.def`), `def_parse_items` (the particlefx rows), `infantry` (the weapon-channel leg), `minimap_overlay` (00TRg); GUT `sound_dialog_test`, `sound_integration_test`; over the `fixtures/` subtree (the reference fixture set) the SKIP-LEG legs of `dbf_roundtrip` (00TRg.DBF) and `cbin_roundtrip` (the three shipped `nlist.kda`), the menu-driven GUT scripts `armory_menu_seam_test`, `armory_presenter_test`, `deploy_screen_presenter_test`, `host_punt_surfacing_test`, `menu_shell_test` (whole scripts, `should_skip_script`) and the legs `game_world_test` (the armory weapon-database reuse) and `loading_screen_test` (the session-variable overlay) |
| `OPENNOVA_MISSION_CORPUS` | a directory of retail `.bms` missions (loose) | ctest `mission_corpus`; GUT `mission_corpus_binding_test`; the render-fixture capture's loose mission (`scripts/render/*.ps1`) |
| `OPENNOVA_CAPTURES` | the captures/goldens root (default `<repo>/.scratch`) | fixed names: `golden/retail-gameplay-session.pcapng` (`npruntime_golden_gameplay`, `npruntime_golden_client`, `nw_golden_diff`'s golden side), `golden/retail-lan-host-join.pcapng` (`npruntime_golden_lan_join`, `npruntime_two_endpoint_socket`'s cross-check leg), `golden/retail-lan-host-join-session.pcapng` (`npruntime_golden_lan_join_session`), `golden/retail-vehicle-session.pcapng` (`netsim_client_replica_pipeline_capture_parent_follow`), `host_and_join_game_on_opennovaworld_loopback_mission_probe.pcapng` (`nw_pool_groundtruth`), `probe2.pcapng` (`nw_dvxi3_groundtruth`), `probe3.pcapng` (`nw_dvxc1_groundtruth`), `probe3_again.pcapng` + `sph/hostprof_probe3again.sph` (`nw_probe3again_lifecycle`, `nw_capture_decoder`'s extra leg), `operation_whitenoise.pcapng` (`nw_whitenoise_coverage`), `karo-guided.pcapng` (`nw_karo_guided`, the D-NET-64 wire leg), `ingame.hexcap` (`nw_ingame_histogram`, `nw_ingame_pool_records`), `sph/host.sph` + `sph/client.sph` (`nw_serverlog_decode`) |

The live "ours" side of the golden diff is argv, not a root:
`nw_golden_diff_test --ours <capture>` (a `ctest` run without it reports Skipped).
Developer knobs are argv too: `mnu_compat_test <extra.mnu>...`,
`wac_corpus_test <dir>...`, `ai_path_conformance_test --report/--ticks/--bms`,
`bunker_walkin_test --from/--to/--column`; dumps are `--dump`, `--write`,
`--write-fixture`, `--write-pff` (`docs/dev-env-vars.md`).

The 00TRa tile-composer leg fingerprints the archived `TRNTILE10.TGA`
payload (`SHA-256 eb3b25ca50f66f2006668198919c8e25374d093c0290e9aceb613ee37d8bc490`)
and pins the correct shipped-output RGB hashes for entries 761/781. This
guards against repeating the 2026-08-21 test-only re-pin that left the
asset-gated test permanently red without changing the renderer.

Runtime probes (`game_probe` tools under `godot/probes/`, `docs/mcp.md`) take
their retail roots as typed arguments (`mission_path`, `mission_resource_dir`,
`output_dir`, ...) or from the launch's `--resource-dir`; they read no environment
variable. The scripts that drive them default those arguments to the four roots.

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

The extracted tree must also carry the `fixtures/` subtree of
`opennova-net/opennova-reference-assets` (the retail-interop fixture set:
`fixtures/mnu/jo_*.mnu`, `fixtures/def/weapon.def`, `fixtures/rtxt/*.bin`, ...),
which a plain JOX extract lacks: clone that repository and point
`OPENNOVA_JO_ASSETS` at the clone, or copy its `fixtures/` directory into the
extract. A test that needs one of those files reads
`retail::reference_fixture("mnu/jo_main.mnu")` (ctest) or
`RetailData.fixture("mnu/jo_main.mnu")` (GUT) and skips or pends without it.

Capture-gated ctests need no variable when the files sit at their fixed names
under the main checkout's `.scratch/` (the default `OPENNOVA_CAPTURES`).
Captures are produced by the recipes in `.agents/README.md` (retail-join
stack) and consumed via `nw_pp --stream` first — see the capture policy in
`.agents/interop.md`. A packed install without an expansion the test needs
(revx02 for the 00TRg payload oracle) skips that leg; the extracted tree
carries the same pair and serves it.

Run the gated set with the roots exported, e.g. `ctest --test-dir build -C
Release -R "00tra|00trg|ai_|muzzle|reload_clips|weapon_action|remote_body|held_weapon|authored_payload|gore_set"`,
and read the output: a real run prints its measurements, a skipped one prints
`SKIP:`.

## CI

Retail data never reaches public runners as tracked files (copyright, size,
credentials). Two private repositories carry what CI needs:
`opennova-net/opennova-reference-assets` (the extracted tree behind
`OPENNOVA_JO_ASSETS` and `OPENNOVA_MISSION_CORPUS`; its README lists the
files) and `opennova-net/opennova-reference-retail-packed` (the packed install
behind `OPENNOVA_JO_DIR`: the retail JO:CA `language.pff`, `localres.pff`,
`resource.pff`, `main.bik`, the SBF banks, `score.ini`, the `weapon.sav` pair,
`Jointops.exe` + Bink, and `expansion/jox01`; the archives are committed as
95 MiB plain-git parts that its `reassemble.sh` rebuilds and verifies against
`MANIFEST.sha256`; a JOTAC tree is a mod install whose `items.def`,
`weapon.def`, rigs and models the retail pins reject, so it never feeds this
root). With the
`REFERENCE_ASSETS_TOKEN` secret (a token with `contents: read` on BOTH
repositories) the `test` and `godot-tests` jobs check the assets out beside
the tree (both restored from the Actions cache keyed by each repository's
`main` commit and saved as soon as the data is ready, so a miss clones — and
reassembles the packed set — once, even when the tests then fail), and point
the three roots at them, so every root-gated test except the capture gates
runs in CI instead of reporting Skipped; without the secret the gates stay
closed and the job is still green. The `OPENNOVA_CAPTURES` gate stays local.
`.github/workflows/ci.yml` is the record.

A green run still never proves a gate opened, so with the data mounted both
jobs end by attesting it: `scripts/ci/retail_gates_ran.py` reads ctest's JUnit
report (`scripts/build.sh` writes `build/Testing/ctest.xml`) and the GUT log,
and fails on any fully gated test that reported Skipped or any `SKIP-LEG:` /
`[Pending]` line that names a mounted root. Its expectation tables are this
page's matrix; a new gated test is added to both. The one known gap it reports
without failing: `mission_coop_convoy`, `mission_script_report` and
`bunker_walkin` promote `05TRcoop.bms`, which no known retail mount or corpus
carries (they read it through the mount, then `OPENNOVA_MISSION_CORPUS`, and
skip honestly), and
`netsim_client_replica_pipeline_capture_parent_follow` also needs the vehicle
capture, which never rides CI.

The logic the capture gates would exercise is covered in CI by the
**inline-pcap unit tests** (`nw_pool_decode_unit_test`,
`nw_capture_decoder_test` craft tiny in-memory pcaps and run unconditionally)
— the sanctioned CI substitute, per the net-test convention.

## Why captures are never committed

Beyond the blanket rule for retail data, **NovaWorld traffic can embed the account
username and password** (the gate/auth exchanges), and `.sph` recordings carry
account identity and machine paths. Treat every capture as credential-bearing
until proven otherwise. The tracked-fixture line is drawn in `.agents/interop.md`:
promote only small *sanitized* artifacts (`.nwmsg`, focused `.hexcap`, `.gsb`,
manifest rows) — `fixtures/novaworld/` shows the shape. Raw `.pcapng`/`.sph` stay
in gitignored `.scratch/`, full stop.

## The two-tier wire-compat gate (maturity program NET-0)

The retail golden diff is root-gated and therefore Skipped in CI — a
net-touching change can look green while silently altering wire bytes. The
maturity program (docs/maturity-program.md) closes that with two tiers:

- **Tier 1 — default CI, cannot skip.** `nw_codec_identity` pins the EXACT bytes
  of every in-match encoder against committed FNV-1a64 vectors over a synthetic
  corpus (our bytes only, so it is committable and unconditional; it also runs in
  the Linux net job). It exists because encode↔decode round-trips cannot see a
  SYMMETRIC codec change — both sides edited together stay field-identical while
  the wire moves. Updating a vector is a wire-format change: it requires the
  [orig] witness or a D-NET entry in the same commit, never a bare regeneration
  (`nw_codec_identity_test --dump` prints the replacement table). The second
  tier-1 leg (NET-0b) is `nw_self_capture` — `nw_golden_diff`'s self mode: a
  deterministic in-process opennova↔opennova join + play session is captured
  live and coverage-diffed per (direction, tag) against the committed
  opennova-produced fixture `fixtures/novaworld/self-capture-session.pcap` (LFS;
  zero retail bytes, so committable). Because both sides are ours the comparison
  is EXACT set equality in both directions — no noise floor, no allowlists — and
  a missing fixture FAILS rather than skips. Regenerating the fixture
  (`nw_self_capture_test --write-fixture`, which re-reads and re-verifies the
  file) is a wire-coverage change: justify the tag delta in the same commit.
- **Tier 2 — local, mandatory protocol for net-touching PRs.** Run the retail
  golden diff (`nw_golden_diff_test --ours <capture>` against the
  `golden/retail-gameplay-session.pcapng` under `OPENNOVA_CAPTURES`) and the
  npruntime golden joins against local retail data, and **attest the run in the
  PR description** (the commands + PASS lines). A net-touching PR without the
  attestation is not reviewable. This is the standing substitute for the
  un-CI-able retail gates above.
