# Asset-gated tests — the roots, the matrix, local setup, and the CI stance

Some tests exercise data we cannot commit: retail game installs, extracted retail
assets, retail mission corpora, and network captures of retail sessions. Each such
test is gated on one of two documented roots and **reports Skipped** without it:
a fully gated ctest returns 77 (`opennova_add_gated_test` sets
`SKIP_RETURN_CODE`, so `ctest` prints `***Skipped`) after a `SKIP: needs ...`
line; a mixed test runs its synthetic legs and prints `SKIP-LEG: needs ...` for
the retail leg, exiting 0; the GUT gates `pending()`. A green run therefore
never hides an unexercised gate, but it also never proves the gate ran: when
touching a gated area, set the root and read the test's output.

The two roots (`docs/dev-env-vars.md`) are read only by the resolvers —
`tests/common/retail_paths.h` (`retail::install()`, `assets()`,
`reference_fixture(rel)`, `asset_file(name)`, `expansions()`, `weapon_sav()`, `skip`,
`skip_leg`, `RETAIL_REQUIRE_OR_SKIP`), `godot/tests/support/retail_data.gd`
(`RetailData.install()`, `assets()`, `expansions()`,
`mount_install_with(witness)`), and the
`Get-OpenNovaRetail*` getters in `scripts/net/lib.ps1`. Machine paths live in
`.claude/settings.local.json` `env` (untracked), never in tracked files.

## The matrix

| Root | Points at | Gates |
|---|---|---|
| `OPENNOVA_JO_DIR` | a packed retail JO install (the `.pff` set; expansions under `expansion/<name>/`) | ctest `rtxt_jo_install_sweep`, `env_jo_install`, `bink_retail`, `sbf_jo_install_sweep`, `mission_ai_path_conformance`, `truck_dismount`, `npruntime_authored_payload_00trg` (00TRg through revx02, or the `OPENNOVA_JO_ASSETS` tree); the mission-kernel ctests (`mission::MissionKernel` over `tests/common/retail_mission_files`) `ai_threat`, `ladder_00tra`, `truck_rest_00tra`, `ai_muzzle_pose` (CP01), `vehicle_ride_00tra`, `watercraft_02tr` (authored hull contact retention and zero-height water support), `defense_00trg`, `lose_flow_04tr`, `lose_flow_00tra`, `particle_gore_set_catalog`, `minefield_retail` (`00TRd` and `CP09` field binding, trigger, damage and reset); the SKIP-LEG legs of `mnu_compat` (every `.mnu` the packed install serves, base mount and each expansion), `terrain_tile_composer` (iterates `expansions()` for the CP12/00TRa tile witnesses), `ground_conform` (the CP01 standing leg), `score_roundtrip` (the `score.ini` the install ships loose beside its archives), `playersav_weapon_sav` (`weapon.sav` at the root or under an expansion), `minimap_overlay` (00TRg with revx02), `npruntime_weapon_table` (the live `weapon.def` oracle over the install's expansions); GUT `avatar_preview_test`, `e50trib_mount_alignment_test`, `skeletal_anim_test`, `sound_pff_install_test` (every expansion), `terrain_static_shadow_runtime_test`, `foliage_dispatcher_assets_test`, `vehicle_emplacement_alignment_test`, `virtual_display_present_test` (the installed M1A1 driver display), `hud_installed_assets_test` (authored launcher/mortar cards, impact cues and vehicle entry/exit; GPU runs write local `user://hud-installed-captures/` readbacks), `mounted_weapon_switch_test` (the designated-G alternate-gun switch on every candidate carrier the install authors it for, placed unoccupied in 07TR: the stock Escalation Apache and Ka-52, plus JOTAC's M1A1 and T80; an install with none fails), `mounted_view_test` (03TR mounted camera, muzzle and continuous NPC attachment during flight), `tank_parity_test` (installed M1A1/T80 driver, cannon and roof-gun seat selection, optical wheel signs and remapping, authored zoom clamps and hull HUD routing on 07TR; assisted placement/boarding), `tank_training_test` (07TR visible cannon prompt and USE boarding, then cannon combat and victory, with explicit positioning after the authored route for occluded respawns; installed revx02 when available, otherwise the base/expansion mount serving 07TR) |
| `OPENNOVA_JO_ASSETS` | an extracted retail asset tree (`items.def`, `weapon.def`, `ammo.def`, models, `.adm`, the shipped `.bms` missions loose at its root, `.til`, `.lwf`, ...) plus its `fixtures/` subtree: the retail-interop fixture set (`retail::reference_fixture(rel)` / `RetailData.fixture(rel)` resolve `<assets>/fixtures/<rel>`), the retail files whose in-tree copies this repository no longer carries | ctest `mission_corpus` (every loose `.bms`), `mnu_compat` and `mnu_coverage` (the fifteen shipped revx02 menus from the reference fixture set), `adm_parse` (mp5_1st.adm), `anim_skeletal_clips_weapon_channel` (BINOC.bad and its twist) and `def_parse_hudpos` (hudpos.def), `root_motion`, `anim_positions_from_model_corpus`, `anim_reload_clips_us01`, `anim_weapon_action_clips`, `wac_corpus` (plus argv corpus dirs), `cpt_jo_assets_sweep`, `lwf_jo_assets_sweep`, `npruntime_authored_payload_00trg` (00TRg; also served by the install's revx02); the mission-kernel ctests `ai_corpse`, `parachute_09tr` (unseated player and authored NPC boarding through helicopter takeoff), `rock_collision_00trg`, `soak_00trg`, `native_assets_00trg`, `npruntime_remote_body_state`, `npruntime_held_weapon_attach`; the SKIP-LEG legs of `occlusion_armry`, `particle_smoke_all_fixtures` (the `.ptl` corpus), `sound_profile` (`sndprof.def`), `def_parse_items` (the particlefx rows), `infantry` (the weapon-channel leg), `minimap_overlay` (00TRg), the loose-mission fallback of the `OPENNOVA_JO_DIR` mission-kernel tests; GUT `mission_corpus_binding_test`, `sound_dialog_test`, `sound_integration_test`, `mission_present_pass_test` (the Iblock01 door leg); the render-fixture capture's loose mission (`scripts/render/*.ps1`); over the `fixtures/` subtree (the reference fixture set) the SKIP-LEG legs of `dbf_roundtrip` (00TRg.DBF), `cbin_roundtrip` (the three shipped `nlist.kda`), `mission_mis_idempotency` (ash_i5b), `avatars_parse` and `avatars_roundtrip` (Avatars.def), `mns_document` (menu_style.mns), `bad_parse` and `anim_sample` (BINOC.bad), `def_parse_weapons`, `def_parse_ammo`, `npruntime_weapon_table` and `npruntime_handshake_server` (the shipped weapon.def / ammo.def pins), `mus_parse`, `mus_compat`, `mus_decompile`, `mus_roundtrip`, `mus_names_roundtrip`, `mus_entry_roundtrip`, `mus_encode_idempotence` and `mus_vm` (jo_gamemus.bin, jo_menumus.bin and the decoded golden: the MDEdit layout pins, the decompile golden and the Unicorn-proved VM streams), the menu-driven GUT scripts `armory_menu_seam_test`, `armory_presenter_test`, `deploy_screen_presenter_test`, `host_punt_surfacing_test`, `menu_shell_test`, `mnu_corpus_test`, `hud_pos_test`, `throwable_repro_test`, `wire_present_pass_test`, `local_player_presenter_test`, `coop_two_sim_test`, `wire_header_world_materialization_test` (whole scripts, `should_skip_script`; the def scripts stage the shipped weapon.def / ammo.def / hudpos.def through `RetailData.def_root()`) and the legs `game_world_test` (the armory weapon-database reuse), `loading_screen_test` (the session-variable overlay), `avatar_preview_test` (the retail-root portrait legs compose the shipped table's parts) and `render_fixture_capture_probe_test` (the arms blue's 0x0402 wears in the shipped table), `mnu_document_test` (jo_main's 800x600 canvas, the shipped style sheet's entries and edits), `player_info_menu_seam_test` (the two legs over the shipped player.mnu) `main_game_lifecycle_test` (the ESC/armory screen verbs over the shipped game.mnu / weapon.mnu) `simulation_test` (the seven posed-collision rig tests over BINOC.bad and the sixteen weapon-table tests over the shipped defs), `player_info_menu_seam_test` (the loadout legs), `hud_overlay_test`, `hud_helpers_test`, `player_weapon_view_test`, `listen_server_test`, `mission_root_test` and `weapon_profile_kit_test` (their shipped-def legs), `npc_attention_test` and `teammate_spawn_test` (BINOC.bad) |

Developer knobs are argv, not roots: `mnu_compat_test <extra.mnu>...`,
`wac_corpus_test <dir>...`, `ai_path_conformance_test --report/--ticks/--bms`;
dumps are `--dump`, `--write`,
`--write-fixture`, `--write-pff` (`docs/dev-env-vars.md`).

The 00TRa tile-composer leg fingerprints the archived `TRNTILE10.TGA`
payload (`SHA-256 eb3b25ca50f66f2006668198919c8e25374d093c0290e9aceb613ee37d8bc490`)
and pins the correct shipped-output RGB hashes for entries 761/781. This
guards against repeating the 2026-08-21 test-only re-pin that left the
asset-gated test permanently red without changing the renderer.

Runtime probes (`game_probe` tools under `godot/probes/`, `docs/mcp.md`) take
their retail roots as typed arguments (`mission_path`, `mission_resource_dir`,
`output_dir`, ...) or from the launch's `--resource-dir`; they read no environment
variable. The scripts that drive them default those arguments to the two roots.

## Local setup

Add to `.claude/settings.local.json` (adjust to this machine's paths):

```json
{
  "env": {
    "OPENNOVA_JO_DIR": "C:/Users/<you>/Desktop/Games/Joint Operations Combined Arms",
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

A packed install without an expansion the test needs (revx02 for the 00TRg
payload oracle) skips that leg; the extracted tree carries the same pair and
serves it. No test reads a machine-local capture: the wire coverage that used
to ride gitignored `.scratch/` pcaps is the in-tree `fixtures/novaworld/` set
(`nw_self_capture`, `nw204_lobby_decode`, the `.nwmsg` replays) plus the
inline-pcap unit tests.

Run the gated set with the roots exported, e.g. `ctest --test-dir build -C
Release -R "00tra|00trg|ai_|muzzle|reload_clips|weapon_action|remote_body|held_weapon|authored_payload|gore_set"`,
and read the output: a real run prints its measurements, a skipped one prints
`SKIP:`.

## CI

Retail data never reaches public runners as tracked files (copyright, size,
credentials). Two private repositories carry what CI needs:
`opennova-net/opennova-reference-assets` (the extracted tree behind
`OPENNOVA_JO_ASSETS`: the loose asset set, the shipped `.bms` missions and the
reference fixture set; its README lists the files) and `opennova-net/opennova-reference-retail-packed` (the packed install
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
the two roots at them, so every root-gated test runs in CI instead of
reporting Skipped; without the secret the gates stay closed and the job is
still green. `.github/workflows/ci.yml` is the record.

A green run still never proves a gate opened, so with the data mounted both
jobs end by attesting it: `scripts/ci/retail_gates_ran.py` reads ctest's JUnit
report (`scripts/build.sh` writes `build/Testing/ctest.xml`) and the GUT log,
and fails on any fully gated test that reported Skipped or any `SKIP-LEG:` /
`[Pending]` line that names a mounted root. Its expectation tables are this
page's matrix; a new gated test is added to both, and
`retail_gates_ran.py --check-docs` (the lint job, and again after the build
with `--junit` for the full ctest universe) fails when a root's table and its
row here name different ctests. With the data mounted nothing is exempt: the
`KNOWN_ABSENT` table is empty and every Skipped gate is a gap.

The wire logic the retired capture gates exercised is covered in CI by the
**inline-pcap unit tests** (`nw_pool_decode_unit_test`,
`nw_capture_decoder_test` craft tiny in-memory pcaps and run unconditionally)
and the committed `fixtures/novaworld/` replays — the sanctioned substitute,
per the net-test convention.

## Why captures are never committed

Beyond the blanket rule for retail data, **NovaWorld traffic can embed the account
username and password** (the gate/auth exchanges), and `.sph` recordings carry
account identity and machine paths. Treat every capture as credential-bearing
until proven otherwise. The tracked-fixture line is drawn in `.agents/interop.md`:
promote only small *sanitized* artifacts (`.nwmsg`, focused `.hexcap`, `.gsb`,
manifest rows) — `fixtures/novaworld/` shows the shape. Raw `.pcapng`/`.sph` stay
in gitignored `.scratch/`, full stop.

## The two-tier wire-compat gate (maturity program NET-0)

CI can never run a live retail session, so a net-touching change could look
green while silently altering wire bytes. The maturity program
(docs/maturity-program.md) closed that with two tiers (the original tier-2
retail golden diff retired with the capture root, 2026-08-29 — the live
harness below replaced it):

- **Tier 1 — default CI, cannot skip.** `nw_codec_identity` pins the EXACT bytes
  of every in-match encoder against committed FNV-1a64 vectors over a synthetic
  corpus (our bytes only, so it is committable and unconditional; it also runs in
  the Linux net job). It exists because encode↔decode round-trips cannot see a
  SYMMETRIC codec change — both sides edited together stay field-identical while
  the wire moves. Updating a vector is a wire-format change: it requires the
  [orig] witness or a D-NET entry in the same commit, never a bare regeneration
  (`nw_codec_identity_test --dump` prints the replacement table). The second
  tier-1 leg (NET-0b) is `nw_self_capture` — the opennova-vs-opennova coverage diff: a
  deterministic in-process opennova↔opennova join + play session is captured
  live and coverage-diffed per (direction, tag) against the committed
  opennova-produced fixture `fixtures/novaworld/self-capture-session.pcap` (LFS;
  zero retail bytes, so committable). Because both sides are ours the comparison
  is EXACT set equality in both directions — no noise floor, no allowlists — and
  a missing fixture FAILS rather than skips. Regenerating the fixture
  (`nw_self_capture_test --write-fixture`, which re-reads and re-verifies the
  file) is a wire-coverage change: justify the tag delta in the same commit.
- **Tier 2 — local, mandatory protocol for net-touching PRs.** Run the live
  retail interop harness (`scripts/net/README.md`: an OpenNova host with a
  retail client, and a retail host with an OpenNova joiner, decoded with
  `nw_pp --coverage`), and **attest the run in the PR description** (the
  commands + the decode summary). A net-touching PR without the attestation
  is not reviewable. This is the standing substitute for the retail sessions
  CI cannot run.
