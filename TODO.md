# TODO

Everything here is work that is **not** a parity divergence: code hardening and project
health. Divergences from the original engine belong in
[docs/divergence-ledger.md](docs/divergence-ledger.md) instead, and
[docs/current-state.md](docs/current-state.md) explains which is which.

## Project health follow-ups

- [ ] Formatting enforcement: `scripts/lint/format_lint.py` checks the lines a change adds (clang-format, ruff, gdlint/gdformat; CI's `format-lint` job, advisory). Remaining: the reformat commits (C++ one per group, whitespace-only, checked by rebuilding and comparing objects; Python and GDScript per tree) plus `.git-blame-ignore-revs`, at a moment the maintainer picks since they conflict with open branches; then `format-lint` runs with `--enforce` as a required check.
