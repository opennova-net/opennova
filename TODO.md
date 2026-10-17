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
      delete the five Actions secrets on revx02/onjo01/ondx01 (the repos stay).

## Project health follow-ups

- [ ] Formatting enforcement: `scripts/lint/format_lint.py` checks the lines a change adds (clang-format, ruff, gdlint/gdformat; CI's `format-lint` job, advisory). Remaining: the reformat commits (C++ one per group, whitespace-only, checked by rebuilding and comparing objects; Python and GDScript per tree) plus `.git-blame-ignore-revs`, at a moment the maintainer picks since they conflict with open branches; then `format-lint` runs with `--enforce` as a required check.
