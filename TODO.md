# TODO

Everything here is work that is **not** a parity divergence: code hardening and project
health. Divergences from the original engine belong in
[docs/divergence-ledger.md](docs/divergence-ledger.md) instead, and
[docs/current-state.md](docs/current-state.md) explains which is which.

## Cleanup & verification backlog

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
      protocol 1.8+). Open: optionally an `onhook_exercise_input` tool to retire
      `exercise_retail_input.ps1`. Baseline: the 2026-08-05 suites captured 24/24 cells
      cleanly, wire-ready + the since-retired `diff_vs_golden.ps1` (ca1cef465) GREEN.

## Project health follow-ups

- [ ] Formatting enforcement: `scripts/lint/format_lint.py` checks the lines a change adds (clang-format, ruff, gdlint/gdformat; CI's `format-lint` job, advisory). Remaining: the reformat commits (C++ one per group, whitespace-only, checked by rebuilding and comparing objects; Python and GDScript per tree) plus `.git-blame-ignore-revs`, at a moment the maintainer picks since they conflict with open branches; then `format-lint` runs with `--enforce` as a required check.
