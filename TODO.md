# TODO

Everything here is work that is **not** a parity divergence: code hardening and project
health. Divergences from the original engine belong in
[docs/divergence-ledger.md](docs/divergence-ledger.md) instead, and
[docs/current-state.md](docs/current-state.md) explains which is which.

## Cleanup & verification backlog

- [ ] `mission_playthrough` probe gates 8 to 11 (`godot/probes/runtime/mission_playthrough_probe.gd`)
      encode the refuted 00TRa friendly-fire failure: gate 8 fires at the instructor until the
      round ends, and gates 9 to 11 leave through the end screen it expects. Retail protects
      every same-team pair out of a session (D-WPN-42, D-NET-333; world-wac-ai-re.md §40.5), so
      gate 8 becomes the protected-pair pin (hits land on the instructor, no damage, the round
      live) and gates 9 to 11 route through the in-game menu's exit and RESTART instead.
      Acceptance: a `mission_playthrough` run on 00TRa through the game MCP passes 11 of 11.
- [ ] Apply the launcher and expansion-distribution retirement (the config left the repo
      with `infra/github`, `on-deploy github` and the downloads/`launcher_ci`/`nw` blocks;
      the PR body has the commands): drop `downloads_bucket_name` and `create_ci_user` from
      the local `infra/aws/terraform.tfvars`; empty `downloads.opennova.net` (every version
      and delete marker); `./deploy/run.sh infra plan` then `apply` (it destroys the bucket
      set, CloudFront, the ACM cert and its validation record, the `downloads` CNAME, the
      `nw` record and `launcher_ci`, and turns the account-wide S3 public access block on);
      delete the `expansions-ci` and `github` items, the `tfstate-github` document and
      `app-prod`'s `expansion_publish_token` field in 1Password; delete the
      `LAUNCHER_AWS_*` secrets and the `DOWNLOADS_BUCKET` variable on this repo and the five
      Actions secrets on revx02/onjo01/ondx01 (the repos stay); then drop the
      `us_east_1` provider alias in `infra/aws/main.tf`.
- [ ] Retail-LAN parity four-topology verdict: the tracked 24-cell matrix harness
      (`run_parity_matrix.ps1`/`generate_parity_manifest.ps1`/`verify_parity_matrix.ps1`
      over `export_parity_corpus.py`) was retired with the Python FFI (ADR 0038; last
      at 5820432c1), so no PARITY_STATUS verdict exists. To produce one: run the four
      `scripts/net/run_parity_topology.ps1` cells (RR/RO/OR/OO) per case under one
      RunId prefix (`.agents/retail-lan-parity.md`), gate each on the wire-ready witness,
      and re-express the retired verifier's RO/OR/OO-vs-RR
      comparison (both directions, packet grouping, 0x0A/0x0C state) as a ctest or
      PowerShell verifier (ADR 0050 R4). Every cell runs on the pinned JO:CA reference
      (`jox01`) with retail stock under onHook's StockObserver (opennova-int, bridge
      protocol 1.8+). Open: a jox01 integrity-challenge profile (D-NET-181; with none,
      our host stays silent on CRC replies and our joiner does not answer a retail
      host's challenges), and optionally an `onhook_exercise_input` tool to retire
      `exercise_retail_input.ps1`. Baseline: the 2026-08-05 suites captured 24/24 cells
      cleanly, wire-ready + the since-retired `diff_vs_golden.ps1` (ca1cef465) GREEN.
- [ ] Precipitation floor-sampler cost (owner: `engine/runtime/environment/precipitation.*`
      + the `godot/src/env/precipitation.cpp` shell sampler): the witnessed per-wrap drop
      raycast (the retail floor probe from +200 m) measured ~0.4 ms/frame at rain 100 on
      03TR (frame_stats "env" row = WORLD_WEATHER, 2026-08-30) — batch or cache the
      entity-raycast half (the terrain/water half is cheap) without changing the
      witnessed floor semantics; re-measure via `perf_mission_rows`
- [ ] Environment/water frame-leg cost: re-measure at the ASH_I5A vantage before
      slicing. The env/water work runs as `GameWorld` frame legs
      (`godot/src/world/game_world_frame.cpp`: `environment_nodes`, `water`, and the
      `weather_settle` / `sky_settle` / `water_settle` tail), timed by the Stats window's
      `env_nodes` (`WORLD_ENV_NODES`) and `water` (`WORLD_WATER`) rows; the #403
      present-side rework (parked idle models, the staggered per-model light restamp)
      invalidated the earlier ~1 ms reading. The metric also survives as the
      `outside_shell` summary the `perf_mission_rows` runtime probe reports, and that
      is where the next attribution round reads it (the #403 live sessions observed the
      remaining frame cost concentrated outside the model system after the
      park/submission gates)
- [ ] NovaWorld client: a fatal session error mid-play ends in ERROR (owner: `godot/src/network/novaworld_client.cpp`): an `on_fatal` inside `lobby_.tick()` (`engine/net/novaworld/session/nwu_lobby_session.cpp`, the malformed-datagram leg) or the session's `S::Error` state enters `STATE_ERROR` without clearing `play_in_flight_` (only the `S::Closed` branch clears it), and the same `_process` call still runs `drain_session_notices()` and the play timeout. A rejected `PlayResult` or the timeout then calls `abort_playing`, which enters `STATE_CONNECTED` and emits `connected`: the NovaWorld panel re-arms Login on a session in error and the tick restarts. Fix: entering `STATE_ERROR` / `STATE_DISCONNECTED` clears `play_in_flight_`, or `abort_playing`, `resolve_join_target` and the host hooks never leave DISCONNECTED or ERROR. Acceptance: a fatal error mid-play leaves the client in ERROR with no `connected` emitted.
- [ ] Converge `engine/formats/cpt`'s bit WRITER on `engine/base/io/bit_stream.h` (owner: `engine/formats/cpt/cpt_io.cpp`; the reader already rides the shared `io::BitReader`): `io/bit_stream.h` carries no writer, and cpt's private `BitWriter` carries a normalizing `set_position` and a `write_to_file`, so this is a real migration (add the shared writer, then move cpt onto it), not a swap, which is why `engine/CLAUDE.md` tracks it as a named exception. Acceptance: `tests/cpt/cpt_roundtrip_test` (ctest `cpt_roundtrip`) stays green AND a by-hand retail-corpus byte diff still reports byte-identical CPT output after cpt drops its private writer; the corpus diff is not in ctest, so it has to be run by hand (`engine/CLAUDE.md` migration exceptions).

## Project health follow-ups

- [ ] Release-gate parity: make tag releases run the same required quality gates as PR/master CI, or reject release tags whose commit is not on `master`. Acceptance: an off-master tag cannot publish, and a valid release commit passes the shared maturity, native, and Godot gates.
- [ ] Incremental conventional linting (vocabulary conventions already ride `scripts/lint/conventions_lint.py` as a CI gate; this row is formatting + per-language linters): the C++ `.clang-format` is committed CONFIG ONLY (ADR 0043 d14), with the per-group whitespace-only reformat commits and `.git-blame-ignore-revs` a separate follow-up PR; GDScript/Python have no project formatting settings yet. Then add per-language lint checks in advisory or changed-file mode before enforcing them. Acceptance: CI checks new changes without requiring a repository-wide reformat, with documented local commands for each enabled linter.
