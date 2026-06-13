---
name: extract-pr
description: Slices a long-lived worktree mega-branch into reviewable, dependency-ordered PRs, each gated on this repo's green bar. Use when asked to extract or split a branch into PRs, run a merge train, or land part of a worktree's work.
---

# Extract reviewable PRs from a mega-branch

The recurring shape: a worktree accumulates a large diff vs `master`; it lands
as a train of small, single-topic PRs, each green before the next is cut.
All commands are Git Bash, inside the current worktree only.

## 1. Inventory and slice

- `git diff master --stat` (and `git log master.. --oneline`) to see the full
  surface. Group files into cohesive single-topic slices — one subsystem, seam,
  or behavior per PR; a slice should be reviewable in one sitting.
- Dependency-order the slices (shared lib/plumbing changes first, consumers
  after). Write the slice list down before cutting anything.

## 2. Cut a slice

- Branch from current `master` with a short topic name (existing convention:
  `b6-detachable-panels`, `c12-pie-overlay` — a few words, no deep paths).
- Bring changes over by `git checkout <mega-branch> -- <paths>` or cherry-pick,
  whichever yields a cleaner minimal diff. Re-read the resulting diff: a slice
  must not smuggle unrelated hunks that ride along in shared files.
- LFS hazard: anything under `fixtures/**` must land as LFS pointers — after
  staging, check `git lfs status` and the staged diff (a pointer file is a few
  lines of text; megabytes of binary in the diff means LFS missed it).
- Edit only inside the worktree.

## 3. Local green bar (before every push)

    BUILD_GODOT=0 bash scripts/build.sh      # C++ + full ctest
    bash scripts/build_godot.sh              # GDExtension (if godot/ touched)
    bash scripts/test_godot.sh               # GUT (use the gut skill's GODOT_BIN setup)
    bash scripts/test_python.sh              # pytest (if Python touched)

Scope ctest with `-R` while iterating, but the pre-push run is the full suite.

## 4. Open the PR

- Push, then `gh pr create` with the context in the PR description (what the
  slice does, what it deliberately excludes, what depends on it). Never post PR
  comments. Do not merge — the maintainer merges.
- CI gate: `test`, `godot-tests`, and `build-gdextension-windows/macos` must be
  green. A red `modsuperoed-smoke` is known-unrelated OED parity drift — never
  block or report on it.

## 5. Advance the train

- Stop at "PR open + green + summarized" and report the slice status plus
  what's next in the train.
- After the user merges a slice: rebase the mega-branch (or the next slice
  branch) onto updated `master`, re-verify the next slice's green bar, repeat.
- Keep a running tally (landed / open / remaining slices) so any session can
  resume the train from the worktree.
